import hashlib
import hmac
import json
from pathlib import Path
import secrets
import sqlite3
import time
from contextlib import contextmanager


def digest(value):
    return hashlib.sha256(value.encode()).hexdigest()

class Store:
    def __init__(self, path):
        self.path = path
        Path(path).parent.mkdir(parents=True, exist_ok=True)
        with self.connection() as db:
            db.executescript('''
            PRAGMA journal_mode=WAL;
            CREATE TABLE IF NOT EXISTS users(name TEXT PRIMARY KEY,salt TEXT NOT NULL,password_hash TEXT NOT NULL);
            CREATE TABLE IF NOT EXISTS sessions(token_hash TEXT PRIMARY KEY,username TEXT NOT NULL,expires REAL NOT NULL);
            CREATE TABLE IF NOT EXISTS tickets(ticket_hash TEXT PRIMARY KEY,session_hash TEXT NOT NULL,device TEXT NOT NULL,expires REAL NOT NULL);
            CREATE TABLE IF NOT EXISTS documents(mode TEXT,device TEXT,kind TEXT,key TEXT,body TEXT NOT NULL,updated REAL NOT NULL,
                PRIMARY KEY(mode,device,kind,key));
            CREATE TABLE IF NOT EXISTS inbox(mode TEXT,device TEXT,id TEXT,PRIMARY KEY(mode,device,id));
            CREATE TABLE IF NOT EXISTS commands(id TEXT PRIMARY KEY,mode TEXT,device TEXT,username TEXT,client_id TEXT,
                body TEXT,state TEXT,result INTEGER,cloud_id TEXT,created REAL,updated REAL,
                UNIQUE(mode,device,username,client_id));
            CREATE TABLE IF NOT EXISTS notes(id INTEGER PRIMARY KEY AUTOINCREMENT,mode TEXT,device TEXT,event_key TEXT,
                username TEXT,note TEXT,resolution TEXT,created REAL);
            CREATE INDEX IF NOT EXISTS doc_order ON documents(mode,device,kind,updated);
            ''')
            # A restart cannot prove the outcome of an interrupted cloud operation.
            db.execute("UPDATE commands SET state='unknown',updated=? WHERE state='pending'", (time.time(),))

    @contextmanager
    def connection(self):
        db = sqlite3.connect(self.path, timeout=10)
        db.row_factory = sqlite3.Row
        try:
            yield db
            db.commit()
        except Exception:
            db.rollback()
            raise
        finally:
            db.close()

    def bootstrap_user(self, name, password):
        with self.connection() as db:
            if not db.execute('SELECT 1 FROM users WHERE name=?', (name,)).fetchone():
                salt = secrets.token_hex(16)
                value = hashlib.pbkdf2_hmac('sha256', password.encode(), bytes.fromhex(salt), 210000).hex()
                db.execute('INSERT INTO users VALUES(?,?,?)', (name, salt, value))

    def login(self, name, password, seconds):
        with self.connection() as db:
            row = db.execute('SELECT * FROM users WHERE name=?', (name,)).fetchone()
            salt = bytes.fromhex(row['salt']) if row else bytes(16)
            value = hashlib.pbkdf2_hmac('sha256', password.encode(), salt, 210000).hex()
            if not row or not hmac.compare_digest(value, row['password_hash']):
                return None
            token = secrets.token_urlsafe(32)
            db.execute('DELETE FROM sessions WHERE expires<?', (time.time(),))
            db.execute('INSERT INTO sessions VALUES(?,?,?)', (digest(token), name, time.time()+seconds))
            return token

    def session(self, token_hash):
        with self.connection() as db:
            row = db.execute('SELECT * FROM sessions WHERE token_hash=? AND expires>?', (token_hash, time.time())).fetchone()
            return dict(row) if row else None

    def logout(self, token_hash):
        with self.connection() as db:
            db.execute('DELETE FROM sessions WHERE token_hash=?', (token_hash,))
            db.execute('DELETE FROM tickets WHERE session_hash=?', (token_hash,))

    def ticket(self, token_hash, device):
        token = secrets.token_urlsafe(32)
        with self.connection() as db:
            db.execute('DELETE FROM tickets WHERE expires<?', (time.time(),))
            db.execute('INSERT INTO tickets VALUES(?,?,?,?)', (digest(token), token_hash, device, time.time()+30))
        return token

    def consume_ticket(self, token, device):
        with self.connection() as db:
            row = db.execute('SELECT * FROM tickets WHERE ticket_hash=? AND device=? AND expires>?',
                             (digest(token), device, time.time())).fetchone()
            if row:
                db.execute('DELETE FROM tickets WHERE ticket_hash=?', (digest(token),))
            return dict(row) if row else None

    def doc(self, mode, device, kind, key='latest'):
        with self.connection() as db:
            row = db.execute('SELECT body FROM documents WHERE mode=? AND device=? AND kind=? AND key=?',
                             (mode,device,kind,key)).fetchone()
            return json.loads(row['body']) if row else None

    def put(self, mode, device, kind, body, key='latest', db=None):
        if db is None:
            with self.connection() as conn:
                return self.put(mode,device,kind,body,key,conn)
        db.execute('INSERT INTO documents VALUES(?,?,?,?,?,?) ON CONFLICT(mode,device,kind,key) DO UPDATE SET body=excluded.body,updated=excluded.updated',
                   (mode,device,kind,key,json.dumps(body,ensure_ascii=False,allow_nan=False),time.time()))

    def list_docs(self, mode, device, kind, limit=100, offset=0):
        with self.connection() as db:
            rows = db.execute('SELECT key,body FROM documents WHERE mode=? AND device=? AND kind=? ORDER BY updated DESC,key DESC LIMIT ? OFFSET ?',
                              (mode,device,kind,limit,offset)).fetchall()
            return [{'key':r['key'], **json.loads(r['body'])} for r in rows]

    def medicine_observation(self, mode, device, health, db=None):
        """Keep reported state transitions, including physical-button changes, across restarts."""
        if db is None:
            with self.connection() as conn:
                return self.medicine_observation(mode,device,health,conn)
        life=health.get('living') or {}
        if life.get('schema')!=2 or not life.get('time_synced') or life.get('dose_day',-1)<0:return
        for slot in life.get('slots') or []:
            latest_key=f"{life['dose_day']}:{slot['slot']}"
            row=db.execute('SELECT body FROM documents WHERE mode=? AND device=? AND kind=? AND key=?',(mode,device,'medicine_latest',latest_key)).fetchone()
            old=json.loads(row['body']) if row else None
            revision=life['medicine_revision'];epoch=health.get('epoch',0)
            if old and (revision<old['medicine_revision'] or revision==old['medicine_revision'] and epoch<old['epoch']):continue
            value=dict(dose_day=life['dose_day'],slot=slot['slot'],state=slot['state'],confirmed_epoch=slot['confirmed_epoch'],minute=slot['minute'],medicine_revision=revision,epoch=epoch)
            self.put(mode,device,'medicine_latest',value,latest_key,db)
            if slot['state'] not in (3,4) or old and old['state']==slot['state']:continue
            key=f"{latest_key}:{revision}:{slot['state']}"
            if not db.execute('SELECT 1 FROM documents WHERE mode=? AND device=? AND kind=? AND key=?',(mode,device,'medicine_history',key)).fetchone():
                self.put(mode,device,'medicine_history',value,key,db)

    def command(self, command_id, mode, device=None):
        with self.connection() as db:
            row = db.execute('SELECT * FROM commands WHERE id=? AND mode=?', (command_id,mode)).fetchone()
            return dict(row) if row and (device is None or row['device']==device) else None

    def new_command(self, mode, device, user, client_id, body):
        text = json.dumps(body,sort_keys=True,separators=(',',':'))
        with self.connection() as db:
            db.execute('BEGIN IMMEDIATE')
            row = db.execute('SELECT * FROM commands WHERE mode=? AND device=? AND username=? AND client_id=?',
                             (mode,device,user,client_id)).fetchone()
            if row:
                if row['body'] != text:
                    raise ValueError('client_request_id reused with a different command')
                return dict(row), False
            command_id = secrets.token_hex(16)
            now = time.time()
            db.execute('INSERT INTO commands VALUES(?,?,?,?,?,?,?,NULL,NULL,?,?)',
                       (command_id,mode,device,user,client_id,text,'pending',now,now))
            return dict(db.execute('SELECT * FROM commands WHERE id=?', (command_id,)).fetchone()), True

    def finish_command(self, command_id, state, result, cloud_id=None):
        with self.connection() as db:
            db.execute('UPDATE commands SET state=?,result=?,cloud_id=?,updated=? WHERE id=?',
                       (state,result,cloud_id,time.time(),command_id))
