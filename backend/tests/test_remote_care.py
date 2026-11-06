import asyncio
import tempfile
import unittest
import httpx
from backend.app import create_app
from backend.settings import Settings

CARE={'enabled':True,'sleep_enabled':False,'start':1320,'end':420,'quiet':0,'revision':1,'noact_ms':1800000,'mode':1,'time_synced':True,'pause_seconds':0,'pause_revision':1}
def health(care=None):return {'schema':1,'fw':'v3.5.58','boot_id':7,'epoch':1700000000,'up':100,'env_valid':1,'imu_valid':0,'door_enabled':0,'wireless_btn_enabled':0,'mute':0,'tls':False,'care_policy':dict(care or CARE),'contact_text':True}
def policy(**extra):return {'boot_id':7,'enabled':True,'sleep_enabled':True,'start':1320,'end':420,'quiet':1,'noact_ms':600000,'expected_revision':1,'client_request_id':'care-one',**extra}
class RemoteCareTests(unittest.TestCase):
    def test_versioned_save_pause_permissions_and_stale(self):
        async def run():
            with tempfile.TemporaryDirectory() as tmp:
                app=create_app(Settings(db=tmp+'/x.db',password='test-password',webhook_token='x'*32,device_id='main'))
                async with httpx.AsyncClient(transport=httpx.ASGITransport(app=app),base_url='http://test') as c:
                    async def login(name='guardian',pw='test-password'):
                        r=await c.post('/api/v1/auth/login',json={'username':name,'password':pw});return {'Authorization':'Bearer '+r.json()['data']['access_token']}
                    owner=await login();root='/api/v1/devices/main/care-policy'
                    self.assertEqual((await c.get(root)).status_code,401)
                    self.assertFalse((await c.get(root,headers=owner)).json()['data']['supported'])
                    self.assertEqual((await c.post(root,headers=owner,json=policy())).status_code,409)
                    r=await c.post('/api/v1/mock/messages?device_id=main',headers=owner,json={'name':'device_health','content':health()});self.assertEqual(r.status_code,200,r.text)
                    await c.post('/api/v1/household/members',headers=owner,json={'username':'viewer','password':'viewer-pass','role':'viewer'});viewer=await login('viewer','viewer-pass')
                    self.assertEqual((await c.get(root,headers=viewer)).status_code,200);self.assertEqual((await c.post(root,headers=viewer,json=policy())).status_code,403)
                    self.assertEqual((await c.post(root,headers=owner,json=policy(start=420))).status_code,422)
                    self.assertEqual((await c.post(root,headers=owner,json=policy(boot_id=8))).status_code,409)
                    r=await c.post(root,headers=owner,json=policy());self.assertEqual(r.status_code,202,r.text);ident=r.json()['data']['command_id']
                    same=await c.post(root,headers=owner,json=policy());self.assertEqual(same.json()['data']['command_id'],ident)
                    self.assertEqual((await c.post(root,headers=owner,json=policy(noact_ms=900000))).status_code,409)
                    await asyncio.sleep(.25);value=(await c.get(root,headers=owner)).json()['data'];self.assertEqual(value['actual']['revision'],2);self.assertEqual(value['actual']['noact_ms'],600000)
                    r=await c.post(root,headers=owner,json=policy(client_request_id='stale-revision'));await asyncio.sleep(.25);result=(await c.get('/api/v1/commands/'+r.json()['data']['command_id'],headers=owner)).json()['data'];self.assertEqual(result['state'],'failed');self.assertEqual(result['result_code'],4)
                    pause={'boot_id':7,'minutes':30,'expected_pause_revision':1,'client_request_id':'pause-one'}
                    r=await c.post('/api/v1/devices/main/care-pause',headers=owner,json=pause);self.assertEqual(r.status_code,202);await asyncio.sleep(.25)
                    value=(await c.get(root,headers=owner)).json()['data'];self.assertEqual(value['actual']['pause_seconds'],1800);self.assertEqual(value['actual']['pause_revision'],2)
                    r=await c.post('/api/v1/devices/main/care-pause',headers=owner,json=pause);self.assertEqual(r.status_code,202);await asyncio.sleep(.05);self.assertEqual((await c.get(root,headers=owner)).json()['data']['actual']['pause_revision'],2)
                    with app.state.store.connection() as db:db.execute("UPDATE documents SET updated=0 WHERE kind='health'")
                    self.assertTrue((await c.get(root,headers=owner)).json()['data']['stale']);self.assertEqual((await c.post(root,headers=owner,json=policy())).status_code,409)
        asyncio.run(run())
    def test_live_receipt_does_not_invent_reported_settings(self):
        class Cloud:
            def __init__(self):self.sent=[]
            def command(self,device,name,p):self.sent.append((device,name,p));return 'executed',0,'fake'
        async def run():
            with tempfile.TemporaryDirectory() as tmp:
                cloud=Cloud();app=create_app(Settings(mode='live',db=tmp+'/x.db',password='test-password',webhook_token='x'*32,device_id='main',ak='x',sk='y',project_id='p'),cloud=cloud)
                app.state.store.put('live','main','health',health())
                async with httpx.AsyncClient(transport=httpx.ASGITransport(app=app),base_url='http://test') as c:
                    r=await c.post('/api/v1/auth/login',json={'username':'guardian','password':'test-password'});h={'Authorization':'Bearer '+r.json()['data']['access_token']}
                    r=await c.post('/api/v1/devices/main/care-policy',headers=h,json=policy());self.assertEqual(r.status_code,202);await asyncio.sleep(.05)
                    self.assertEqual(cloud.sent[0][1],'set_care_policy');self.assertEqual(cloud.sent[0][2]['noact_ms'],600000)
                    actual=(await c.get('/api/v1/devices/main/care-policy',headers=h)).json()['data']['actual'];self.assertEqual(actual['revision'],1);self.assertEqual(actual['noact_ms'],1800000)
        asyncio.run(run())
