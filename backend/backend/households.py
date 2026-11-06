"""Household membership is enforced on the server, independently of App UI."""
import secrets
import time
from fastapi import Depends, HTTPException
from pydantic import BaseModel, Field, ConfigDict

class MemberInput(BaseModel):
    model_config = ConfigDict(extra='forbid')
    username: str = Field(pattern=r'^[A-Za-z0-9_-]{3,32}$')
    password: str = Field(min_length=8,max_length=128)
    role: str = Field(pattern=r'^(caregiver|viewer|community|property)$')

class MemberRoleInput(BaseModel):
    model_config = ConfigDict(extra='forbid')
    role: str = Field(pattern=r'^(caregiver|viewer|community|property)$')

ROLE_PERMISSIONS = {
    'owner': {'view', 'control', 'alarms.read', 'alarms.handle', 'members.manage'},
    'caregiver': {'view', 'control', 'alarms.read', 'alarms.handle'},
    'viewer': {'view', 'alarms.read'},
    'community': {'alarms.read', 'alarms.handle'},
    'property': {'alarms.read', 'alarms.handle'},
}

class Households:
    def __init__(self,store,settings):
        self.store,self.settings=store,settings
        with store.connection() as db:
            db.executescript('''CREATE TABLE IF NOT EXISTS home_members(home TEXT,username TEXT UNIQUE,role TEXT,PRIMARY KEY(home,username));
            CREATE TABLE IF NOT EXISTS home_devices(device TEXT PRIMARY KEY,home TEXT,kind TEXT,name TEXT,room TEXT);
            CREATE TABLE IF NOT EXISTS account_audit(id INTEGER PRIMARY KEY AUTOINCREMENT,home TEXT,actor TEXT,target TEXT,action TEXT,created REAL);
            CREATE TABLE IF NOT EXISTS deleted_accounts(username TEXT PRIMARY KEY,created REAL);''')
            db.execute('INSERT OR IGNORE INTO home_members VALUES(?,?,?)',('default',settings.username,'owner'))
            for device,kind,name,room in [
                (settings.primary_device,'gateway','居家守护主机','客厅'),
                *((device,'nearlink','星闪环境节点','房间待设置') for device in settings.device_ids[1:] if device != settings.watch_device_id),
                (settings.nearlink_device_id,'nearlink','星闪 A','房间待设置'),
                (settings.watch_device_id,'wearable','星闪 B · 健康手表','随身')]:
                if device: db.execute('INSERT OR IGNORE INTO home_devices VALUES(?,?,?,?,?)',(device,'default',kind,name,room))
            # Repair the previous broad environment registration without moving household ownership.
            if settings.watch_device_id:
                db.execute("UPDATE home_devices SET kind='wearable' WHERE device=?",(settings.watch_device_id,))

    def describe(self,username):
        with self.store.connection() as db:
            return [dict(r) for r in db.execute('SELECT d.device AS device_id,d.kind,d.name,d.room FROM home_devices d JOIN home_members m ON m.home=d.home WHERE m.username=?',(username,)) if r['device_id'] in self.configured_ids()]

    def configured_ids(self):
        return set(self.settings.allowed_device_ids)

    def role(self, username):
        with self.store.connection() as db:
            row=db.execute('SELECT role FROM home_members WHERE username=?',(username,)).fetchone()
        return row['role'] if row else ''

    def permissions(self, username):
        return sorted(ROLE_PERMISSIONS.get(self.role(username), set()))

    def authorize(self,username,device,control=False,permission=None):
        with self.store.connection() as db:
            row=db.execute('SELECT m.role FROM home_members m JOIN home_devices d ON d.home=m.home WHERE m.username=? AND d.device=?',(username,device)).fetchone()
        if not row or device not in self.configured_ids(): raise HTTPException(404,'Device not available to this account')
        required=permission or ('control' if control else 'view')
        if required not in ROLE_PERMISSIONS.get(row['role'],set()): raise HTTPException(403,'Account permission denied')

    def device_ids(self,username):
        with self.store.connection() as db:
            return [r['device'] for r in db.execute('SELECT d.device FROM home_devices d JOIN home_members m ON m.home=d.home WHERE m.username=?',(username,)) if r['device'] in self.configured_ids()]

    def install(self,app,session,envelope):
        @app.get('/api/v1/household')
        def household(user=Depends(session)):
            with self.store.connection() as db:
                member=db.execute('SELECT home,role FROM home_members WHERE username=?',(user['username'],)).fetchone()
                if not member: return envelope({'name':'尚未加入家庭','role':'','permissions':[],'devices':[],'members':[],'membership':False})
                nodes=[dict(r) for r in db.execute('SELECT device AS device_id,kind,name,room FROM home_devices WHERE home=?',(member['home'],)) if r['device_id'] in self.configured_ids()]
                members=[dict(r) for r in db.execute('SELECT username,role FROM home_members WHERE home=?',(member['home'],))] if member['role']=='owner' else [{'username':user['username'],'role':member['role']}]
            permissions=sorted(ROLE_PERMISSIONS.get(member['role'],set()))
            for node in nodes:
                node['configured']=True
                if 'view' not in permissions: continue
                shadow=self.store.doc(self.settings.mode,node['device_id'],'shadow') or {}
                node['properties']=shadow.get('properties')
                node['updated_at']=shadow.get('updated_at')
                node['online']=shadow.get('online')
                node['watch']=self.store.doc(self.settings.mode,self.settings.primary_device,'watch') if node['kind']=='wearable' and self.settings.primary_device in [n['device_id'] for n in nodes] else None
            return envelope({'name':'颐鸿安 · 我的家庭','role':member['role'],'permissions':permissions,'devices':nodes,'members':members})

        @app.post('/api/v1/household/members',status_code=201)
        def create_member(body:MemberInput,user=Depends(session)):
            # Only the existing owner can provision an account; no public registration.
            with self.store.connection() as db:
                db.execute('BEGIN IMMEDIATE')
                owner=db.execute('SELECT home,role FROM home_members WHERE username=?',(user['username'],)).fetchone()
                if not owner or owner['role']!='owner': raise HTTPException(403,'Owner required')
                if db.execute('SELECT 1 FROM deleted_accounts WHERE username=?',(body.username,)).fetchone(): raise HTTPException(409,'Deleted account name cannot be reused')
                if db.execute('SELECT 1 FROM users WHERE name=?',(body.username,)).fetchone(): raise HTTPException(409,'Account already exists')
                import hashlib
                salt=secrets.token_hex(16)
                hashed=hashlib.pbkdf2_hmac('sha256',body.password.encode(),bytes.fromhex(salt),210000).hex()
                db.execute('INSERT INTO users VALUES(?,?,?)',(body.username,salt,hashed))
                db.execute('INSERT INTO home_members VALUES(?,?,?)',(owner['home'],body.username,body.role))
                db.execute('INSERT INTO account_audit(home,actor,target,action,created) VALUES(?,?,?,?,?)',(owner['home'],user['username'],body.username,'create:'+body.role,time.time()))
            return envelope({'username':body.username,'role':body.role})

        def managed_member(db, actor, username):
            owner=db.execute('SELECT home,role FROM home_members WHERE username=?',(actor,)).fetchone()
            if not owner or owner['role']!='owner': raise HTTPException(403,'Owner required')
            target=db.execute('SELECT home,role FROM home_members WHERE username=?',(username,)).fetchone()
            if not target or target['home']!=owner['home']: raise HTTPException(404,'Member not found')
            if username==actor or username==self.settings.username or target['role']=='owner': raise HTTPException(409,'Administrator account is protected')
            return owner['home']

        def revoke_sessions(db, username):
            db.execute('DELETE FROM tickets WHERE session_hash IN (SELECT token_hash FROM sessions WHERE username=?)',(username,))
            db.execute('DELETE FROM sessions WHERE username=?',(username,))

        @app.patch('/api/v1/household/members/{username}')
        def change_role(username:str,body:MemberRoleInput,user=Depends(session)):
            with self.store.connection() as db:
                db.execute('BEGIN IMMEDIATE')
                home=managed_member(db,user['username'],username)
                db.execute('UPDATE home_members SET role=? WHERE username=?',(body.role,username))
                revoke_sessions(db,username)
                db.execute('DELETE FROM alarm_claims WHERE username=?',(username,))
                db.execute('INSERT INTO account_audit(home,actor,target,action,created) VALUES(?,?,?,?,?)',(home,user['username'],username,'role:'+body.role,time.time()))
            return envelope({'username':username,'role':body.role,'sessions_revoked':True})

        @app.delete('/api/v1/household/members/{username}')
        def delete_account(username:str,user=Depends(session)):
            with self.store.connection() as db:
                db.execute('BEGIN IMMEDIATE')
                home=managed_member(db,user['username'],username)
                revoke_sessions(db,username)
                db.execute('DELETE FROM alarm_claims WHERE username=?',(username,))
                if getattr(app.state, 'chats', None):
                    app.state.chats.delete_account_relations(db, username)
                db.execute('DELETE FROM home_members WHERE username=?',(username,))
                db.execute('DELETE FROM users WHERE name=?',(username,))
                db.execute('INSERT OR IGNORE INTO deleted_accounts VALUES(?,?)',(username,time.time()))
                db.execute('INSERT INTO account_audit(home,actor,target,action,created) VALUES(?,?,?,?,?)',(home,user['username'],username,'delete',time.time()))
            return envelope({'username':username,'deleted':True,'sessions_revoked':True})
