import tempfile
import time
import unittest
from pathlib import Path
from fastapi.testclient import TestClient
from backend.app import create_app
from backend.settings import Settings

class FakeCloud:
    def __init__(self): self.calls=0
    def shadow(self,device):
        return {'device_id':device,'service_id':'HomeSafety','online':True,'updated_at':None,'properties':{'temp':26}}
    def command(self,device,name):
        self.calls+=1
        return 'executed',0,'cloud-001'

class BrokenCloud(FakeCloud):
    def shadow(self,device): raise RuntimeError('unavailable')
    def command(self,device,name): raise RuntimeError('timeout')

class BackendTests(unittest.TestCase):
    def setUp(self):
        self.directory=tempfile.TemporaryDirectory()
        self.settings=Settings(db=str(Path(self.directory.name)/'test.sqlite3'),password='local-test-password-only',webhook_token='webhook-test-secret-value-only')
        self.app=create_app(self.settings)
        self.client=TestClient(self.app)
        self.client.__enter__()
        response=self.client.post('/api/v1/auth/login',json={'username':'guardian','password':self.settings.password})
        self.assertEqual(response.status_code,200)
        self.token=response.json()['data']['access_token']
        self.auth={'Authorization':'Bearer '+self.token}
        self.device=self.settings.device_id
    def tearDown(self):
        self.client.__exit__(None,None,None)
        self.directory.cleanup()
    def command(self,key='one',name='ack'):
        return {'device_id':self.device,'service_id':'HomeSafety','command_name':name,'paras':{},'client_request_id':key}
    def alarm(self,phase='start'):
        return {'name':'alarm_event','id':'alarm-'+phase,'content':{'boot_id':47,'event_id':27,'code':1,'value':0,'phase':phase,'duration':10 if phase=='end' else 0,'epoch':0,'up':30}}
    def test_auth_and_scope(self):
        self.assertEqual(self.client.get('/api/v1/devices/'+self.device+'/shadow').status_code,401)
        self.assertEqual(self.client.get('/api/v1/devices/other/shadow',headers=self.auth).status_code,404)
        response=self.client.post('/api/v1/auth/login',json={'username':'guardian','password':'wrong-password'})
        self.assertEqual(response.status_code,401)
        bad=self.client.post('/api/v1/auth/login',json={'username':'guardian','password':self.settings.password,'other':1})
        self.assertNotIn(self.settings.password,bad.text)
        self.assertTrue(self.client.get('/api/v1/devices/'+self.device+'/shadow',headers=self.auth).json()['simulated'])
        self.client.post('/api/v1/auth/logout',headers=self.auth)
        self.assertEqual(self.client.get('/api/v1/devices/'+self.device+'/shadow',headers=self.auth).status_code,401)
    def test_command_idempotency_and_receipt(self):
        response=self.client.post('/api/v1/commands',headers=self.auth,json=self.command())
        self.assertEqual(response.status_code,202)
        data=response.json()['data']; self.assertEqual(data['state'],'pending')
        duplicate=self.client.post('/api/v1/commands',headers=self.auth,json=self.command()).json()['data']
        self.assertEqual(data['command_id'],duplicate['command_id'])
        self.assertEqual(self.client.post('/api/v1/commands',headers=self.auth,json=self.command(name='mute')).status_code,409)
        time.sleep(0.3)
        result=self.client.get('/api/v1/commands/'+data['command_id'],headers=self.auth).json()['data']
        self.assertEqual((result['state'],result['result_code']),('executed',0))
        invalid=self.command('invalid'); invalid['paras']={'enabled':1}
        self.assertEqual(self.client.post('/api/v1/commands',headers=self.auth,json=invalid).status_code,422)
    def test_message_dedup_end_before_start_notes(self):
        path='/api/v1/mock/messages?device_id='+self.device
        self.assertEqual(self.client.post(path,headers=self.auth,json=self.alarm('end')).status_code,200)
        self.assertTrue(self.client.post(path,headers=self.auth,json=self.alarm('end')).json()['data']['duplicate'])
        self.client.post(path,headers=self.auth,json=self.alarm('start'))
        items=self.client.get('/api/v1/devices/'+self.device+'/alarms',headers=self.auth).json()['data']['items']
        self.assertEqual(len(items),1); self.assertEqual(items[0]['phase'],'end')
        note_path='/api/v1/devices/'+self.device+'/alarms/47:27/notes'
        self.assertEqual(self.client.post(note_path,headers=self.auth,json={'note':'已联系家属','resolution':'handled'}).status_code,201)
        self.assertEqual(self.client.get(note_path,headers=self.auth).json()['data']['items'][0]['note'],'已联系家属')
    def test_stats_are_snapshots_not_repeated_sums(self):
        day={'date':'2026-10-14','activity_total':12,'alarm_total':1,'activity_by_hour':[0]*24,'alarm_by_type':{'HELP':1}}
        msg={'name':'activity_stats','id':'stats-1','content':{'boot_id':47,'uptime':60,'time_synced':True,'coverage':'observed_only','days':[day]}}
        path='/api/v1/mock/messages?device_id='+self.device
        self.assertEqual(self.client.post(path,headers=self.auth,json=msg).status_code,200)
        msg['id']='stats-2'
        self.client.post(path,headers=self.auth,json=msg)
        data=self.client.get('/api/v1/devices/'+self.device+'/stats?from_date=2026-10-01&to_date=2026-10-31',headers=self.auth).json()['data']
        self.assertEqual(data['activity_total'],12); self.assertEqual(len(data['days']),1)
    def test_websocket_ticket_push_and_reuse(self):
        ticket=self.client.post('/api/v1/ws-ticket?device_id='+self.device,headers=self.auth).json()['data']['ticket']
        url='/api/v1/ws?device_id='+self.device+'&ticket='+ticket
        with self.client.websocket_connect(url) as ws:
            self.assertEqual(ws.receive_json()['type'],'hello')
            self.client.post('/api/v1/mock/messages?device_id='+self.device,headers=self.auth,json=self.alarm())
            self.assertEqual(ws.receive_json()['type'],'alarm')
        from starlette.websockets import WebSocketDisconnect
        with self.assertRaises(WebSocketDisconnect):
            with self.client.websocket_connect(url): pass
    def test_live_mode_no_mock_fallback_and_webhook_authorization(self):
        live=Settings(mode='live',db=self.settings.db,password=self.settings.password,webhook_token=self.settings.webhook_token,ak='test-ak',sk='test-sk',project_id='test-project')
        with TestClient(create_app(live,BrokenCloud())) as client:
            token=client.post('/api/v1/auth/login',json={'username':'guardian','password':live.password}).json()['data']['access_token']
            auth={'Authorization':'Bearer '+token}
            result=client.get('/api/v1/devices/'+self.device+'/shadow',headers=auth)
            self.assertEqual(result.status_code,502); self.assertFalse(result.json()['simulated'])
            self.assertEqual(client.post('/api/v1/mock/messages?device_id='+self.device,headers=auth,json=self.alarm()).status_code,404)
            notification={'resource':'device.message','event':'report','notify_data':{'header':{'device_id':self.device},'body':{'content':self.alarm()}}}
            self.assertEqual(client.post('/api/v1/iotda/notifications',json=notification).status_code,401)
            self.assertEqual(client.post('/api/v1/iotda/notifications',headers={'X-Webhook-Token':live.webhook_token},json=notification).status_code,200)
            self.assertEqual(len(client.get('/api/v1/devices/'+self.device+'/alarms',headers=auth).json()['data']['items']),1)
            command=client.post('/api/v1/commands',headers=auth,json=self.command('live')).json()['data']
            time.sleep(0.1)
            result=client.get('/api/v1/commands/'+command['command_id'],headers=auth).json()['data']
            self.assertEqual(result['state'],'unknown'); self.assertIsNone(result['result_code'])
    def test_firmware_health_quality_preserved(self):
        content={'fw':'v3.3.8','boot_id':50,'epoch':0,'up':21,'env_valid':1,'imu_valid':0,
                 'door_enabled':0,'wireless_btn_enabled':0,'mute':0,'tls':False,
                 'sample_up':10,'sample_age_s':11,'data_state':'stale','data_stale':True,
                 'env_available':False,'fall_detection_available':False,'time_synced':False}
        message={'name':'device_health','id':'health-quality','content':content}
        result=self.client.post('/api/v1/mock/messages?device_id='+self.device,headers=self.auth,json=message)
        self.assertEqual(result.status_code,200)
        data=self.client.get('/api/v1/devices/'+self.device+'/health',headers=self.auth).json()['data']
        for name in ('sample_age_s','data_state','data_stale','env_available','fall_detection_available','time_synced'):
            self.assertEqual(data[name],content[name])
    def test_restart_marks_unfinished_command_unknown(self):
        self.app.state.store.new_command('mock',self.device,'guardian','interrupted',{'command_name':'ack','service_id':'HomeSafety','paras':{}})
        second=create_app(self.settings)
        with second.state.store.connection() as db:
            row=db.execute("SELECT state FROM commands WHERE client_id='interrupted'").fetchone()
            self.assertEqual(row['state'],'unknown')

if __name__=='__main__': unittest.main(verbosity=2)
