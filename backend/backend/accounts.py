"""Independent account invitations and revocable sessions; no shared passwords."""
import hashlib
import hmac
import secrets
import time
from collections import defaultdict, deque
from threading import Lock
from fastapi import Depends, HTTPException, Query, Request
from pydantic import BaseModel, ConfigDict, Field
from .store import digest

class Strict(BaseModel):
    model_config = ConfigDict(extra='forbid')
class InviteIn(Strict):
    role: str = Field(pattern=r'^(caregiver|viewer|community|property)$')
    expires_hours: int = Field(default=24, ge=1, le=168)
class InviteToken(Strict):
    invitation_token: str = Field(min_length=32, max_length=128)
class RegisterInvite(InviteToken):
    username: str = Field(pattern=r'^[A-Za-z0-9_-]{3,32}$')
    password: str = Field(min_length=8,max_length=128)
class ChangePassword(Strict):
    old_password: str = Field(min_length=1,max_length=256)
    new_password: str = Field(min_length=8,max_length=128)
class DeleteSelf(Strict):
    password: str = Field(min_length=1,max_length=256)

def hash_password(password):
    salt=secrets.token_hex(16)
    return salt,hashlib.pbkdf2_hmac('sha256',password.encode(),bytes.fromhex(salt),210000).hex()
def matches(row,password):
    salt=bytes.fromhex(row['salt']) if row else bytes(16)
    value=hashlib.pbkdf2_hmac('sha256',password.encode(),salt,210000).hex()
    return bool(row) and hmac.compare_digest(value,row['password_hash'])

class Accounts:
    def __init__(self,store,settings):
        self.store,self.settings=store,settings
        self.attempt_lock=Lock()
        self.attempts=defaultdict(deque)
        with store.connection() as db:
            db.executescript('''
            CREATE TABLE IF NOT EXISTS account_invitations(id TEXT PRIMARY KEY,token_hash TEXT UNIQUE,home TEXT,role TEXT,created_by TEXT,created REAL,expires REAL,consumed_by TEXT,revoked INTEGER DEFAULT 0);
            CREATE TABLE IF NOT EXISTS session_details(token_hash TEXT PRIMARY KEY,id TEXT UNIQUE,label TEXT,created REAL);
            ''')
    def record_session(self,token,label):
        label=''.join(c for c in label if c.isprintable())[:160] or '未命名客户端'
        with self.store.connection() as db:
            db.execute('INSERT OR IGNORE INTO session_details VALUES(?,?,?,?)',(digest(token),secrets.token_hex(16),label,time.time()))
            db.execute('DELETE FROM session_details WHERE token_hash NOT IN (SELECT token_hash FROM sessions)')
    def limited(self,key):
        with self.attempt_lock:
            now=time.monotonic();q=self.attempts[key]
            while q and q[0]<now-60:q.popleft()
            if len(q)>=10:raise HTTPException(429,'Too many attempts; retry later')
            q.append(now)
    @staticmethod
    def revoke(db,username,except_hash=None):
        if except_hash:
            db.execute('DELETE FROM tickets WHERE session_hash IN (SELECT token_hash FROM sessions WHERE username=? AND token_hash<>?)',(username,except_hash))
            db.execute('DELETE FROM sessions WHERE username=? AND token_hash<>?',(username,except_hash))
        else:
            db.execute('DELETE FROM tickets WHERE session_hash IN (SELECT token_hash FROM sessions WHERE username=?)',(username,))
            db.execute('DELETE FROM sessions WHERE username=?',(username,))
        db.execute('DELETE FROM session_details WHERE token_hash NOT IN (SELECT token_hash FROM sessions)')
    @staticmethod
    def audit(db,home,actor,target,action):
        db.execute('INSERT INTO account_audit(home,actor,target,action,created) VALUES(?,?,?,?,?)',(home or '',actor,target,action,time.time()))
    @staticmethod
    def owner(db,username):
        m=db.execute('SELECT * FROM home_members WHERE username=?',(username,)).fetchone()
        if not m or m['role']!='owner':raise HTTPException(403,'Owner required')
        return m['home']
    @staticmethod
    def invitation(db,token):
        row=db.execute('SELECT * FROM account_invitations WHERE token_hash=?',(digest(token),)).fetchone()
        if not row or row['revoked'] or row['consumed_by'] or row['expires']<=time.time():raise HTTPException(410,'Invitation expired, used or revoked')
        # Removing/downgrading the issuer invalidates its outstanding invitations.
        issuer=db.execute('SELECT home,role FROM home_members WHERE username=?',(row['created_by'],)).fetchone()
        if not issuer or issuer['home']!=row['home'] or issuer['role']!='owner':raise HTTPException(410,'Invitation no longer valid')
        return row
    def install(self,app,session,envelope):
        @app.post('/api/v1/household/invitations',status_code=201)
        def create_invitation(body:InviteIn,user=Depends(session)):
            with self.store.connection() as db:
                db.execute('BEGIN IMMEDIATE');home=self.owner(db,user['username'])
                active=db.execute('SELECT COUNT(*) FROM account_invitations WHERE home=? AND revoked=0 AND consumed_by IS NULL AND expires>?',(home,time.time())).fetchone()[0]
                if active>=20:raise HTTPException(409,'Too many pending invitations')
                token=secrets.token_urlsafe(32);ident=secrets.token_hex(16);now=time.time();expires=now+body.expires_hours*3600
                db.execute('INSERT INTO account_invitations VALUES(?,?,?,?,?,?,?,?,0)',(ident,digest(token),home,body.role,user['username'],now,expires,None))
                self.audit(db,home,user['username'],ident,'invite.create:'+body.role)
            return envelope({'id':ident,'invitation_token':token,'role':body.role,'expires_at':expires,'single_use':True})
        @app.get('/api/v1/household/invitations')
        def list_invitations(user=Depends(session)):
            with self.store.connection() as db:
                home=self.owner(db,user['username'])
                rows=[dict(r) for r in db.execute('SELECT id,role,created,expires,consumed_by,revoked FROM account_invitations WHERE home=? ORDER BY created DESC LIMIT 100',(home,))]
            return envelope({'items':rows})
        @app.delete('/api/v1/household/invitations/{ident}')
        def revoke_invitation(ident:str,user=Depends(session)):
            with self.store.connection() as db:
                db.execute('BEGIN IMMEDIATE');home=self.owner(db,user['username'])
                row=db.execute('SELECT id FROM account_invitations WHERE id=? AND home=?',(ident,home)).fetchone()
                if not row:raise HTTPException(404,'Invitation not found')
                db.execute('UPDATE account_invitations SET revoked=1 WHERE id=?',(ident,));self.audit(db,home,user['username'],ident,'invite.revoke')
            return envelope({'revoked':True})
        @app.post('/api/v1/auth/register-invite',status_code=201)
        def register_invite(body:RegisterInvite,request:Request):
            self.limited(('register',request.client.host if request.client else 'unknown'))
            salt,hashed=hash_password(body.password)
            with self.store.connection() as db:
                db.execute('BEGIN IMMEDIATE');invite=self.invitation(db,body.invitation_token)
                if db.execute('SELECT 1 FROM users WHERE name=?',(body.username,)).fetchone() or db.execute('SELECT 1 FROM deleted_accounts WHERE username=?',(body.username,)).fetchone():raise HTTPException(409,'Account name unavailable; existing accounts must log in to accept')
                db.execute('INSERT INTO users VALUES(?,?,?)',(body.username,salt,hashed))
                db.execute('INSERT INTO home_members VALUES(?,?,?)',(invite['home'],body.username,invite['role']))
                db.execute('UPDATE account_invitations SET consumed_by=? WHERE id=?',(body.username,invite['id']))
                self.audit(db,invite['home'],body.username,body.username,'invite.accept:'+invite['role'])
            return envelope({'username':body.username,'role':invite['role'],'login_required':True})
        @app.post('/api/v1/household/invitations/accept')
        def accept_invite(body:InviteToken,user=Depends(session)):
            self.limited(('accept',user['username']))
            with self.store.connection() as db:
                db.execute('BEGIN IMMEDIATE');invite=self.invitation(db,body.invitation_token)
                if db.execute('SELECT 1 FROM home_members WHERE username=?',(user['username'],)).fetchone():raise HTTPException(409,'Leave current household before accepting another invitation')
                db.execute('INSERT INTO home_members VALUES(?,?,?)',(invite['home'],user['username'],invite['role']))
                db.execute('UPDATE account_invitations SET consumed_by=? WHERE id=?',(user['username'],invite['id']))
                self.audit(db,invite['home'],user['username'],user['username'],'invite.accept:'+invite['role'])
            return envelope({'joined':True,'role':invite['role']})
        @app.get('/api/v1/auth/sessions')
        def list_sessions(user=Depends(session)):
            with self.store.connection() as db:
                rows=db.execute('SELECT s.token_hash,s.expires,d.id,d.label,d.created FROM sessions s LEFT JOIN session_details d ON d.token_hash=s.token_hash WHERE s.username=? AND s.expires>? ORDER BY COALESCE(d.created,0) DESC',(user['username'],time.time())).fetchall()
                result=[]
                for r in rows:
                    ident=r['id']
                    if not ident:
                        ident=secrets.token_hex(16);db.execute('INSERT OR IGNORE INTO session_details VALUES(?,?,?,?)',(r['token_hash'],ident,'旧版登录客户端',time.time()))
                    canonical=db.execute('SELECT id,label,created FROM session_details WHERE token_hash=?',(r['token_hash'],)).fetchone()
                    ident=canonical['id']
                    result.append({'id':ident,'label':r['label'] or '旧版登录客户端','created_at':r['created'],'expires_at':r['expires'],'current':r['token_hash']==user['token_hash']})
            return envelope({'items':result})
        @app.post('/api/v1/auth/sessions/revoke-others')
        def revoke_others(user=Depends(session)):
            with self.store.connection() as db:
                db.execute('BEGIN IMMEDIATE');self.revoke(db,user['username'],user['token_hash']);self.audit(db,'',user['username'],user['username'],'sessions.revoke_others')
            return envelope({'other_sessions_revoked':True})
        @app.delete('/api/v1/auth/sessions/{ident}')
        def revoke_session(ident:str,user=Depends(session)):
            with self.store.connection() as db:
                db.execute('BEGIN IMMEDIATE');row=db.execute('SELECT s.token_hash FROM sessions s JOIN session_details d ON s.token_hash=d.token_hash WHERE d.id=? AND s.username=?',(ident,user['username'])).fetchone()
                if not row:raise HTTPException(404,'Session not found')
                db.execute('DELETE FROM tickets WHERE session_hash=?',(row['token_hash'],));db.execute('DELETE FROM sessions WHERE token_hash=?',(row['token_hash'],));db.execute('DELETE FROM session_details WHERE token_hash=?',(row['token_hash'],));self.audit(db,'',user['username'],user['username'],'session.revoke')
            return envelope({'revoked':True,'current_session':row['token_hash']==user['token_hash']})
        @app.post('/api/v1/auth/password')
        def change_password(body:ChangePassword,user=Depends(session)):
            self.limited(('password',user['username']))
            with self.store.connection() as db:
                db.execute('BEGIN IMMEDIATE');row=db.execute('SELECT * FROM users WHERE name=?',(user['username'],)).fetchone()
                if not matches(row,body.old_password):raise HTTPException(403,'Current password incorrect')
                salt,hashed=hash_password(body.new_password);db.execute('UPDATE users SET salt=?,password_hash=? WHERE name=?',(salt,hashed,user['username']));self.revoke(db,user['username']);self.audit(db,'',user['username'],user['username'],'password.change')
            return envelope({'changed':True,'login_required':True,'all_sessions_revoked':True})
        @app.delete('/api/v1/household/members/{username}/membership')
        def remove_membership(username:str,user=Depends(session)):
            with self.store.connection() as db:
                db.execute('BEGIN IMMEDIATE');home=self.owner(db,user['username'])
                target=db.execute('SELECT role FROM home_members WHERE home=? AND username=?',(home,username)).fetchone()
                if not target:raise HTTPException(404,'Member not found')
                if target['role']=='owner' or username==user['username']:raise HTTPException(409,'Owner membership is protected')
                db.execute('DELETE FROM home_members WHERE username=?',(username,));self.revoke(db,username);db.execute('DELETE FROM alarm_claims WHERE username=?',(username,));self.audit(db,home,user['username'],username,'membership.remove')
            return envelope({'username':username,'removed_from_household':True,'account_deleted':False,'login_required':True})
        @app.delete('/api/v1/account')
        def delete_self(body:DeleteSelf,user=Depends(session)):
            self.limited(('delete',user['username']))
            with self.store.connection() as db:
                db.execute('BEGIN IMMEDIATE');row=db.execute('SELECT * FROM users WHERE name=?',(user['username'],)).fetchone()
                if not matches(row,body.password):raise HTTPException(403,'Current password incorrect')
                member=db.execute('SELECT home,role FROM home_members WHERE username=?',(user['username'],)).fetchone()
                if user['username']==self.settings.username:raise HTTPException(409,'Server bootstrap account is protected')
                if member and member['role']=='owner':raise HTTPException(409,'Owner account is protected; household ownership must be transferred first')
                self.revoke(db,user['username']);db.execute('DELETE FROM alarm_claims WHERE username=?',(user['username'],))
                if getattr(app.state,'chats',None):app.state.chats.delete_account_relations(db,user['username'])
                db.execute('DELETE FROM home_members WHERE username=?',(user['username'],));db.execute('DELETE FROM users WHERE name=?',(user['username'],));db.execute('INSERT OR IGNORE INTO deleted_accounts VALUES(?,?)',(user['username'],time.time()));self.audit(db,member['home'] if member else '',user['username'],user['username'],'account.self_delete')
            return envelope({'deleted':True,'all_sessions_revoked':True})
        @app.get('/api/v1/household/audit')
        def audit_log(limit:int=Query(default=50,ge=1,le=200),user=Depends(session)):
            with self.store.connection() as db:
                home=self.owner(db,user['username']);rows=[dict(r) for r in db.execute('SELECT id,actor,target,action,created FROM account_audit WHERE home=? ORDER BY id DESC LIMIT ?',(home,limit))]
            return envelope({'items':rows})
