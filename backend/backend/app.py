import asyncio
from collections import defaultdict, deque
from contextlib import asynccontextmanager
from datetime import date, datetime, timezone, timedelta
import hashlib
import hmac
import json
import os
from pathlib import Path
import time
import uuid
from typing import Literal

from fastapi import FastAPI, Depends, Header, HTTPException, Query, Request, WebSocket, WebSocketDisconnect
from fastapi.exceptions import RequestValidationError
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import JSONResponse, Response
from pydantic import BaseModel, Field, ConfigDict, ValidationError, StrictBool, StrictInt

from .settings import Settings
from .store import Store, digest
from .reports import build_report
from . import door as door_model
from .cloud import HuaweiCloud, CloudError
from . import watch as watch_model
from .medicine import LightControl, LivingConfig, MedicinePlan, MedicineConfirmation, MedicineChange, LivingStatus, mock_living_command


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
    command_name: Literal['ack','mute','set_living','confirm_medicine','set_medicine_plan','set_medicine_state','set_light']
    paras: dict = Field(default_factory=dict)
    client_request_id: str = Field(min_length=1,max_length=128)

class Note(StrictModel):
    note: str = Field(min_length=1,max_length=2000)
    resolution: Literal['handled','false_alarm','follow_up']

class AppSOS(StrictModel):
    client_request_id: str = Field(min_length=1,max_length=128)

class BatchNotes(Note):
    event_keys: list[str] = Field(min_length=1,max_length=100)

class DeviceMessage(StrictModel):
    name: Literal['alarm_event','device_health','activity_stats']
    id: str | None = Field(default=None,max_length=256)
    content: dict

class WatchService(BaseModel):
    # 手表物模型是产品侧自己建的，字段可能随固件增减，这里不锁死 extra。
    service_id: str = Field(min_length=1,max_length=64)
    properties: dict

class WatchReport(BaseModel):
    services: list[WatchService] = Field(min_length=1,max_length=16)

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
    # WS63 传感器节点快照（可选字段；通晓板固件不上报时缺省为 None，行为不变）
    light: float | None = Field(default=None,ge=0)                 # 光敏 ADC 电压 mV
    smoke: float | None = Field(default=None,ge=0)                 # 烟雾 ADC 电压 mV
    body: int | None = Field(default=None,ge=0,le=1)               # 人体感应 1=有人
    door: str | None = Field(default=None,max_length=16)           # 门磁 OPEN/CLOSED

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
    from .households import Households
    homes = Households(store, settings)
    provider = cloud if cloud is not None else HuaweiCloud(settings) if settings.mode=='live' else None
    hub = Hub()
    tasks = set()
    cloud_semaphore = asyncio.Semaphore(2)
    login_attempts = defaultdict(deque)

    def envelope(data=None,code=0,message='ok'):
        return {'code':code,'message':message,'data':data,'request_id':uuid.uuid4().hex,
                'mode':settings.mode,'simulated':settings.mode=='mock'}

    def check_device(device, user=None, control=False):
        if not settings.has_device(device): raise HTTPException(404,'Device not configured')
        if user is not None: homes.authorize(user['username'], device, control)

    def session(authorization: str | None = Header(default=None)):
        if not authorization or not authorization.startswith('Bearer '): raise HTTPException(401,'Login required')
        row = store.session(digest(authorization[7:]))
        if not row: raise HTTPException(401,'Session expired or invalid')
        return row

    async def publish(device,kind,data):
        await hub.publish(device, {'schema':1,'type':kind,'device_id':device,'timestamp':utc(),
                                  'mode':settings.mode,'simulated':settings.mode=='mock','data':data})

    if settings.mode=='mock':
        for _dev in settings.device_ids:
            if not store.doc('mock',_dev,'shadow'):
                store.put('mock',_dev,'shadow',{'device_id':_dev,'service_id':'HomeSafety',
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
                if name in ('set_living','confirm_medicine','set_medicine_plan','set_medicine_state','set_light'):
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

    app = FastAPI(title='ElderCare App Backend',version='1.8.1',lifespan=lifespan)
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
        return envelope({'status':'ready','cloud_configured':provider is not None,'push_mode':'ws',
                         'api_version':'1.8.1','camera_transport':'jpeg-relay'})

    homes.install(app, session, envelope)
    from .camera import CameraService
    cameras = CameraService(store, settings, homes)
    cameras.install(app, session, envelope)
    app.state.cameras = cameras

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
                         'username':body.username,'device_ids':homes.device_ids(body.username),
                         'devices':homes.describe(body.username)})

    @app.post('/api/v1/auth/logout')
    def logout(user=Depends(session)):
        store.logout(user['token_hash'])
        return envelope({'logged_out':True})

    @app.get('/api/v1/devices/{device}/shadow')
    async def shadow(device,user=Depends(session)):
        check_device(device,user)
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
        check_device(device,user)
        return envelope(store.doc(settings.mode,device,'health'))

    @app.get('/api/v1/devices/{device}/medicine/history')
    def medicine_history(device,limit: int=Query(default=50,ge=1,le=200),user=Depends(session)):
        check_device(device,user)
        return envelope({'items':store.list_docs(settings.mode,device,'medicine_history',limit)})

    # ===== 手表（WS63 穿戴节点）数据，2026-11-02 合并自队友版本 =====
    # 数据统一挂在主设备下（primary_device），URL 里的 device 只用于校验身份。
    @app.get('/api/v1/devices/{device}/watch')
    def watch_latest(device,user=Depends(session)):
        check_device(device,user)
        check_device(settings.primary_device,user)
        data = store.doc(settings.mode,settings.primary_device,'watch')
        if data is None: raise HTTPException(404,'No watch report yet')
        return envelope(data)

    @app.get('/api/v1/devices/{device}/watch/hours')
    def watch_hours(device,limit: int=Query(default=24,ge=1,le=720),user=Depends(session)):
        check_device(device,user)
        check_device(settings.primary_device,user)
        items = store.list_docs(settings.mode,settings.primary_device,'watch_hour',limit)
        for item in items:
            for name in ('hr','spo2'):
                agg = item.get(name)
                if agg: agg['avg'] = round(agg['sum']/agg['n'],1) if agg['n'] else None
        return envelope({'items':items,'limit':limit})

    @app.get('/api/v1/devices/{device}/watch/falls')
    def watch_falls(device,limit: int=Query(default=50,ge=1,le=200),user=Depends(session)):
        check_device(device,user)
        check_device(settings.primary_device,user)
        return envelope({'items':store.list_docs(settings.mode,settings.primary_device,'watch_fall',limit)})
    # ===== 手表接口结束 =====

    # ===== 视觉抓拍：事件触发取证（2026-10-22 设计，2026-10-24 上线） =====
    # 复用 documents 表（kind=capture_request / capture），不建表、不改 schema、零迁移。
    # 图片二进制落磁盘，数据库只存元数据。
    CAPTURE_DIR = Path(settings.db).parent / 'captures'
    CAPTURE_DIR.mkdir(parents=True, exist_ok=True)
    MAX_CAPTURE_BYTES = 4 * 1024 * 1024   # 单张上限 4MB（OV2640 VGA JPEG 通常 20~60KB）
    # 报警联动开关：默认关闭。固件 alarm.code(1..5) 的语义尚未在文档中固化，
    # 确认对应关系后写环境变量 CAPTURE_ALARM_CODES（如 "1,2"）即可启用，无需改代码。
    CAPTURE_ALARM_CODES = {int(x) for x in os.getenv('CAPTURE_ALARM_CODES','').split(',') if x.strip().isdigit()}

    class CaptureRequestIn(BaseModel):
        reason: str = Field(default='MANUAL', max_length=32)

    def request_capture_on_alarm(reason: str, device: str | None = None):
        """报警联动入口：标记一个待抓拍请求，ESP32-CAM 轮询到 pending 后拍照回传。
        多设备版：按触发报警的设备记账；device 缺失时记到第一台配置的设备。"""
        dev = device if (device and settings.has_device(device)) else settings.device_ids[0]
        req_id = uuid.uuid4().hex
        store.put(settings.mode, dev, 'capture_request', {
            'pending': True,
            'request_id': req_id,
            'reason': reason,
            'created_at': int(time.time()),
            'source': 'alarm',
        })
        return req_id

    @app.get('/api/v1/devices/{device}/capture-request')
    def get_capture_request(device,user=Depends(session)):
        """ESP32-CAM 轮询此接口；也供 App 查询当前是否有待抓拍。"""
        check_device(device,user)
        doc = store.doc(settings.mode,device,'capture_request') or {}
        return envelope({'pending':bool(doc.get('pending')),'request_id':doc.get('request_id'),
                         'reason':doc.get('reason'),'created_at':doc.get('created_at'),
                         'fulfilled_by':doc.get('fulfilled_by')})

    @app.post('/api/v1/devices/{device}/capture-request',status_code=202)
    def create_capture_request(device,body: CaptureRequestIn,user=Depends(session)):
        """App 主动请求抓拍（监护人想看一眼现场）。"""
        check_device(device,user,control=True)
        req_id = uuid.uuid4().hex
        store.put(settings.mode,device,'capture_request',{
            'pending':True,'request_id':req_id,'reason':body.reason,
            'created_at':int(time.time()),'source':'app'})
        return envelope({'request_id':req_id,'pending':True})

    @app.post('/api/v1/devices/{device}/captures',status_code=201)
    async def upload_capture(device,request: Request,user=Depends(session)):
        """ESP32-CAM 上传 JPEG。body 是裸二进制，理由与请求号走 header。"""
        check_device(device,user,control=True)
        raw = await request.body()
        if not raw:
            raise HTTPException(400,'empty body')
        if len(raw) > MAX_CAPTURE_BYTES:
            raise HTTPException(413,'capture too large')
        # JPEG 魔数 FF D8，防止传上来一堆乱码白占磁盘
        if len(raw) < 2 or not (raw[0] == 0xFF and raw[1] == 0xD8):
            raise HTTPException(415,'not a JPEG')
        req_id = request.headers.get('x-capture-request-id','')
        reason = (request.headers.get('x-capture-reason') or 'MANUAL')[:32]
        capture_id = uuid.uuid4().hex
        (CAPTURE_DIR / f'{capture_id}.jpg').write_bytes(raw)
        meta = {'capture_id':capture_id,'device':device,'reason':reason,'request_id':req_id,
                'bytes':len(raw),'created_at':int(time.time()),
                'file':f'{capture_id}.jpg'}   # 只存文件名，不暴露服务器绝对路径
        store.put(settings.mode,device,'capture',meta,key=capture_id)
        # 抓到了就清掉 pending，避免重复拍
        store.put(settings.mode,device,'capture_request',{
            'pending':False,'request_id':req_id,'reason':reason,
            'created_at':int(time.time()),'fulfilled_by':capture_id})
        try:
            await publish(device,'capture',meta)
        except Exception:
            pass
        return envelope(meta)

    @app.get('/api/v1/devices/{device}/captures')
    def list_captures(device,limit: int=Query(default=20,ge=1,le=200),user=Depends(session)):
        """抓拍历史（按时间倒序，已删除的不返回）。"""
        check_device(device,user)
        items=[x for x in store.list_docs(settings.mode,device,'capture',limit) if not x.get('deleted')]
        return envelope({'items':items})

    @app.get('/api/v1/devices/{device}/captures/{capture_id}')
    def get_capture_image(device,capture_id,user=Depends(session)):
        """取图片本体，直接返回 image/jpeg。"""
        check_device(device,user)
        meta = store.doc(settings.mode,device,'capture',key=capture_id)
        if not meta or meta.get('deleted'):
            raise HTTPException(404,'capture not found')
        p = CAPTURE_DIR / str(meta.get('file',''))
        # 防止 file 字段被篡改成 ../ 越权读取
        if p.resolve().parent != CAPTURE_DIR.resolve() or not p.exists():
            raise HTTPException(404,'capture file missing')
        return Response(content=p.read_bytes(),media_type='image/jpeg',
                        headers={'Cache-Control':'private, max-age=60'})

    @app.delete('/api/v1/devices/{device}/captures/{capture_id}')
    def delete_capture(device,capture_id,user=Depends(session)):
        """隐私开关：单张删除（批量删就是循环调这个）。"""
        check_device(device,user,control=True)
        meta = store.doc(settings.mode,device,'capture',key=capture_id)
        if not meta or meta.get('deleted'):
            raise HTTPException(404,'capture not found')
        p = CAPTURE_DIR / str(meta.get('file',''))
        if p.resolve().parent == CAPTURE_DIR.resolve() and p.exists():
            p.unlink()
        store.put(settings.mode,device,'capture',{'capture_id':capture_id,'deleted':True,
                  'deleted_at':int(time.time())},key=capture_id)
        return envelope({'capture_id':capture_id,'deleted':True})

    # ===== 视觉抓拍结束 =====

    @app.post('/api/v1/commands',status_code=202)
    async def commands(body: Command,user=Depends(session)):
        check_device(body.device_id,user,control=True)
        if body.device_id != settings.primary_device: raise HTTPException(422,"This device does not support gateway commands")
        if body.command_name in ('ack','mute') and body.paras:
            raise HTTPException(422,'ack/mute require paras:{}')
        try:
            if body.command_name == 'set_light': LightControl.model_validate(body.paras)
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
        row = store.command(command_id,settings.mode)
        if not row or row['username']!=user['username'] or not settings.has_device(row['device']):
            raise HTTPException(404,'Command not found')
        return envelope(command_data(row))

    @app.post('/api/v1/ws-ticket')
    def ws_ticket(device_id: str=Query(...),user=Depends(session)):
        check_device(device_id,user)
        return envelope({'ticket':store.ticket(user['token_hash'],device_id),'expires_in':30,
                         'path':'/api/v1/ws','device_id':device_id})

    @app.websocket('/api/v1/ws')
    async def websocket(ws: WebSocket,device_id: str,ticket: str):
        if device_id not in homes.configured_ids():
            await ws.close(code=4404); return
        grant = store.consume_ticket(ticket,device_id)
        if not grant or not store.session(grant['session_hash']):
            await ws.close(code=4401); return
        try:
            homes.authorize(store.session(grant['session_hash'])['username'],device_id)
        except HTTPException:
            await ws.close(code=4403); return
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

    async def ingest_nearlink(device,payload):
        services=payload.get('services') or [payload]
        service=next((item for item in services if isinstance(item,dict) and isinstance(item.get('properties'),dict) and any(k in item['properties'] for k in ('light','smoke','body','door'))),None)
        if service is None: raise HTTPException(422,'No NearLink properties')
        import math
        props={}
        for key,value in service['properties'].items():
            if key in ('temp','humi','light','smoke'):
                if isinstance(value,bool) or not isinstance(value,(int,float)) or not math.isfinite(value): raise HTTPException(422,'Invalid sensor value')
                props[key]=value
            elif key=='body':
                if type(value) is not int or value not in (0,1): raise HTTPException(422,'Invalid body value')
                props[key]=value
            elif key=='door':
                if value not in ('OPEN','CLOSED'): raise HTTPException(422,'Invalid door value')
                props[key]=value
        door_model.observe(store,settings.mode,device,props,time.time())
        old=store.doc(settings.mode,device,'shadow') or {'properties':{}}
        old['properties'].update(props)
        old.update(device_id=device,service_id=service.get('service_id',''),updated_at=utc(),online=True)
        store.put(settings.mode,device,'shadow',old)
        await publish(device,'properties',old)
        return envelope({'accepted':True})

    async def ingest_watch(device,payload):
        """手表属性上报入库：最新值 + 小时桶 + 摔倒事件，并实时推给 App。

        手表数据统一挂在**主设备**（device_ids 第一台）下：App 只面向一台设备，
        不必区分两颗芯片。这里用 primary_device 而不是 settings.device_id ——
        后者在配了多台设备时是逗号分隔的字符串，直接当 device 用会写出脏键。
        """
        check_device(device)
        source_device = device
        target = settings.primary_device
        services = payload.get('services')
        if services is None and 'properties' in payload:
            services = [payload]        # IoTDA 通知里 body 是单条 service
        service = watch_model.pick_service(services,settings.watch_service_id)
        if service is None:
            raise HTTPException(422,'No watch service in property report')
        try:
            report = watch_model.normalize(service.get('properties'),service.get('service_id',''),utc())
        except watch_model.WatchReportError:
            raise HTTPException(422,'Invalid watch properties')
        previous = store.doc(settings.mode,target,'watch') or {}
        store.put(settings.mode,target,'watch',report)
        hour = store.watch_hour(settings.mode,target,report)
        if source_device != target:
            store.put(settings.mode,source_device,'watch',report)
            await publish(source_device,'watch',report)
        # fall_count 只增不减，差值就是这一条上报里新增的摔倒判定
        known = previous.get('fall_count') or 0
        events = []
        delta=max(0,report['fall_count']-known)
        if delta:
            sequence=store.doc(settings.mode,target,'watch_history_sequence') or {'value':0}
            sequence['value']+=1
            store.put(settings.mode,target,'watch_history_sequence',sequence)
            event = {'kind':'watch_fall','fall_count':report['fall_count'],'new_count':delta,
                     'detected_at':report['received_at'],'time_basis':'received_at',
                     'heart_rate':report['heart_rate'],'spo2':report['spo2'],
                     'motion_mg':report['motion_mg'],'steps':report['steps']}
            # A disconnected counter jump is one observed batch, not thousands
            # of incidents with invented individual timestamps. Reboots cannot overwrite it.
            store.put(settings.mode,target,'watch_fall',event,'batch:'+str(sequence['value']))
            events.append(event)
        wearable_alarm = None
        if (report['fall_active'] and not previous.get('fall_active')) or (bool(previous) and bool(events)):
            # New incidents use a persisted sequence; a rebooted counter cannot collide.
            seq = store.doc(settings.mode,target,'watch_alarm_sequence') or {'value':0}
            seq['value'] += 1
            store.put(settings.mode,target,'watch_alarm_sequence',seq)
            wearable_alarm = {'boot_id':2147483647,'event_id':seq['value'],'code':2,
                              'phase':'start','epoch':int(time.time()),'up':0,'duration':0,
                              'value':report['motion_mg'],'source':'wearable','source_device_id':source_device}
            store.put(settings.mode,target,'alarm',wearable_alarm,f"2147483647:{seq['value']}")
        await publish(target,'watch',report)
        if wearable_alarm: await publish(target,'alarm',wearable_alarm)
        for event in events:
            await publish(target,'watch_fall',event)
        return envelope({'accepted':True,'fall_events':len(events),'hour':hour['hour'],'device':target})

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
        # 报警联动（默认关闭）：报警开始时刻标记待抓拍，ESP32-CAM 轮询到后取证。
        if message.name=='alarm_event' and CAPTURE_ALARM_CODES and data.get('phase')=='start' and data.get('code') in CAPTURE_ALARM_CODES:
            try: request_capture_on_alarm('ALARM%d'%data['code'], device)
            except Exception: pass
        kind={'alarm_event':'alarm','device_health':'health','activity_stats':'stats'}[message.name]
        await publish(device,kind,data)
        return envelope({'duplicate':False,'accepted':True})

    @app.post('/api/v1/mock/watch')
    async def mock_watch(report: WatchReport,device_id: str=Query(...),user=Depends(session)):
        if settings.mode!='mock': raise HTTPException(404,'Mock API disabled in live mode')
        check_device(device_id,user,control=True)
        return await ingest_watch(device_id,report.model_dump())

    @app.post('/api/v1/mock/messages')
    async def mock_message(message: DeviceMessage,device_id: str=Query(...),user=Depends(session)):
        if settings.mode!='mock': raise HTTPException(404,'Mock API disabled in live mode')
        check_device(device_id,user,control=True)
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
            resource=payload.get('resource')
            if payload.get('event')!='report' or resource not in ('device.message','device.property'):
                raise ValueError()
            notify=payload['notify_data']
            device=notify['header']['device_id']
            body=notify['body']
        except (KeyError,TypeError,ValueError,AttributeError):
            raise HTTPException(422,'Expected IoTDA device.message/device.property report')
        if resource=='device.property':
            # 属性上报只服务手表：
            #   - 未知设备 -> 404（与其它接口一致，便于排障）
            #   - 已知但非手表（如 WS63 环境节点也发属性上报）-> 忽略并返回 200，
            #     避免它每 10 秒一条产生 422 噪音和死信堆积
            if not settings.has_device(device):
                raise HTTPException(404,'Device not configured')
            if settings.nearlink_device_id and device == settings.nearlink_device_id:
                return await ingest_nearlink(device,body if isinstance(body,dict) else json.loads(body))
            if not settings.is_watch(device):
                return envelope({'ignored':True,'reason':'property report from non-watch device'})
            return await ingest_watch(device,body if isinstance(body,dict) else json.loads(body))
        try:
            content=body['content']
            if isinstance(content,str): content=json.loads(content)
            message=DeviceMessage.model_validate(content)
        except (KeyError,TypeError,ValueError,ValidationError):
            raise HTTPException(422,'Expected IoTDA device.message/report notification')
        return await ingest(device,message)

    @app.post('/api/v1/devices/{device}/sos',status_code=201)
    async def app_sos(device,body: AppSOS,user=Depends(session)):
        check_device(device,user,control=True)
        # Stable across retries and isolated from physical boot/event identifiers.
        event_id=int(hashlib.sha256((user['username']+':'+body.client_request_id).encode()).hexdigest()[:12],16)
        key=f"2147483647:{event_id}"
        with store.connection() as db:
            db.execute('BEGIN IMMEDIATE')
            row=db.execute('SELECT body FROM documents WHERE mode=? AND device=? AND kind=? AND key=?',
                           (settings.mode,device,'alarm',key)).fetchone()
            created=row is None
            data=json.loads(row['body']) if row else {'boot_id':2147483647,'event_id':event_id,'code':1,
                'phase':'start','epoch':int(time.time()),'up':0,'duration':0,'value':0,
                'source':'app','requested_by':user['username']}
            if created: store.put(settings.mode,device,'alarm',data,key,db)
        if created: await publish(device,'alarm',data)
        return envelope(data)

    @app.post('/api/v1/devices/{device}/alarms/batch-notes')
    async def batch_notes(device,body: BatchNotes,user=Depends(session)):
        check_device(device,user,control=True)
        keys=list(dict.fromkeys(body.event_keys))
        changed=[]
        with store.connection() as db:
            db.execute('BEGIN IMMEDIATE')
            # Validate entire snapshot before writing any notes.
            for key in keys:
                if not db.execute('SELECT 1 FROM documents WHERE mode=? AND device=? AND kind=? AND key=?',
                                  (settings.mode,device,'alarm',key)).fetchone():
                    raise HTTPException(404,'Alarm not found')
            for key in keys:
                prior=db.execute('SELECT resolution FROM notes WHERE mode=? AND device=? AND event_key=? ORDER BY id DESC LIMIT 1',
                                 (settings.mode,device,key)).fetchone()
                if prior and prior['resolution']!='follow_up': continue
                db.execute('INSERT INTO notes(mode,device,event_key,username,note,resolution,created) VALUES(?,?,?,?,?,?,?)',
                           (settings.mode,device,key,user['username'],body.note,body.resolution,time.time()))
                changed.append(key)
        for key in changed: await publish(device,'alarm_note',{'event_key':key,'resolution':body.resolution})
        return envelope({'handled_keys':changed,'requested_count':len(keys)})

    @app.get('/api/v1/devices/{device}/door-records')
    def door_records(device,limit: int=Query(default=30,ge=1,le=100),user=Depends(session)):
        check_device(device,user)
        state=store.doc(settings.mode,device,'door_context') or {}
        return envelope({'summary':door_model.summarize(state,time.time()),
                         'items':store.list_docs(settings.mode,device,'door_record',limit)})

    @app.get('/api/v1/devices/{device}/alarms')
    def alarms(device,limit: int=Query(default=50,ge=1,le=100),offset: int=Query(default=0,ge=0),user=Depends(session)):
        check_device(device,user)
        items=store.list_docs(settings.mode,device,'alarm',limit,offset)
        with store.connection() as db:
            for item in items:
                key=f"{item['boot_id']}:{item['event_id']}"
                note=db.execute('SELECT resolution,username,created FROM notes WHERE mode=? AND device=? AND event_key=? ORDER BY id DESC LIMIT 1',
                                (settings.mode,device,key)).fetchone()
                if note: item['handling']=dict(note)
        return envelope({'items':items,'limit':limit,'offset':offset})

    @app.post('/api/v1/devices/{device}/alarms/{event_key}/notes',status_code=201)
    async def add_note(device,event_key,body: Note,user=Depends(session)):
        check_device(device,user,control=True)
        if not store.doc(settings.mode,device,'alarm',event_key): raise HTTPException(404,'Alarm not found')
        with store.connection() as db:
            cur=db.execute('INSERT INTO notes(mode,device,event_key,username,note,resolution,created) VALUES(?,?,?,?,?,?,?)',
                           (settings.mode,device,event_key,user['username'],body.note,body.resolution,time.time()))
        await publish(device,'alarm_note',{'event_key':event_key,'resolution':body.resolution})
        return envelope({'note_id':cur.lastrowid,'event_key':event_key,'username':user['username']})

    @app.get('/api/v1/devices/{device}/alarms/{event_key}/notes')
    def notes(device,event_key,user=Depends(session)):
        check_device(device,user)
        with store.connection() as db:
            rows=db.execute('SELECT id,username,note,resolution,created FROM notes WHERE mode=? AND device=? AND event_key=? ORDER BY id',
                            (settings.mode,device,event_key)).fetchall()
        return envelope({'items':[dict(r) for r in rows]})

    @app.get('/api/v1/devices/{device}/reports')
    def reports(device,user=Depends(session)):
        check_device(device,user)
        today=datetime.now(timezone(timedelta(hours=8))).date()
        return envelope(build_report(store.list_docs(settings.mode,device,'day',1000),today))

    @app.get('/api/v1/devices/{device}/stats')
    def stats(device,from_date: date | None=None,to_date: date | None=None,user=Depends(session)):
        check_device(device,user)
        if from_date and to_date and from_date>to_date: raise HTTPException(422,'Invalid date range')
        days=store.list_docs(settings.mode,device,'day',1000)
        days=[d for d in days if (not from_date or d['date']>=from_date.isoformat()) and (not to_date or d['date']<=to_date.isoformat())]
        return envelope({'latest':store.doc(settings.mode,device,'stats'),'days':sorted(days,key=lambda d:d['date']),
                         'coverage':'observed_only','activity_total':sum(d['activity_total'] for d in days),
                         'alarm_total':sum(d['alarm_total'] for d in days)})

    return app
