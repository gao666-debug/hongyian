"""Versioned remote inactivity settings, based on actual device reports."""
import asyncio
import json
import time
from typing import Literal
from fastapi import Depends, HTTPException
from pydantic import BaseModel, ConfigDict, Field, model_validator

class CareReport(BaseModel):
    model_config=ConfigDict(extra='forbid',strict=True)
    enabled:bool
    sleep_enabled:bool
    start:int=Field(ge=0,le=1439)
    end:int=Field(ge=0,le=1439)
    quiet:int=Field(ge=0,le=1)
    revision:int=Field(ge=1,le=999999999)
    noact_ms:int=Field(ge=10000,le=86400000)
    mode:int=Field(ge=0,le=2)
    time_synced:bool
    pause_seconds:int=Field(ge=0,le=86400)
    pause_revision:int=Field(ge=1,le=999999999)
    @model_validator(mode='after')
    def valid_window(self):
        if self.sleep_enabled and self.start==self.end:raise ValueError('Sleep window must have different endpoints')
        return self
class CarePolicyInput(BaseModel):
    model_config=ConfigDict(extra='forbid',strict=True)
    boot_id:int=Field(ge=1,le=2147483647)
    enabled:bool
    sleep_enabled:bool
    start:int=Field(ge=0,le=1439)
    end:int=Field(ge=0,le=1439)
    quiet:int=Field(ge=0,le=1)
    noact_ms:int=Field(ge=10000,le=86400000)
    expected_revision:int=Field(ge=1,le=999999998)
    client_request_id:str=Field(min_length=1,max_length=128)
    @model_validator(mode='after')
    def valid_window(self):
        if self.sleep_enabled and self.start==self.end:raise ValueError('Sleep window must have different endpoints')
        return self
class CarePauseInput(BaseModel):
    model_config=ConfigDict(extra='forbid',strict=True)
    boot_id:int=Field(ge=1,le=2147483647)
    minutes:int=Field(ge=0,le=1440)
    expected_pause_revision:int=Field(ge=1,le=999999998)
    client_request_id:str=Field(min_length=1,max_length=128)

class CareControls:
    def __init__(self,store,settings,homes):self.store,self.settings,self.homes=store,settings,homes
    def report(self,device):
        with self.store.connection() as db:
            row=db.execute("SELECT body,updated FROM documents WHERE mode=? AND device=? AND kind='health' AND key='latest'",(self.settings.mode,device)).fetchone()
        health=json.loads(row['body']) if row else {}
        age=max(0,time.time()-row['updated']) if row else None
        return {'supported':bool(health.get('care_policy')),'actual':health.get('care_policy'),'boot_id':health.get('boot_id'),'fw':health.get('fw'),'updated_at':row['updated'] if row else None,'age_seconds':age,'stale':age is None or age>90}
    def check(self,device,user,control=False):
        if not self.settings.has_device(device):raise HTTPException(404,'Device not configured')
        self.homes.authorize(user['username'],device,control=control)
        if device!=self.settings.primary_device:raise HTTPException(409,'Inactivity policy requires the home gateway')
    def mock_execute(self,device,name,paras,health):
        care=health.get('care_policy')
        if not care or health.get('boot_id')!=paras['boot_id']:return 4
        care=dict(care)
        if name=='set_care_policy':
            if care['revision']!=paras['expected_revision']:return 4
            care.update({k:paras[k] for k in ('enabled','sleep_enabled','start','end','quiet','noact_ms')});care['revision']+=1
        else:
            if care['pause_revision']!=paras['expected_pause_revision']:return 4
            care['pause_revision']+=1;care['pause_seconds']=paras['minutes']*60
        minute=(int(time.time())//60+480)%1440
        sleeping=care['sleep_enabled'] and (care['start']<=minute<care['end'] if care['start']<care['end'] else minute>=care['start'] or minute<care['end'])
        care['mode']=0 if not care['enabled'] or care['pause_seconds'] else (2 if care['quiet'] else 0) if sleeping else 1
        health['care_policy']=care;health['epoch']=int(time.time());return 0
    def install(self,app,session,envelope,run_command,tasks,command_data):
        @app.get('/api/v1/devices/{device}/care-policy')
        def read_policy(device:str,user=Depends(session)):
            self.check(device,user);return envelope(self.report(device))
        def submit(device,body,user,name):
            self.check(device,user,True)
            actual=self.report(device)
            if not actual['supported']:raise HTTPException(409,'Firmware does not report inactivity settings; upgrade to v3.5.58')
            if actual['stale']:raise HTTPException(409,'Device settings are stale; wait for a fresh report')
            if actual['boot_id']!=body.boot_id:raise HTTPException(409,'Device restarted; reload its settings')
            paras=body.model_dump(exclude={'client_request_id'})
            payload={'service_id':self.settings.service_id,'command_name':name,'paras':paras}
            try:row,new=self.store.new_command(self.settings.mode,device,user['username'],'care:'+body.client_request_id,payload)
            except ValueError as exc:raise HTTPException(409,str(exc))
            if new:
                task=asyncio.create_task(run_command(row));tasks.add(task);task.add_done_callback(tasks.discard)
            return envelope(command_data(row),message='accepted' if new else 'existing request')
        @app.post('/api/v1/devices/{device}/care-policy',status_code=202)
        async def save_policy(device:str,body:CarePolicyInput,user=Depends(session)):
            return submit(device,body,user,'set_care_policy')
        @app.post('/api/v1/devices/{device}/care-pause',status_code=202)
        async def pause_policy(device:str,body:CarePauseInput,user=Depends(session)):
            return submit(device,body,user,'pause_care')
