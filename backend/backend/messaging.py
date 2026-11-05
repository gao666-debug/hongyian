"""账号间消息：好友、会话（单聊/群聊）、文本/图片/语音、未读。

设计要点
--------
* 单聊与群聊统一用「会话」模型：单聊是 kind='direct' 的会话，避免以后加群聊时重构表。
* 消息**先落库再推送**。对方离线时消息躺在库里，用 last_read_at 算未读，上线拉取即可，
  所以不依赖系统级推送（Push Kit）也不会丢消息。
* 媒体走原始字节上传（不用 multipart），因此不引入 python-multipart 依赖。
* 媒体下载必须校验会话成员身份，不暴露裸 URL。
"""
import hashlib
import json
import os
import secrets
import time
import uuid
from datetime import datetime, timezone

from fastapi import Depends, HTTPException, Query, Request
from fastapi.responses import Response
from pydantic import BaseModel, ConfigDict, Field

MAX_GROUP_MEMBERS = 10
INVITE_TTL_SECONDS = 7 * 86400
MAX_TEXT = 4000
MAX_MEDIA_BYTES = 8 * 1024 * 1024
MAX_VOICE_SECONDS = 60
# 去掉易混字符 0/O/1/I
INVITE_ALPHABET = 'ABCDEFGHJKLMNPQRSTUVWXYZ23456789'

IMAGE_TYPES = {'image/jpeg': 'jpg', 'image/png': 'png'}
VOICE_TYPES = {'audio/mp4': 'm4a', 'audio/m4a': 'm4a', 'audio/aac': 'aac',
               'audio/x-m4a': 'm4a', 'audio/amr': 'amr'}


class StrictModel(BaseModel):
    model_config = ConfigDict(extra='forbid')


class InviteAccept(StrictModel):
    code: str = Field(min_length=4, max_length=16)


class ContactPeer(StrictModel):
    peer: str = Field(min_length=1, max_length=64)


class GroupCreate(StrictModel):
    name: str = Field(min_length=1, max_length=32)
    members: list[str] = Field(min_length=1, max_length=MAX_GROUP_MEMBERS - 1)


class GroupRename(StrictModel):
    name: str = Field(min_length=1, max_length=32)


class MemberChange(StrictModel):
    username: str = Field(min_length=1, max_length=64)


class TextMessage(StrictModel):
    body: str = Field(min_length=1, max_length=MAX_TEXT)
    client_id: str | None = Field(default=None, max_length=128)


class MediaMessage(StrictModel):
    media_id: str = Field(min_length=1, max_length=64)
    client_id: str | None = Field(default=None, max_length=128)


def _now():
    return time.time()


def _iso(ts):
    if not ts:
        return None
    return datetime.fromtimestamp(ts, timezone.utc).isoformat().replace('+00:00', 'Z')


class Messaging:
    def __init__(self, store, settings):
        self.store = store
        self.settings = settings
        self.media_dir = os.path.join(os.path.dirname(settings.db), 'media')
        os.makedirs(self.media_dir, exist_ok=True)
        with store.connection() as db:
            db.executescript('''
            CREATE TABLE IF NOT EXISTS contacts(
                owner TEXT NOT NULL, peer TEXT NOT NULL, status TEXT NOT NULL,
                created REAL NOT NULL, accepted REAL,
                PRIMARY KEY(owner, peer));
            CREATE TABLE IF NOT EXISTS conversations(
                id TEXT PRIMARY KEY, kind TEXT NOT NULL, name TEXT, owner TEXT NOT NULL,
                pair_key TEXT UNIQUE, created REAL NOT NULL);
            CREATE TABLE IF NOT EXISTS conversation_members(
                conversation_id TEXT NOT NULL, username TEXT NOT NULL,
                joined REAL NOT NULL, last_read_at REAL NOT NULL DEFAULT 0,
                PRIMARY KEY(conversation_id, username));
            CREATE TABLE IF NOT EXISTS messages(
                id TEXT PRIMARY KEY, conversation_id TEXT NOT NULL, sender TEXT NOT NULL,
                kind TEXT NOT NULL, body TEXT, duration REAL, created REAL NOT NULL,
                client_id TEXT UNIQUE);
            CREATE TABLE IF NOT EXISTS media(
                id TEXT PRIMARY KEY, conversation_id TEXT NOT NULL, owner TEXT NOT NULL,
                filename TEXT NOT NULL, content_type TEXT NOT NULL, size INTEGER NOT NULL,
                duration REAL, created REAL NOT NULL);
            CREATE INDEX IF NOT EXISTS msg_conv ON messages(conversation_id, created);
            CREATE INDEX IF NOT EXISTS member_user ON conversation_members(username);
            ''')
            db.commit()

    def _db(self):
        return self.store.connection()

    def user_exists(self, username):
        with self._db() as db:
            return db.execute('SELECT 1 FROM users WHERE name=?', (username,)).fetchone() is not None

    def are_friends(self, a, b):
        with self._db() as db:
            row = db.execute(
                'SELECT 1 FROM contacts WHERE owner=? AND peer=? AND status=? '
                'UNION SELECT 1 FROM contacts WHERE owner=? AND peer=? AND status=?',
                (a, b, 'accepted', b, a, 'accepted')).fetchone()
        return row is not None

    def make_invite(self, owner):
        expires = _now() + INVITE_TTL_SECONDS
        with self._db() as db:
            db.execute('BEGIN IMMEDIATE')
            for _ in range(20):
                code = ''.join(secrets.choice(INVITE_ALPHABET) for _ in range(6))
                if not db.execute('SELECT 1 FROM documents WHERE mode=? AND kind=? AND key=?',
                                  (self.settings.mode, 'invite_code', code)).fetchone():
                    break
            else:
                raise HTTPException(503, 'Please retry creating invite')
            db.execute('INSERT INTO documents(mode,device,kind,key,body,updated) '
                       'VALUES(?,?,?,?,?,?)',
                       (self.settings.mode, owner, 'invite_code', code,
                        json.dumps({'owner': owner, 'expires': expires, 'used_by': None}),
                        _now()))
            db.commit()
        return {'code': code, 'expires_at': _iso(expires), 'ttl_seconds': INVITE_TTL_SECONDS}

    def accept_invite(self, code, user):
        code = (code or '').strip().upper()
        with self._db() as db:
            db.execute('BEGIN IMMEDIATE')
            row = db.execute('SELECT device,body FROM documents WHERE mode=? AND kind=? AND key=?',
                             (self.settings.mode, 'invite_code', code)).fetchone()
            if not row:
                raise HTTPException(404, 'Invite code not found')
            data = json.loads(row['body'])
            owner = data.get('owner') or row['device']
            if owner == user:
                raise HTTPException(422, 'Cannot add yourself')
            if data.get('used_by'):
                raise HTTPException(409, 'Invite code already used')
            if _now() > float(data.get('expires') or 0):
                raise HTTPException(410, 'Invite code expired')
            if not self.user_exists(owner):
                raise HTTPException(404, 'Invite owner no longer exists')
            if self.are_friends(owner, user):
                raise HTTPException(409, 'Already friends')
            now = _now()
            db.execute('INSERT OR REPLACE INTO contacts VALUES(?,?,?,?,?)',
                       (owner, user, 'accepted', data.get('created') or now, now))
            db.execute('INSERT OR REPLACE INTO contacts VALUES(?,?,?,?,?)',
                       (user, owner, 'accepted', now, now))
            data['used_by'] = user
            db.execute('UPDATE documents SET body=?,updated=? WHERE mode=? AND kind=? AND key=?',
                       (json.dumps(data), now, self.settings.mode, 'invite_code', code))
            db.commit()
        return {'peer': owner, 'status': 'accepted'}

    def list_contacts(self, user):
        with self._db() as db:
            rows = db.execute('SELECT peer,accepted FROM contacts WHERE owner=? AND status=? '
                              'ORDER BY accepted DESC', (user, 'accepted')).fetchall()
        return [{'username': r['peer'], 'since': _iso(r['accepted'])} for r in rows]

    def remove_contact(self, user, peer):
        with self._db() as db:
            n = db.execute('DELETE FROM contacts WHERE (owner=? AND peer=?) OR (owner=? AND peer=?)',
                           (user, peer, peer, user)).rowcount
            db.commit()
        if not n:
            raise HTTPException(404, 'Contact not found')
        return {'removed': peer}

    def _conversation(self, db, cid):
        row = db.execute('SELECT * FROM conversations WHERE id=?', (cid,)).fetchone()
        if not row:
            raise HTTPException(404, 'Conversation not found')
        return row

    def require_member(self, cid, user):
        with self._db() as db:
            row = db.execute('SELECT 1 FROM conversation_members WHERE conversation_id=? AND username=?',
                             (cid, user)).fetchone()
        if not row:
            raise HTTPException(403, 'Not a member of this conversation')

    def is_group_owner(self, cid, user):
        self.require_member(cid, user)
        with self._db() as db:
            row = db.execute('SELECT owner,kind FROM conversations WHERE id=?', (cid,)).fetchone()
        return bool(row) and row['kind'] == 'group' and row['owner'] == user

    def direct(self, user, peer):
        if peer == user:
            raise HTTPException(422, 'Cannot chat with yourself')
        if not self.user_exists(peer):
            raise HTTPException(404, 'Peer not found')
        if not self.are_friends(user, peer):
            raise HTTPException(403, 'Not friends yet')
        pair = '|'.join(sorted([user, peer]))
        with self._db() as db:
            db.execute('BEGIN IMMEDIATE')
            row = db.execute('SELECT id FROM conversations WHERE pair_key=?', (pair,)).fetchone()
            if row:
                cid = row['id']
            else:
                cid = uuid.uuid4().hex
                now = _now()
                db.execute('INSERT INTO conversations VALUES(?,?,?,?,?,?)',
                           (cid, 'direct', None, user, pair, now))
                for name in (user, peer):
                    db.execute('INSERT OR IGNORE INTO conversation_members VALUES(?,?,?,?)',
                               (cid, name, now, 0))
                db.commit()
        return self._conversation_view(user, cid)

    def create_group(self, owner, name, members):
        if not name.strip():
            raise HTTPException(422, 'Group name required')
        members = [m for m in dict.fromkeys(members) if m != owner]
        if not members:
            raise HTTPException(422, 'Group needs at least one other member')
        if len(members) + 1 > MAX_GROUP_MEMBERS:
            raise HTTPException(422, 'Group exceeds %d members' % MAX_GROUP_MEMBERS)
        for m in members:
            if not self.user_exists(m):
                raise HTTPException(404, 'Member %s not found' % m)
            if not self.are_friends(owner, m):
                raise HTTPException(403, 'Member %s is not a friend' % m)
        cid = uuid.uuid4().hex
        now = _now()
        with self._db() as db:
            db.execute('INSERT INTO conversations VALUES(?,?,?,?,?,?)',
                       (cid, 'group', name.strip(), owner, None, now))
            for m in [owner] + members:
                db.execute('INSERT OR IGNORE INTO conversation_members VALUES(?,?,?,?)',
                           (cid, m, now, 0))
            db.commit()
        return self._conversation_view(owner, cid)

    def rename_group(self, cid, user, name):
        if not name.strip():
            raise HTTPException(422, 'Group name required')
        if not self.is_group_owner(cid, user):
            raise HTTPException(403, 'Owner required')
        with self._db() as db:
            db.execute('UPDATE conversations SET name=? WHERE id=?', (name.strip(), cid))
            db.commit()
        return self._conversation_view(user, cid)

    def add_member(self, cid, actor, username):
        if not self.is_group_owner(cid, actor):
            raise HTTPException(403, 'Owner required')
        if not self.user_exists(username):
            raise HTTPException(404, 'Member not found')
        with self._db() as db:
            db.execute('BEGIN IMMEDIATE')
            conv = self._conversation(db, cid)
            if conv['owner'] != actor or not db.execute(
                    'SELECT 1 FROM conversation_members WHERE conversation_id=? AND username=?',
                    (cid, actor)).fetchone():
                raise HTTPException(403, 'Owner required')
            count = db.execute('SELECT count(*) c FROM conversation_members WHERE conversation_id=?',
                               (cid,)).fetchone()['c']
            existing = db.execute('SELECT 1 FROM conversation_members WHERE conversation_id=? AND username=?',
                                  (cid, username)).fetchone()
            if count >= MAX_GROUP_MEMBERS and not existing:
                raise HTTPException(422, 'Group is full')
            db.execute('INSERT OR IGNORE INTO conversation_members VALUES(?,?,?,?)',
                       (cid, username, _now(), 0))
            db.commit()
        return self._conversation_view(actor, cid)

    def remove_member(self, cid, actor, username):
        with self._db() as db:
            db.execute('BEGIN IMMEDIATE')
            conv = self._conversation(db, cid)
            if not db.execute('SELECT 1 FROM conversation_members WHERE conversation_id=? AND username=?',
                              (cid, actor)).fetchone():
                raise HTTPException(403, 'Not a member of this conversation')
            if conv['kind'] != 'group':
                raise HTTPException(422, 'Only group members can be removed')
            if username == conv['owner']:
                raise HTTPException(409, 'Group owner cannot leave before ownership transfer is supported')
            if actor != username and conv['owner'] != actor:
                raise HTTPException(403, 'Owner required')
            db.execute('DELETE FROM conversation_members WHERE conversation_id=? AND username=?',
                       (cid, username))
            db.commit()
        return {'removed': username, 'conversation_id': cid}

    def list_conversations(self, user):
        with self._db() as db:
            rows = db.execute(
                'SELECT c.id,c.kind,c.name,c.owner,m.last_read_at FROM conversations c '
                'JOIN conversation_members m ON m.conversation_id=c.id WHERE m.username=? '
                'ORDER BY c.created DESC', (user,)).fetchall()
        out = []
        for row in rows:
            view = self._conversation_view(user, row['id'], last_read_at=row['last_read_at'])
            out.append(view)
        out.sort(key=lambda v: v.get('last_message_at') or '', reverse=True)
        return out

    def _conversation_view(self, user, cid, last_read_at=None):
        with self._db() as db:
            conv = self._conversation(db, cid)
            if last_read_at is None:
                row = db.execute('SELECT last_read_at FROM conversation_members '
                                 'WHERE conversation_id=? AND username=?', (cid, user)).fetchone()
                last_read_at = row['last_read_at'] if row else 0
            members = [r['username'] for r in db.execute(
                'SELECT username FROM conversation_members WHERE conversation_id=? '
                'ORDER BY joined', (cid,))]
            last = db.execute('SELECT * FROM messages WHERE conversation_id=? '
                              'ORDER BY created DESC LIMIT 1', (cid,)).fetchone()
            unread = db.execute('SELECT count(*) c FROM messages WHERE conversation_id=? '
                                'AND sender<>? AND created>?',
                                (cid, user, float(last_read_at or 0))).fetchone()['c']
        if conv['kind'] == 'direct':
            peer = next((m for m in members if m != user), None)
            title = peer
        else:
            peer = None
            title = conv['name'] or '群聊'
        return {'conversation_id': cid, 'kind': conv['kind'], 'title': title,
                'name': conv['name'], 'owner': conv['owner'], 'peer': peer,
                'members': members, 'member_count': len(members),
                'unread': unread,
                'last_message': self._message_view(last) if last else None,
                'last_message_at': _iso(last['created']) if last else _iso(conv['created'])}

    def _message_view(self, row):
        return {'id': row['id'], 'conversation_id': row['conversation_id'],
                'sender': row['sender'], 'kind': row['kind'], 'body': row['body'],
                'duration': row['duration'], 'created_at': _iso(row['created'])}

    def send_text(self, cid, user, body, client_id=None):
        self.require_member(cid, user)
        return self._insert_message(cid, user, 'text', body, None, client_id)

    def send_media(self, cid, user, media_id, client_id=None):
        self.require_member(cid, user)
        with self._db() as db:
            row = db.execute('SELECT * FROM media WHERE id=?', (media_id,)).fetchone()
            if not row:
                raise HTTPException(404, 'Media not found')
            if row['conversation_id'] != cid:
                raise HTTPException(422, 'Media belongs to another conversation')
        # 媒体消息的 kind 由内容类型推断，调用方不需要再传
        kind = 'image' if str(row['content_type']).startswith('image/') else 'voice'
        return self._insert_message(cid, user, kind, media_id, row['duration'], client_id)

    def _insert_message(self, cid, user, kind, body, duration, client_id):
        mid = uuid.uuid4().hex
        now = _now()
        # The database has a global UNIQUE key: namespace it by sender AND
        # conversation, so a guessed retry key cannot reveal another chat.
        request_key = ('v2:' + hashlib.sha256(json.dumps(
            [cid, user, client_id], ensure_ascii=False).encode()).hexdigest()) if client_id else None
        with self._db() as db:
            db.execute('BEGIN IMMEDIATE')
            if not db.execute('SELECT 1 FROM conversation_members WHERE conversation_id=? AND username=?',
                              (cid, user)).fetchone():
                raise HTTPException(403, 'Not a member of this conversation')
            conv = self._conversation(db, cid)
            if conv['kind'] == 'direct':
                peer = db.execute('SELECT username FROM conversation_members WHERE conversation_id=? AND username<>?',
                                  (cid, user)).fetchone()
                if not peer or not db.execute("SELECT 1 FROM contacts WHERE owner=? AND peer=? AND status='accepted'",
                                              (user, peer['username'])).fetchone():
                    raise HTTPException(403, 'Not friends yet')
            if client_id:
                existing = db.execute('SELECT * FROM messages WHERE client_id=?',
                                      (request_key,)).fetchone()
                if not existing:
                    # Preserve retries for 1.9.0 messages without trusting a
                    # global legacy key owned by another sender/conversation.
                    existing = db.execute('SELECT * FROM messages WHERE client_id=? AND conversation_id=? AND sender=?',
                                          (client_id, cid, user)).fetchone()
                if existing:
                    if (existing['conversation_id'], existing['sender'], existing['kind'],
                            existing['body'], existing['duration']) != (cid, user, kind, body, duration):
                        raise HTTPException(409, 'Retry key already used for a different message')
                    return self._message_view(existing)
            # Keep timestamp cursors strictly ordered even for clock ties/rollback.
            latest = db.execute('SELECT max(created) value FROM messages WHERE conversation_id=?', (cid,)).fetchone()['value']
            now = max(now, (latest or 0) + 0.000001)
            db.execute('INSERT INTO messages VALUES(?,?,?,?,?,?,?,?)',
                       (mid, cid, user, kind, body, duration, now, request_key))
            # 发送者自己的未读水位推进到最后一条，避免自己发的算未读
            db.execute('UPDATE conversation_members SET last_read_at=? '
                       'WHERE conversation_id=? AND username=?', (now, cid, user))
            db.commit()
            row = db.execute('SELECT * FROM messages WHERE id=?', (mid,)).fetchone()
        return self._message_view(row)

    def history(self, cid, user, before=None, limit=30):
        self.require_member(cid, user)
        limit = max(1, min(int(limit), 100))
        with self._db() as db:
            if before:
                rows = db.execute('SELECT * FROM messages WHERE conversation_id=? AND created<? '
                                  'ORDER BY created DESC LIMIT ?', (cid, float(before), limit)).fetchall()
            else:
                rows = db.execute('SELECT * FROM messages WHERE conversation_id=? '
                                  'ORDER BY created DESC LIMIT ?', (cid, limit)).fetchall()
        items = [self._message_view(r) for r in reversed(rows)]
        return {'items': items, 'limit': limit,
                'has_more': len(items) == limit,
                'next_before': rows[-1]['created'] if len(items) == limit and rows else None}

    def mark_read(self, cid, user):
        self.require_member(cid, user)
        with self._db() as db:
            last = db.execute('SELECT max(created) m FROM messages WHERE conversation_id=?',
                              (cid,)).fetchone()['m']
            db.execute('UPDATE conversation_members SET last_read_at=? '
                       'WHERE conversation_id=? AND username=?',
                       (float(last or _now()), cid, user))
            db.commit()
        return {'conversation_id': cid, 'read_at': _iso(last)}

    def unread_total(self, user):
        with self._db() as db:
            row = db.execute(
                'SELECT count(*) c FROM messages msg '
                'JOIN conversation_members m ON m.conversation_id=msg.conversation_id '
                'WHERE m.username=? AND msg.sender<>? AND msg.created>m.last_read_at',
                (user, user)).fetchone()
        return {'unread': row['c']}

    def members_of(self, cid):
        with self._db() as db:
            return [r['username'] for r in db.execute(
                'SELECT username FROM conversation_members WHERE conversation_id=?', (cid,))]

    def save_media(self, cid, user, raw, content_type, duration):
        self.require_member(cid, user)
        content_type = (content_type or '').split(';')[0].strip().lower()
        if content_type in IMAGE_TYPES:
            kind, ext = 'image', IMAGE_TYPES[content_type]
        elif content_type in VOICE_TYPES:
            kind, ext = 'voice', VOICE_TYPES[content_type]
        else:
            raise HTTPException(415, 'Unsupported media type')
        if not raw:
            raise HTTPException(422, 'Empty media')
        if len(raw) > MAX_MEDIA_BYTES:
            raise HTTPException(413, 'Media too large')
        if kind == 'image':
            valid = raw[:2] == b'\xff\xd8' if ext == 'jpg' else raw[:8] == b'\x89PNG\r\n\x1a\n'
            if not valid:
                raise HTTPException(415, 'Not a JPEG/PNG image')
            duration = None
        else:
            valid = (raw[4:8] == b'ftyp' if ext == 'm4a' else
                     raw[:6] == b'#!AMR\n' if ext == 'amr' else
                     len(raw) >= 7 and raw[0] == 0xff and (raw[1] & 0xf6) == 0xf0)
            if not valid:
                raise HTTPException(415, 'Not a recognised audio container')
            if duration is None:
                raise HTTPException(422, 'Voice duration required')
            if not (1 <= float(duration) <= MAX_VOICE_SECONDS):
                raise HTTPException(422, 'Voice duration must be 1~%d seconds' % MAX_VOICE_SECONDS)
            duration = float(duration)
        media_id = uuid.uuid4().hex
        filename = '%s.%s' % (media_id, ext)
        path = os.path.join(self.media_dir, filename)
        with open(path, 'wb') as fh:
            fh.write(raw)
        with self._db() as db:
            db.execute('INSERT INTO media VALUES(?,?,?,?,?,?,?,?)',
                       (media_id, cid, user, filename, content_type, len(raw), duration, _now()))
            db.commit()
        return {'id': media_id, 'kind': kind, 'size': len(raw),
                'duration': duration, 'content_type': content_type}

    def open_media(self, media_id, user):
        with self._db() as db:
            row = db.execute('SELECT * FROM media WHERE id=?', (media_id,)).fetchone()
            if not row:
                raise HTTPException(404, 'Media not found')
            member = db.execute('SELECT 1 FROM conversation_members '
                                'WHERE conversation_id=? AND username=?',
                                (row['conversation_id'], user)).fetchone()
        if not member:
            raise HTTPException(403, 'Not a member of this conversation')
        path = os.path.join(self.media_dir, row['filename'])
        if not os.path.isfile(path):
            raise HTTPException(410, 'Media file missing')
        with open(path, 'rb') as fh:
            return row['content_type'], fh.read()

    def install(self, app, session, envelope, notify):
        """注册路由。notify(username, event) 是 async，负责推到该账号的所有在线连接。"""
        svc = self

        async def fanout(cid, sender, message):
            event = {'schema': 1, 'type': 'message', 'conversation_id': cid,
                     'timestamp': message.get('created_at'),
                     'data': message}
            for name in svc.members_of(cid):
                # Also sync the sender's other devices. Clients deduplicate by id.
                await notify(name, event)

        @app.get('/api/v1/contacts')
        def contacts(user=Depends(session)):
            return envelope({'items': svc.list_contacts(user['username'])})

        @app.post('/api/v1/contacts/invite', status_code=201)
        def invite(user=Depends(session)):
            return envelope(svc.make_invite(user['username']))

        @app.post('/api/v1/contacts/accept')
        def accept(body: InviteAccept, user=Depends(session)):
            return envelope(svc.accept_invite(body.code, user['username']))

        @app.delete('/api/v1/contacts/{peer}')
        def remove_contact(peer: str, user=Depends(session)):
            return envelope(svc.remove_contact(user['username'], peer))

        @app.get('/api/v1/conversations')
        def conversations(user=Depends(session)):
            return envelope({'items': svc.list_conversations(user['username'])})

        @app.get('/api/v1/messages/unread')
        def unread(user=Depends(session)):
            return envelope(svc.unread_total(user['username']))

        @app.post('/api/v1/conversations/direct')
        def direct(body: ContactPeer, user=Depends(session)):
            return envelope(svc.direct(user['username'], body.peer))

        @app.post('/api/v1/conversations', status_code=201)
        def create_group(body: GroupCreate, user=Depends(session)):
            return envelope(svc.create_group(user['username'], body.name, body.members))

        @app.patch('/api/v1/conversations/{cid}')
        def rename(cid: str, body: GroupRename, user=Depends(session)):
            return envelope(svc.rename_group(cid, user['username'], body.name))

        @app.post('/api/v1/conversations/{cid}/members')
        def add_member(cid: str, body: MemberChange, user=Depends(session)):
            return envelope(svc.add_member(cid, user['username'], body.username))

        @app.delete('/api/v1/conversations/{cid}/members/{username}')
        def remove_member(cid: str, username: str, user=Depends(session)):
            return envelope(svc.remove_member(cid, user['username'], username))

        @app.get('/api/v1/conversations/{cid}/messages')
        def history(cid: str, before: float | None = Query(default=None),
                    limit: int = Query(default=30, ge=1, le=100), user=Depends(session)):
            return envelope(svc.history(cid, user['username'], before, limit))

        @app.post('/api/v1/conversations/{cid}/messages', status_code=201)
        async def send(cid: str, body: TextMessage, user=Depends(session)):
            message = svc.send_text(cid, user['username'], body.body, body.client_id)
            await fanout(cid, user['username'], message)
            return envelope(message)

        @app.post('/api/v1/conversations/{cid}/messages/media', status_code=201)
        async def send_media_message(cid: str, body: MediaMessage, user=Depends(session)):
            message = svc.send_media(cid, user['username'], body.media_id, body.client_id)
            await fanout(cid, user['username'], message)
            return envelope(message)

        @app.post('/api/v1/conversations/{cid}/read')
        def read(cid: str, user=Depends(session)):
            return envelope(svc.mark_read(cid, user['username']))

        @app.post('/api/v1/media', status_code=201)
        async def upload_media(request: Request, conversation_id: str = Query(...),
                              duration: float | None = Query(default=None),
                              user=Depends(session)):
            svc.require_member(conversation_id, user['username'])
            raw = bytearray()
            async for chunk in request.stream():
                if len(raw) + len(chunk) > MAX_MEDIA_BYTES:
                    raise HTTPException(413, 'Media too large')
                raw.extend(chunk)
            info = svc.save_media(conversation_id, user['username'], raw,
                                  request.headers.get('content-type', ''), duration)
            return envelope(info)

        @app.get('/api/v1/media/{media_id}')
        def get_media(media_id: str, user=Depends(session)):
            content_type, data = svc.open_media(media_id, user['username'])
            return Response(content=data, media_type=content_type,
                            headers={'Cache-Control': 'private, no-store', 'X-Content-Type-Options': 'nosniff'})

        return svc
