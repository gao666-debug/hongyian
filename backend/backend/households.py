"""Household membership is enforced on the server, independently of App UI."""
import secrets
from fastapi import Depends, HTTPException
from pydantic import BaseModel, Field, ConfigDict

class MemberInput(BaseModel):
    model_config = ConfigDict(extra='forbid')
    username: str = Field(pattern=r'^[A-Za-z0-9_-]{3,32}$')
    password: str = Field(min_length=8,max_length=128)
    role: str = Field(pattern=r'^(caregiver|viewer)$')

class Households:
    def __init__(self,store,settings):
        self.store,self.settings=store,settings
        with store.connection() as db:
            db.executescript('''CREATE TABLE IF NOT EXISTS home_members(home TEXT,username TEXT UNIQUE,role TEXT,PRIMARY KEY(home,username));
            CREATE TABLE IF NOT EXISTS home_devices(device TEXT PRIMARY KEY,home TEXT,kind TEXT,name TEXT,room TEXT);''')
            db.execute('INSERT OR IGNORE INTO home_members VALUES(?,?,?)',('default',settings.username,'owner'))
            for device,kind,name,room in [
                (settings.primary_device,'gateway','居家守护主机','客厅'),
                *((device,'nearlink','星闪环境节点','房间待设置') for device in settings.device_ids[1:]),
                (settings.nearlink_device_id,'nearlink','星闪 A','房间待设置'),
                (settings.watch_device_id,'wearable','星闪 B · 健康手表','随身')]:
                if device: db.execute('INSERT OR IGNORE INTO home_devices VALUES(?,?,?,?,?)',(device,'default',kind,name,room))

    def configured_ids(self):
        return set(self.settings.allowed_device_ids)

    def authorize(self,username,device,control=False):
        with self.store.connection() as db:
            row=db.execute('SELECT m.role FROM home_members m JOIN home_devices d ON d.home=m.home WHERE m.username=? AND d.device=?',(username,device)).fetchone()
        if not row or device not in self.configured_ids(): raise HTTPException(404,'Device not available to this account')
        if control and row['role']=='viewer': raise HTTPException(403,'Read-only account')

    def device_ids(self,username):
        with self.store.connection() as db:
            return [r['device'] for r in db.execute('SELECT d.device FROM home_devices d JOIN home_members m ON m.home=d.home WHERE m.username=?',(username,)) if r['device'] in self.configured_ids()]

    def install(self,app,session,envelope):
        @app.get('/api/v1/household')
        def household(user=Depends(session)):
            with self.store.connection() as db:
                member=db.execute('SELECT home,role FROM home_members WHERE username=?',(user['username'],)).fetchone()
                if not member: raise HTTPException(403,'No household membership')
                nodes=[dict(r) for r in db.execute('SELECT device AS device_id,kind,name,room FROM home_devices WHERE home=?',(member['home'],)) if r['device_id'] in self.configured_ids()]
                members=[dict(r) for r in db.execute('SELECT username,role FROM home_members WHERE home=?',(member['home'],))]
            for node in nodes:
                node['configured']=True
                shadow=self.store.doc(self.settings.mode,node['device_id'],'shadow') or {}
                node['properties']=shadow.get('properties')
                node['updated_at']=shadow.get('updated_at')
                node['watch']=self.store.doc(self.settings.mode,self.settings.primary_device,'watch') if node['kind']=='wearable' else None
            return envelope({'name':'颐鸿安 · 我的家庭','role':member['role'],'devices':nodes,'members':members})

        @app.post('/api/v1/household/members',status_code=201)
        def create_member(body:MemberInput,user=Depends(session)):
            # Only the existing owner can provision an account; no public registration.
            with self.store.connection() as db:
                owner=db.execute('SELECT home,role FROM home_members WHERE username=?',(user['username'],)).fetchone()
                if not owner or owner['role']!='owner': raise HTTPException(403,'Owner required')
                if db.execute('SELECT 1 FROM users WHERE name=?',(body.username,)).fetchone(): raise HTTPException(409,'Account already exists')
                import hashlib
                salt=secrets.token_hex(16)
                hashed=hashlib.pbkdf2_hmac('sha256',body.password.encode(),bytes.fromhex(salt),210000).hex()
                db.execute('INSERT INTO users VALUES(?,?,?)',(body.username,salt,hashed))
                db.execute('INSERT INTO home_members VALUES(?,?,?)',(owner['home'],body.username,body.role))
            return envelope({'username':body.username,'role':body.role})
