import asyncio
from collections import defaultdict, deque
from contextlib import asynccontextmanager
from datetime import date, datetime, timezone
import hashlib
import hmac
import json
import time
import uuid
from typing import Literal

from fastapi import FastAPI, Depends, Header, HTTPException, Query, Request, WebSocket, WebSocketDisconnect
from fastapi.exceptions import RequestValidationError
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import JSONResponse
from pydantic import BaseModel, Field, ConfigDict, ValidationError, StrictBool, StrictInt

from .settings import Settings
from .store import Store, digest
from .cloud import HuaweiCloud, CloudError
from .medicine import LivingConfig, MedicinePlan, MedicineConfirmation, MedicineChange, LivingStatus, mock_living_command


def utc():
    return datetime.now(timezone.utc).isoformat().replace('+00:00','Z')

class StrictModel(BaseModel):
    model_config = ConfigDict(extra='forbid')

class Login(StrictModel):
    username: str = Field(min_length=1,max_length=64)
    password: str = Field(min_length=1,max_length=256)

class Command(StrictModel):
    device_id: str
    service_id: Literal['HomeSafety'] = 'HomeSafety'
    command_name: Literal['ack','mute','set_living','confirm_medicine','set_medicine_plan','set_medicine_state']
    paras: dict = Field(default_factory=dict)
    client_request_id: str = Field(min_length=1,max_length=128)

class Note(StrictModel):
    note: str = Field(min_length=1,max_length=2000)
    resolution: Literal['handled','false_alarm','follow_up']

class DeviceMessage(StrictModel):
    name: Literal['alarm_event','device_health','activity_stats']
    id: str | None = Field(default=None,max_length=256)
    content: dict

class Alarm(BaseModel):
    schema_version: int = Field(default=1,alias="schema")
    boot_id: int = Field(ge=0)
    event_id: int = Field(ge=1)
    code: int = Field(ge=1,le=5)
    value: float = Field(allow_inf_nan=False)
    phase: Literal['start','end','record']
    duration: int = Field(ge=0)
    epoch: int = Field(ge=0)
    up: int = Field(ge=0)

class Health(BaseModel):
    schema_version: int = Field(default=1,alias="schema")
    fw: str = Field(max_length=64)
    boot_id: int = Field(ge=0)
    epoch: int = Field(ge=0)
    up: int = Field(ge=0)
    env_valid: int = Field(ge=0,le=1)
    imu_valid: int = Field(ge=0,le=1)
    door_enabled: int = Field(ge=0,le=1)
    wireless_btn_enabled: int = Field(ge=0,le=1)
    mute: int = Field(ge=0,le=1)
    tls: bool
    living: LivingStatus | None = None
    door_valid: int | None = Field(default=None,ge=0,le=1)
    # Optional for older firmware; absence is not a healthy reading.
    sample_up: int | None = Field(default=None,ge=0)
    sample_age_s: int | None = Field(default=None,ge=-1)
    data_state: Literal['waiting','stale','ready'] | None = None
    data_stale: bool | None = None
    env_available: bool | None = None
    fall_detection_available: bool | None = None
    time_synced: bool | None = None

class DayStats(BaseModel):
    date: date
    activity_total: int = Field(ge=0)
    alarm_total: int = Field(ge=0)
    activity_by_hour: list[int] = Field(min_length=24,max_length=24)
    alarm_by_type: dict[str,int]

class Stats(BaseModel):
    model_config = ConfigDict(extra='allow')
    boot_id: int = Field(ge=0)
    uptime: int = Field(ge=0)
    time_synced: bool
    coverage: Literal['observed_only']
    days: list[DayStats] = Field(max_length=7)

class Hub:
    def __init__(self):
        self.clients = defaultdict(set)
    async def publish(self, device, event):
        for queue in tuple(self.clients[device]):
            if queue.full():
                while not queue.empty(): queue.get_nowait()
                queue.put_nowait({'type':'resync_required','device_id':device,'timestamp':utc()})
            else:
                queue.put_nowait(event)


def create_app(settings=None, cloud=None):
    settings = settings or Settings.load()
    settings.validate()
    store = Store(settings.db)
    store.bootstrap_user(settings.username,settings.password)
    provider = cloud if cloud is not None else HuaweiCloud(settings) if settings.mode=='live' else None
    hub = Hub()
    tasks = set()
    cloud_semaphore = asyncio.Semaphore(2)
    login_attempts = defaultdict(deque)

    def envelope(data=None,code=0,message='ok'):
        return {'code':code,'message':message,'data':data,'request_id':uuid.uuid4().hex,
                'mode':settings.mode,'simulated':settings.mode=='mock'}

    def check_device(device):
        if device != settings.device_id: raise HTTPException(404,'Device not configured')

    def session(authorization: str | None = Header(default=None)):
        if not authorization or not authorization.startswith('Bearer '): raise HTTPException(401,'Login required')
        row = store.session(digest(authorization[7:]))
        if not row: raise HTTPException(401,'Session expired or invalid')
        return row

    async def publish(device,kind,data):
        await hub.publish(device, {'schema':1,'type':kind,'device_id':device,'timestamp':utc(),
                                  'mode':settings.mode,'simulated':settings.mode=='mock','data':data})

    if settings.mode=='mock' and not store.doc('mock',settings.device_id,'shadow'):
        store.put('mock',settings.device_id,'shadow',{'device_id':settings.device_id,'service_id':'HomeSafety',
            'online':True,'updated_at':utc(),'properties':{'temp':25.5,'humi':60,'gas':20,'light':120,
            'body':0,'inactive':0,'alarm':0,'status':0,'help':0,'fall':0,'gas_alarm':0,'smoke':0,'door':0}})

    def command_data(row):
        body = json.loads(row['body'])
        return {'command_id':row['id'],'client_request_id':row['client_id'],'command_name':body['command_name'],
                'state':row['state'],'result_code':row['result'],'cloud_command_id':row['cloud_id'],
                'created_at':datetime.fromtimestamp(row['created'],timezone.utc).isoformat(),
                'updated_at':datetime.fromtimestamp(row['updated'],timezone.utc).isoformat()}

    async def run_command(row):
        async with cloud_semaphore:
            if settings.mode=='mock':
                await asyncio.sleep(0.15)
                shadow = store.doc('mock',row['device'],'shadow')
                props = shadow['properties']
                name = json.loads(row['body'])['command_name']
                if name=='ack':
                    props['help']=props['fall']=0
                    props['status']=3 if props.get('gas_alarm') else 4 if props.get('inactive',0)>60 else 5 if props.get('status')==5 else 0
                    props['alarm']=int(props['status']!=0)
                shadow['updated_at']=utc()
                store.put('mock',row['device'],'shadow',shadow)
                health = store.doc('mock',row['device'],'health') or {'schema':1,'fw':'mock','boot_id':0,'epoch':0,'up':0,
                    'env_valid':1,'imu_valid':0,'door_enabled':0,'wireless_btn_enabled':0,'mute':0,'tls':False}
                if name in ('ack','mute'): health['mute']=1
                result=0
                if name in ('set_living','confirm_medicine','set_medicine_plan','set_medicine_state'):
                    params=json.loads(row['body'])['paras']
                    life,result=mock_living_command(health.get('living'),name,params,int(time.time()),props.get('light',100),props.get('alarm',0))
                    health['living']=life
                    health['epoch']=int(time.time())
                    store.medicine_observation('mock',row['device'],health)
                store.put('mock',row['device'],'health',health)
                await publish(row['device'],'health',health)
                state,result,cloud_id = ('executed' if result==0 else 'failed'),result,None
            else:
                try:
                    command_body=json.loads(row['body'])
                    if command_body['paras']:
                        state,result,cloud_id = await asyncio.to_thread(provider.command,row['device'],command_body['command_name'],command_body['paras'])
                    else:
                        state,result,cloud_id = await asyncio.to_thread(provider.command,row['device'],command_body['command_name'])
                except Exception:
                    # Network timeout cannot establish whether the board executed a command.
                    state,result,cloud_id = 'unknown',None,None
            store.finish_command(row['id'],state,result,cloud_id)
            completed = store.command(row['id'],settings.mode)
            await publish(row['device'],'command_ack',command_data(completed))

    @asynccontextmanager
    async def lifespan(app):
        yield
        if tasks:
            await asyncio.gather(*tasks,return_exceptions=True)

    app = FastAPI(title='ElderCare App Backend',version='1.2.0',lifespan=lifespan)
    app.state.settings = settings
    app.state.store = store
    app.state.hub = hub
    if settings.cors:
        app.add_middleware(CORSMiddleware,allow_origins=settings.cors.split(','),
                           allow_methods=['GET','POST'],allow_headers=['Authorization','Content-Type'])

    @app.exception_handler(HTTPException)
    async def http_error(request,exc):
        return JSONResponse(envelope(code=exc.status_code,message=str(exc.detail)),status_code=exc.status_code)

    @app.exception_handler(RequestValidationError)
    async def validation_error(request,exc):
        # Do not echo submitted passwords or other input in an error response.
        return JSONResponse(envelope(code=422,message='Invalid request fields'),status_code=422)

    @app.get('/healthz')
    def healthz():
        return envelope({'status':'ready','cloud_configured':provider is not None,'push_mode':'ws'})

    @app.post('/api/v1/auth/login')
    def login(body: Login,request: Request):
        ip = request.client.host if request.client else 'unknown'
        attempts = login_attempts[ip]
        now = time.monotonic()
        while attempts and attempts[0]<now-60: attempts.popleft()
        if len(attempts)>=10: raise HTTPException(429,'Too many login attempts; retry later')
        attempts.append(now)
        token = store.login(body.username,body.password,settings.token_seconds)
        if not token: raise HTTPException(401,'Invalid username or password')
        return envelope({'access_token':token,'token_type':'Bearer','expires_in':settings.token_seconds,
                         'username':body.username,'device_ids':[settings.device_id]})

    @app.post('/api/v1/auth/logout')
    def logout(user=Depends(session)):
        store.logout(user['token_hash'])
        return envelope({'logged_out':True})

    @app.get('/api/v1/devices/{device}/shadow')
    async def shadow(device,user=Depends(session)):
        check_device(device)
        if settings.mode=='mock': data=store.doc(settings.mode,device,'shadow')
        else:
            try:
                data = await asyncio.to_thread(provider.shadow,device)
            except Exception:
                raise HTTPException(502,'IoTDA unavailable; no simulated fallback')
            store.put(settings.mode,device,'shadow',data)
        return envelope(data)

    @app.get('/api/v1/devices/{device}/health')
    def device_health(device,user=Depends(session)):
        check_device(device)
        return envelope(store.doc(settings.mode,device,'health'))

    @app.get('/api/v1/devices/{device}/medicine/history')
    def medicine_history(device,limit: int=Query(default=50,ge=1,le=200),user=Depends(session)):
        check_device(device)
        return envelope({'items':store.list_docs(settings.mode,device,'medicine_history',limit)})

    @app.post('/api/v1/commands',status_code=202)
    async def commands(body: Command,user=Depends(session)):
        check_device(body.device_id)
        if body.command_name in ('ack','mute') and body.paras:
            raise HTTPException(422,'ack/mute require paras:{}')
        try:
            if body.command_name == 'set_living': LivingConfig.model_validate(body.paras)
            if body.command_name == 'confirm_medicine': MedicineConfirmation.model_validate(body.paras)
            if body.command_name == 'set_medicine_plan': MedicinePlan.model_validate(body.paras)
            if body.command_name == 'set_medicine_state': MedicineChange.model_validate(body.paras)
        except ValidationError:
            raise HTTPException(422,'Invalid living reminder parameters')
        payload = {'service_id':body.service_id,'command_name':body.command_name,'paras':body.paras}
        try:
            row,new = store.new_command(settings.mode,body.device_id,user['username'],body.client_request_id,payload)
        except ValueError as exc: raise HTTPException(409,str(exc))
        if new:
            task = asyncio.create_task(run_command(row))
            tasks.add(task); task.add_done_callback(tasks.discard)
        return envelope(command_data(row),message='accepted' if new else 'existing request')

    @app.get('/api/v1/commands/{command_id}')
    def command_result(command_id,user=Depends(session)):
        row = store.command(command_id,settings.mode,settings.device_id)
        if not row or row['username']!=user['username']: raise HTTPException(404,'Command not found')
        return envelope(command_data(row))

    @app.post('/api/v1/ws-ticket')
    def ws_ticket(device_id: str=Query(...),user=Depends(session)):
        check_device(device_id)
        return envelope({'ticket':store.ticket(user['token_hash'],device_id),'expires_in':30,
                         'path':'/api/v1/ws','device_id':device_id})

    @app.websocket('/api/v1/ws')
    async def websocket(ws: WebSocket,device_id: str,ticket: str):
        if device_id!=settings.device_id:
            await ws.close(code=4404); return
        grant = store.consume_ticket(ticket,device_id)
        if not grant or not store.session(grant['session_hash']):
            await ws.close(code=4401); return
        await ws.accept()
        queue = asyncio.Queue(maxsize=100)
        hub.clients[device_id].add(queue)
        await ws.send_json({'schema':1,'type':'hello','device_id':device_id,'timestamp':utc(),
                           'mode':settings.mode,'simulated':settings.mode=='mock'})
        async def sender():
            while True:
                if not store.session(grant['session_hash']):
                    await ws.close(code=4401); return
                try: event=await asyncio.wait_for(queue.get(),timeout=15)
                except asyncio.TimeoutError: event={'type':'ping','timestamp':utc()}
                await ws.send_json(event)
                if event['type']=='resync_required':
                    await ws.close(code=1013); return
        async def receiver():
            while True:
                text = await ws.receive_text()
                if len(text)>1024:
                    await ws.close(code=1009); return
                # Client sends 'pong' or any short keepalive. No command execution here.
        send_task=asyncio.create_task(sender()); receive_task=asyncio.create_task(receiver())
        try:
            done,pending=await asyncio.wait([send_task,receive_task],return_when=asyncio.FIRST_COMPLETED)
            for task in done:
                try: task.result()
                except (WebSocketDisconnect,RuntimeError): pass
        finally:
            send_task.cancel(); receive_task.cancel()
            await asyncio.gather(send_task,receive_task,return_exceptions=True)
            hub.clients[device_id].discard(queue)

    async def ingest(device,message):
        check_device(device)
        try:
            model = {'alarm_event':Alarm,'device_health':Health,'activity_stats':Stats}[message.name]
            data = model.model_validate(message.content).model_dump(mode='json',by_alias=True)
        except ValidationError:
            raise HTTPException(422,'Invalid device message content')
        if message.name=='activity_stats':
            for day in data['days']:
                if any(x<0 for x in day['activity_by_hour']) or any(x<0 for x in day['alarm_by_type'].values()):
                    raise HTTPException(422,'Negative statistics are invalid')
        message_id = message.id or hashlib.sha256(json.dumps(message.model_dump(),sort_keys=True).encode()).hexdigest()
        with store.connection() as db:
            db.execute('BEGIN IMMEDIATE')
            cur=db.execute('INSERT OR IGNORE INTO inbox VALUES(?,?,?)',(settings.mode,device,message_id))
            if not cur.rowcount: return envelope({'duplicate':True})
            if message.name=='alarm_event':
                key=f"{data['boot_id']}:{data['event_id']}"
                old=store.doc(settings.mode,device,'alarm',key) or {}
                # An out-of-order start must not overwrite a recorded end.
                if old.get('phase')=='end' and data['phase']=='start': data=old
                else: data['message_id']=message_id
                store.put(settings.mode,device,'alarm',data,key,db)
            elif message.name=='device_health':
                row=db.execute('SELECT body FROM documents WHERE mode=? AND device=? AND kind=? AND key=?',(settings.mode,device,'health','latest')).fetchone()
                previous=json.loads(row['body']) if row else None
                if previous:
                    older=(data['boot_id'],data['up'])<(previous['boot_id'],previous['up'])
                    current_life=previous.get('living') or {};incoming_life=data.get('living') or {}
                    if data['boot_id']==previous['boot_id'] and current_life.get('schema')==2 and incoming_life.get('schema')==2:
                        older=older or incoming_life['medicine_revision']<current_life['medicine_revision'] or incoming_life['config_revision']<current_life['config_revision']
                    if older:return envelope({'duplicate':False,'accepted':False,'reason':'older health report'})
                store.put(settings.mode,device,'health',data,db=db)
                store.medicine_observation(settings.mode,device,data,db)
            else:
                store.put(settings.mode,device,'stats',data,db=db)
                if data['time_synced']:
                    for day in data['days']:
                        store.put(settings.mode,device,'day',day,day['date'],db)
        kind={'alarm_event':'alarm','device_health':'health','activity_stats':'stats'}[message.name]
        await publish(device,kind,data)
        return envelope({'duplicate':False,'accepted':True})

    @app.post('/api/v1/mock/messages')
    async def mock_message(message: DeviceMessage,device_id: str=Query(...),user=Depends(session)):
        if settings.mode!='mock': raise HTTPException(404,'Mock API disabled in live mode')
        return await ingest(device_id,message)

    @app.post('/api/v1/iotda/notifications')
    async def notifications(request: Request,token: str | None=Query(default=None),x_webhook_token: str | None=Header(default=None)):
        supplied=x_webhook_token or token or ''
        if not hmac.compare_digest(supplied,settings.webhook_token): raise HTTPException(401,'Webhook authorization required')
        if settings.mode!='live': raise HTTPException(409,'Live notifications disabled in mock mode')
        raw=await request.body()
        if len(raw)>262144: raise HTTPException(413,'Notification too large')
        try:
            payload=json.loads(raw)
            if payload.get('resource')!='device.message' or payload.get('event')!='report':
                raise ValueError()
            notify=payload['notify_data']
            device=notify['header']['device_id']
            content=notify['body']['content']
            if isinstance(content,str): content=json.loads(content)
            message=DeviceMessage.model_validate(content)
        except (KeyError,TypeError,ValueError,ValidationError):
            raise HTTPException(422,'Expected IoTDA device.message/report notification')
        return await ingest(device,message)

    @app.get('/api/v1/devices/{device}/alarms')
    def alarms(device,limit: int=Query(default=50,ge=1,le=100),offset: int=Query(default=0,ge=0),user=Depends(session)):
        check_device(device)
        return envelope({'items':store.list_docs(settings.mode,device,'alarm',limit,offset),'limit':limit,'offset':offset})

    @app.post('/api/v1/devices/{device}/alarms/{event_key}/notes',status_code=201)
    def add_note(device,event_key,body: Note,user=Depends(session)):
        check_device(device)
        if not store.doc(settings.mode,device,'alarm',event_key): raise HTTPException(404,'Alarm not found')
        with store.connection() as db:
            cur=db.execute('INSERT INTO notes(mode,device,event_key,username,note,resolution,created) VALUES(?,?,?,?,?,?,?)',
                           (settings.mode,device,event_key,user['username'],body.note,body.resolution,time.time()))
        return envelope({'note_id':cur.lastrowid,'event_key':event_key,'username':user['username']})

    @app.get('/api/v1/devices/{device}/alarms/{event_key}/notes')
    def notes(device,event_key,user=Depends(session)):
        check_device(device)
        with store.connection() as db:
            rows=db.execute('SELECT id,username,note,resolution,created FROM notes WHERE mode=? AND device=? AND event_key=? ORDER BY id',
                            (settings.mode,device,event_key)).fetchall()
        return envelope({'items':[dict(r) for r in rows]})

    @app.get('/api/v1/devices/{device}/stats')
    def stats(device,from_date: date | None=None,to_date: date | None=None,user=Depends(session)):
        check_device(device)
        if from_date and to_date and from_date>to_date: raise HTTPException(422,'Invalid date range')
        days=store.list_docs(settings.mode,device,'day',1000)
        days=[d for d in days if (not from_date or d['date']>=from_date.isoformat()) and (not to_date or d['date']<=to_date.isoformat())]
        return envelope({'latest':store.doc(settings.mode,device,'stats'),'days':sorted(days,key=lambda d:d['date']),
                         'coverage':'observed_only','activity_total':sum(d['activity_total'] for d in days),
                         'alarm_total':sum(d['alarm_total'] for d in days)})

    return app
