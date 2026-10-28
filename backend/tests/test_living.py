import tempfile
import time
import unittest
from pathlib import Path
from fastapi.testclient import TestClient
from backend.app import create_app, LivingStatus
from backend.settings import Settings

class LivingTests(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory()
        self.settings=Settings(db=str(Path(self.tmp.name)/'living.sqlite'),password='local-test-only-password',webhook_token='local-test-only-webhook-token')
        self.client=TestClient(create_app(self.settings))
        self.client.__enter__()
        token=self.client.post('/api/v1/auth/login',json={'username':'guardian','password':self.settings.password}).json()['data']['access_token']
        self.auth={'Authorization':'Bearer '+token}
    def tearDown(self):
        self.client.__exit__(None,None,None);self.tmp.cleanup()
    def send(self,name,params,key):
        return self.client.post('/api/v1/commands',headers=self.auth,json={'device_id':self.settings.device_id,'service_id':'HomeSafety','command_name':name,'paras':params,'client_request_id':key})
    def wait(self,response):
        self.assertEqual(response.status_code,202,response.text)
        path='/api/v1/commands/'+response.json()['data']['command_id']
        for _ in range(30):
            result=self.client.get(path,headers=self.auth).json()['data']
            if result['state']!='pending':return result
            time.sleep(.03)
        self.fail('command did not finish')
    def test_settings_conflict_confirmation_and_replay(self):
        params={'night_auto':True,'medicine_enabled':True,'medicine_minute':0,'expected_revision':1}
        result=self.wait(self.send('set_living',params,'cfg'))
        self.assertEqual(result['result_code'],0)
        day=(int(time.time())+28800)//86400
        self.assertEqual(self.wait(self.send('confirm_medicine',{'dose_day':day-1},'stale'))['result_code'],1)
        self.assertEqual(self.wait(self.send('confirm_medicine',{'dose_day':day},'confirm'))['result_code'],0)
        self.assertEqual(self.wait(self.send('confirm_medicine',{'dose_day':day},'again'))['result_code'],0)
        self.assertEqual(self.wait(self.send('set_living',params,'conflict'))['result_code'],4)
        response=self.client.get('/api/v1/devices/'+self.settings.device_id+'/health',headers=self.auth)
        life=LivingStatus.model_validate(response.json()['data']['living'])
        self.assertEqual((life.config_revision,life.medicine_state),(2,4))
    def test_strict_parameters(self):
        valid={'night_auto':True,'medicine_enabled':False,'medicine_minute':480,'expected_revision':1}
        for i,(key,value) in enumerate([('night_auto',1),('medicine_enabled','false'),('medicine_minute',1440),('medicine_minute',480.0),('expected_revision',0),('other',True)]):
            params=valid|{key:value}
            self.assertEqual(self.send('set_living',params,str(i)).status_code,422)
        self.assertEqual(self.send('set_living',{},'missing').status_code,422)
        self.assertEqual(self.send('confirm_medicine',{'dose_day':1.0},'float').status_code,422)
        self.assertEqual(self.send('ack',{'dose_day':1},'ack').status_code,422)

class CloudForwardingTests(unittest.TestCase):
    def test_live_command_keeps_exact_parameters(self):
        class Cloud:
            def command(self,device,name,paras=None):
                self.received=(device,name,paras)
                return 'executed',0,'living-cloud-test'
        with tempfile.TemporaryDirectory() as directory:
            settings=Settings(mode='live',db=str(Path(directory)/'cloud.sqlite'),password='local-test-only-password',webhook_token='local-test-only-webhook-token',ak='test',sk='test',project_id='test')
            cloud=Cloud()
            with TestClient(create_app(settings,cloud=cloud)) as client:
                token=client.post('/api/v1/auth/login',json={'username':'guardian','password':settings.password}).json()['data']['access_token']
                auth={'Authorization':'Bearer '+token}
                params={'night_auto':False,'medicine_enabled':True,'medicine_minute':510,'expected_revision':7}
                response=client.post('/api/v1/commands',headers=auth,json={'device_id':settings.device_id,'service_id':'HomeSafety','command_name':'set_living','paras':params,'client_request_id':'live-forward'})
                self.assertEqual(response.status_code,202,response.text)
                for _ in range(30):
                    data=client.get('/api/v1/commands/'+response.json()['data']['command_id'],headers=auth).json()['data']
                    if data['state']!='pending':break
                    time.sleep(.03)
                self.assertEqual(data['state'],'executed')
                self.assertEqual(cloud.received,(settings.device_id,'set_living',params))

class ThreeDoseTests(LivingTests):
    def test_independent_slots_undo_conflict_and_history(self):
        plan={'night_auto':True,'medicine_enabled':True,'medicine_minute':0,'medicine_enabled_2':True,'medicine_minute_2':0,'medicine_enabled_3':True,'medicine_minute_3':0,'expected_revision':1}
        self.assertEqual(self.wait(self.send('set_medicine_plan',plan,'three'))['result_code'],0)
        day=(int(time.time())+28800)//86400
        base={'dose_day':day,'slot':1,'taken':True,'expected_medicine_revision':1}
        self.assertEqual(self.wait(self.send('set_medicine_state',base,'take-second'))['result_code'],0)
        life=self.client.get('/api/v1/devices/'+self.settings.device_id+'/health',headers=self.auth).json()['data']['living']
        self.assertEqual([s['state'] for s in life['slots']],[3,4,3])
        LivingStatus.model_validate(life)
        self.assertEqual(self.wait(self.send('set_medicine_state',base|{'taken':False},'stale-undo'))['result_code'],4)
        undo=base|{'taken':False,'expected_medicine_revision':2}
        self.assertEqual(self.wait(self.send('set_medicine_state',undo,'undo'))['result_code'],0)
        history=self.client.get('/api/v1/devices/'+self.settings.device_id+'/medicine/history',headers=self.auth).json()['data']['items']
        second=[r['state'] for r in history if r['slot']==1]
        self.assertEqual(second,[3,4,3])
        # Retries reuse a request ID; they must not create another action or record.
        self.assertEqual(self.wait(self.send('set_medicine_state',undo,'undo'))['result_code'],0)
        history2=self.client.get('/api/v1/devices/'+self.settings.device_id+'/medicine/history',headers=self.auth).json()['data']['items']
        self.assertEqual(history2,history)
    def test_reported_state_history_and_out_of_order_health(self):
        from backend.medicine import mock_living_command
        from backend.store import Store
        epoch=int(time.time())
        plan={'night_auto':True,'medicine_enabled':True,'medicine_minute':0,'medicine_enabled_2':True,'medicine_minute_2':0,'medicine_enabled_3':False,'medicine_minute_3':1200,'expected_revision':1}
        life,_=mock_living_command(None,'set_medicine_plan',plan,epoch,100,False)
        life,_=mock_living_command(life,'set_medicine_state',{'dose_day':life['dose_day'],'slot':1,'taken':True,'expected_medicine_revision':1},epoch,100,False)
        health={'schema':1,'fw':'v3.5.0','boot_id':10,'epoch':epoch,'up':100,'env_valid':1,'imu_valid':1,'door_enabled':1,'wireless_btn_enabled':0,'mute':0,'tls':False,'living':life}
        path='/api/v1/mock/messages?device_id='+self.settings.device_id
        response=self.client.post(path,headers=self.auth,json={'name':'device_health','id':'health-new','content':health})
        self.assertEqual(response.status_code,200,response.text)
        self.assertTrue(response.json()['data']['accepted'])
        old=health|{'up':100,'living':life|{'medicine_revision':1}}
        response=self.client.post(path,headers=self.auth,json={'name':'device_health','id':'health-old','content':old})
        self.assertFalse(response.json()['data']['accepted'])
        current=self.client.get('/api/v1/devices/'+self.settings.device_id+'/health',headers=self.auth).json()['data']
        self.assertEqual(current['living']['slots'][1]['state'],4)
        self.assertEqual(current['up'],100)
        persisted=Store(self.settings.db).list_docs('mock',self.settings.device_id,'medicine_history')
        self.assertTrue(any(r['slot']==1 and r['state']==4 for r in persisted))

    def test_plan_and_state_strict_bounds(self):
        params={'dose_day':1,'slot':3,'taken':True,'expected_medicine_revision':1}
        self.assertEqual(self.send('set_medicine_state',params,'slot-range').status_code,422)
        self.assertEqual(self.send('set_medicine_state',params|{'slot':0,'taken':1},'bool').status_code,422)
        self.assertEqual(self.send('set_medicine_plan',{'night_auto':True},'missing-plan').status_code,422)
    def test_schema_one_and_invalid_schema_two(self):
        from pydantic import ValidationError
        life={'schema':1,'config_revision':1,'night_auto':True,'night_on':False,'light_valid':True,'light_owner':'off','medicine_enabled':False,'medicine_minute':480,'medicine_state':0,'dose_day':-1,'confirmed_epoch':0,'time_synced':False}
        LivingStatus.model_validate(life)
        with self.assertRaises(ValidationError):LivingStatus.model_validate(life|{'schema':2})
