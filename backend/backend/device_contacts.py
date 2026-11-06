"""Persisted device messages, separate from person-to-person conversations."""
import asyncio
import hashlib
import json
import time
from typing import Literal
from fastapi import Depends, HTTPException, Query
from pydantic import BaseModel, ConfigDict, Field, ValidationError, model_validator

class ContactEvent(BaseModel):
    model_config=ConfigDict(extra='forbid',strict=True,populate_by_name=True)
    schema_version: Literal[1]=Field(alias='schema')
    boot_id: int=Field(ge=1,le=2147483647)
    message_id: int=Field(ge=1,le=2147483647)
    revision: int=Field(ge=1,le=1000000000)
    epoch: int=Field(ge=0,le=4294967295)
    direction: Literal['board','family']
    code: int=Field(ge=1,le=4)
    reply_to: int=Field(ge=0,le=2147483647)
    seen: bool
    text: str=Field(default='',max_length=120)
    author: str=Field(default='',max_length=32)
    request_id: str=Field(max_length=64,pattern=r'^[A-Za-z0-9_-]*$')
    @model_validator(mode='after')
    def direction_matches(self):
        if self.code==4:
            if not valid_text(self.text):raise ValueError('Text requires 1..120 UTF-8 bytes with no control characters')
            if not self.request_id:raise ValueError('Text requires a stable request id')
        elif self.text or self.author:raise ValueError('Preset messages cannot carry arbitrary text')
        if self.direction=='board' and (self.reply_to or (self.request_id and self.code!=4)):raise ValueError('Invalid board message context')
        if self.direction=='family' and (not self.request_id or (not self.reply_to and self.code!=4)):raise ValueError('Family message requires reply context')
        return self

def valid_text(value):
    try:return bool(value.strip()) and len(value.encode('utf-8'))<=120 and not any((ord(c)<32 and c not in '\n\t') or ord(c)==127 for c in value)
    except UnicodeError:return False

class ContactRequest(BaseModel):
    model_config=ConfigDict(extra='forbid')
    client_request_id: str=Field(min_length=1,max_length=128)
class ReplyRequest(ContactRequest):
    code: int=Field(ge=1,le=4,strict=True)
    text: str=Field(default='',max_length=120)
    @model_validator(mode='after')
    def valid_reply(self):
        if self.code==4 and not valid_text(self.text):raise ValueError('Text requires 1..120 UTF-8 bytes')
        if self.code!=4 and self.text:raise ValueError('Preset reply cannot include text')
        return self
class ChatRequest(ContactRequest):
    boot_id:int=Field(ge=1,le=2147483647,strict=True)
    text:str=Field(min_length=1,max_length=120)
    @model_validator(mode='after')
    def valid_body(self):
        if not valid_text(self.text):raise ValueError('Text requires 1..120 UTF-8 bytes')
        return self

class DeviceContacts:
    def __init__(self,store,settings,homes):
        self.store,self.settings,self.homes=store,settings,homes
        with store.connection() as db:
            db.executescript('''CREATE TABLE IF NOT EXISTS device_contact_events(
            cursor INTEGER PRIMARY KEY AUTOINCREMENT,mode TEXT,device TEXT,boot INTEGER,message INTEGER,revision INTEGER,body TEXT,received REAL,
            UNIQUE(mode,device,boot,message,revision));
            CREATE INDEX IF NOT EXISTS device_contact_cursor ON device_contact_events(mode,device,cursor);
            CREATE INDEX IF NOT EXISTS device_contact_latest ON device_contact_events(mode,device,boot,message,revision);
            CREATE TABLE IF NOT EXISTS device_contact_senders(mode TEXT,device TEXT,request TEXT,username TEXT,PRIMARY KEY(mode,device,request));''')
    def check(self,device,user=None,control=False):
        if not self.settings.has_device(device):raise HTTPException(404,'Device not configured')
        if user is not None:self.homes.authorize(user['username'],device,permission='device.messages.write' if control else 'device.messages.read')
        if device!=self.settings.primary_device:raise HTTPException(409,'Contact messages require the home gateway')
    def ingest(self,device,content):
        self.check(device)
        try:data=ContactEvent.model_validate(content).model_dump(by_alias=True)
        except ValidationError:raise HTTPException(422,'Invalid device contact event')
        with self.store.connection() as db:
            db.execute('BEGIN IMMEDIATE')
            if data['code']==4:
                if data['direction']=='family':
                    sender=db.execute('SELECT username FROM device_contact_senders WHERE mode=? AND device=? AND request=?',(self.settings.mode,device,data['request_id'])).fetchone()
                    if not sender:raise HTTPException(409,'Unknown family text sender')
                    data['author']=sender['username']
                else:data['author']='NFC网页'
            old=db.execute('SELECT * FROM device_contact_events WHERE mode=? AND device=? AND boot=? AND message=? ORDER BY revision DESC LIMIT 1',(self.settings.mode,device,data['boot_id'],data['message_id'])).fetchone()
            if old:
                previous=json.loads(old['body'])
                if any(previous.get(k,'')!=data[k] for k in ('schema','boot_id','message_id','epoch','direction','code','reply_to','request_id','text','author')):raise HTTPException(409,'Message identity conflict')
                if data['revision']<=previous['revision']:
                    if data['revision']==previous['revision'] and any(previous.get(k,'')!=v for k,v in data.items()):raise HTTPException(409,'Message revision conflict')
                    return {'duplicate':True,'cursor':old['cursor'],'event':previous}
                if previous['seen'] and not data['seen']:raise HTTPException(409,'Seen state cannot move backwards')
            cur=db.execute('INSERT INTO device_contact_events(mode,device,boot,message,revision,body,received) VALUES(?,?,?,?,?,?,?)',(self.settings.mode,device,data['boot_id'],data['message_id'],data['revision'],json.dumps(data,separators=(',',':')),time.time()))
            return {'duplicate':False,'cursor':cur.lastrowid,'event':data}
    def board_message(self,device,boot,message):
        with self.store.connection() as db:
            row=db.execute('SELECT body FROM device_contact_events WHERE mode=? AND device=? AND boot=? AND message=? ORDER BY revision DESC LIMIT 1',(self.settings.mode,device,boot,message)).fetchone()
        if not row or json.loads(row['body'])['direction']!='board':raise HTTPException(404,'Board message not found')
        return json.loads(row['body'])
    def mock_execute(self,device,p):
        if p['action']!='chat':
            try:board=self.board_message(device,p['boot_id'],p['message_id'])
            except HTTPException:return 2
        if p['action']=='seen':
            if not board['seen']:self.ingest(device,{**board,'seen':True,'revision':board['revision']+1})
        else:
            with self.store.connection() as db:
                rows=db.execute('SELECT body FROM device_contact_events WHERE mode=? AND device=? AND boot=?',(self.settings.mode,device,p['boot_id'])).fetchall()
            existing=[json.loads(r['body']) for r in rows]
            if not any(r['request_id']==p['request_id'] for r in existing):
                ident=max([r['message_id'] for r in existing] or [0])+1
                self.ingest(device,{'schema':1,'boot_id':p['boot_id'],'message_id':ident,'revision':1,'epoch':int(time.time()),'direction':'family','code':p['code'],'reply_to':p['message_id'],'seen':False,'request_id':p['request_id'],**({'text':p['text'],'author':p['author']} if p['code']==4 else {})})
        return 0
    def install(self,app,session,envelope,run_command,tasks,command_data):
        @app.get('/api/v1/devices/{device}/messages')
        def changes(device:str,after:int=Query(default=0,ge=0),limit:int=Query(default=100,ge=1,le=200),user=Depends(session)):
            self.check(device,user)
            with self.store.connection() as db:
                rows=db.execute('SELECT cursor,body,received FROM device_contact_events WHERE mode=? AND device=? AND cursor>? ORDER BY cursor LIMIT ?',(self.settings.mode,device,after,limit+1)).fetchall()
            items=[{'cursor':r['cursor'],'received_at':r['received'],**json.loads(r['body'])} for r in rows[:limit]]
            health=self.store.doc(self.settings.mode,device,'health') or {}
            return envelope({'text_supported':health.get('contact_text') is True,'boot_id':health.get('boot_id'),'items':items,'next_cursor':items[-1]['cursor'] if items else after,'has_more':len(rows)>limit,'merge_key':['boot_id','message_id'],'kind':'device_contact'})
        def submit(device,boot,message,body,user,action):
            self.check(device,user,True)
            if boot<1 or boot>2147483647 or message<0 or message>2147483647:raise HTTPException(422,'Invalid message identity')
            if action!='chat':self.board_message(device,boot,message)
            text=body.text if action=='chat' or (action=='reply' and body.code==4) else ''
            if text:
                health=self.store.doc(self.settings.mode,device,'health') or {}
                if health.get('contact_text') is not True:raise HTTPException(409,'Upgrade gateway firmware to v3.5.58 for text chat')
                if health.get('boot_id')!=boot:raise HTTPException(409,'Device restarted; refresh messages')
            # A stable opaque nonce prevents cross-account collisions on board.
            namespace=json.dumps([user['username'],body.client_request_id,boot,message,action],separators=(',',':'))
            nonce=hashlib.sha256(namespace.encode()).hexdigest()
            paras={'action':action,'boot_id':boot,'message_id':message,'code':0,'request_id':''}
            if action=='seen':paras.pop('code');paras.pop('request_id')
            if action in ('reply','chat'):paras.update(code=4 if action=='chat' else body.code,request_id=nonce)
            if text:
                paras.update(text=text,author=user['username'].encode('utf-8')[:32].decode('utf-8','ignore') or '家人')
                with self.store.connection() as db:db.execute('INSERT OR IGNORE INTO device_contact_senders VALUES(?,?,?,?)',(self.settings.mode,device,nonce,user['username']))
            payload={'service_id':self.settings.service_id,'command_name':'contact','paras':paras}
            try:row,new=self.store.new_command(self.settings.mode,device,user['username'],'contact:'+nonce,payload)
            except ValueError as exc:raise HTTPException(409,str(exc))
            if new:
                task=asyncio.create_task(run_command(row));tasks.add(task);task.add_done_callback(tasks.discard)
            return envelope({**command_data(row),'delivery_stage':{'pending':'board_pending','executed':'board_executed','failed':'failed','unknown':'unknown'}.get(row['state'],'unknown'),'human_confirmed':False if row['state']=='pending' else None,'message_request_id':nonce,'confirmation_source':'device_contact_feed'},message='accepted' if new else 'existing request')
        @app.post('/api/v1/devices/{device}/messages/chat',status_code=202)
        async def chat(device:str,body:ChatRequest,user=Depends(session)):
            return submit(device,body.boot_id,0,body,user,'chat')
        @app.post('/api/v1/devices/{device}/messages/{boot}/{message}/reply',status_code=202)
        async def reply(device:str,boot:int,message:int,body:ReplyRequest,user=Depends(session)):
            return submit(device,boot,message,body,user,'reply')
        @app.post('/api/v1/devices/{device}/messages/{boot}/{message}/seen',status_code=202)
        async def seen(device:str,boot:int,message:int,body:ContactRequest,user=Depends(session)):
            return submit(device,boot,message,body,user,'seen')
