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
    code: int=Field(ge=1,le=3)
    reply_to: int=Field(ge=0,le=2147483647)
    seen: bool
    request_id: str=Field(max_length=64,pattern=r'^[A-Za-z0-9_-]*$')
    @model_validator(mode='after')
    def direction_matches(self):
        if self.direction=='board' and (self.reply_to or self.request_id):raise ValueError('board message cannot be a reply')
        if self.direction=='family' and (not self.reply_to or not self.request_id):raise ValueError('family message requires reply context')
        return self
class ContactRequest(BaseModel):
    model_config=ConfigDict(extra='forbid')
    client_request_id: str=Field(min_length=1,max_length=128)
class ReplyRequest(ContactRequest):
    code: int=Field(ge=1,le=3,strict=True)

class DeviceContacts:
    def __init__(self,store,settings,homes):
        self.store,self.settings,self.homes=store,settings,homes
        with store.connection() as db:
            db.executescript('''CREATE TABLE IF NOT EXISTS device_contact_events(
            cursor INTEGER PRIMARY KEY AUTOINCREMENT,mode TEXT,device TEXT,boot INTEGER,message INTEGER,revision INTEGER,body TEXT,received REAL,
            UNIQUE(mode,device,boot,message,revision));
            CREATE INDEX IF NOT EXISTS device_contact_cursor ON device_contact_events(mode,device,cursor);
            CREATE INDEX IF NOT EXISTS device_contact_latest ON device_contact_events(mode,device,boot,message,revision);''')
    def check(self,device,user=None,control=False):
        if not self.settings.has_device(device):raise HTTPException(404,'Device not configured')
        if user is not None:self.homes.authorize(user['username'],device,control=control)
        if device!=self.settings.primary_device:raise HTTPException(409,'Contact messages require the home gateway')
    def ingest(self,device,content):
        self.check(device)
        try:data=ContactEvent.model_validate(content).model_dump(by_alias=True)
        except ValidationError:raise HTTPException(422,'Invalid device contact event')
        with self.store.connection() as db:
            db.execute('BEGIN IMMEDIATE')
            old=db.execute('SELECT * FROM device_contact_events WHERE mode=? AND device=? AND boot=? AND message=? ORDER BY revision DESC LIMIT 1',(self.settings.mode,device,data['boot_id'],data['message_id'])).fetchone()
            if old:
                previous=json.loads(old['body'])
                if any(previous[k]!=data[k] for k in ('schema','boot_id','message_id','epoch','direction','code','reply_to','request_id')):raise HTTPException(409,'Message identity conflict')
                if data['revision']<=previous['revision']:
                    if data['revision']==previous['revision'] and data!=previous:raise HTTPException(409,'Message revision conflict')
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
        try:board=self.board_message(device,p['boot_id'],p['message_id'])
        except HTTPException:return 2
        if p['action']=='seen':
            if not board['seen']:self.ingest(device,{**board,'seen':True,'revision':board['revision']+1})
        else:
            with self.store.connection() as db:
                rows=db.execute('SELECT body FROM device_contact_events WHERE mode=? AND device=? AND boot=?',(self.settings.mode,device,p['boot_id'])).fetchall()
            existing=[json.loads(r['body']) for r in rows]
            if not any(r['request_id']==p['request_id'] for r in existing):
                ident=max(r['message_id'] for r in existing)+1
                self.ingest(device,{'schema':1,'boot_id':p['boot_id'],'message_id':ident,'revision':1,'epoch':int(time.time()),'direction':'family','code':p['code'],'reply_to':p['message_id'],'seen':False,'request_id':p['request_id']})
        return 0
    def install(self,app,session,envelope,run_command,tasks,command_data):
        @app.get('/api/v1/devices/{device}/messages')
        def changes(device:str,after:int=Query(default=0,ge=0),limit:int=Query(default=100,ge=1,le=200),user=Depends(session)):
            self.check(device,user)
            with self.store.connection() as db:
                rows=db.execute('SELECT cursor,body,received FROM device_contact_events WHERE mode=? AND device=? AND cursor>? ORDER BY cursor LIMIT ?',(self.settings.mode,device,after,limit+1)).fetchall()
            items=[{'cursor':r['cursor'],'received_at':r['received'],**json.loads(r['body'])} for r in rows[:limit]]
            return envelope({'items':items,'next_cursor':items[-1]['cursor'] if items else after,'has_more':len(rows)>limit,'merge_key':['boot_id','message_id'],'kind':'device_contact'})
        def submit(device,boot,message,body,user,action):
            self.check(device,user,True)
            if boot<1 or boot>2147483647 or message<1 or message>2147483647:raise HTTPException(422,'Invalid message identity')
            self.board_message(device,boot,message)
            # A stable opaque nonce prevents cross-account collisions on board.
            namespace=json.dumps([user['username'],body.client_request_id,boot,message,action],separators=(',',':'))
            nonce=hashlib.sha256(namespace.encode()).hexdigest()
            paras={'action':action,'boot_id':boot,'message_id':message,'code':0,'request_id':''}
            if action=='reply':paras.update(code=body.code,request_id=nonce)
            payload={'service_id':self.settings.service_id,'command_name':'contact','paras':paras}
            try:row,new=self.store.new_command(self.settings.mode,device,user['username'],'contact:'+nonce,payload)
            except ValueError as exc:raise HTTPException(409,str(exc))
            if new:
                task=asyncio.create_task(run_command(row));tasks.add(task);task.add_done_callback(tasks.discard)
            return envelope({**command_data(row),'delivery_stage':{'pending':'board_pending','executed':'board_executed','failed':'failed','unknown':'unknown'}.get(row['state'],'unknown'),'human_confirmed':False if row['state']=='pending' else None,'confirmation_source':'device_contact_feed'},message='accepted' if new else 'existing request')
        @app.post('/api/v1/devices/{device}/messages/{boot}/{message}/reply',status_code=202)
        async def reply(device:str,boot:int,message:int,body:ReplyRequest,user=Depends(session)):
            return submit(device,boot,message,body,user,'reply')
        @app.post('/api/v1/devices/{device}/messages/{boot}/{message}/seen',status_code=202)
        async def seen(device:str,boot:int,message:int,body:ContactRequest,user=Depends(session)):
            return submit(device,boot,message,body,user,'seen')
