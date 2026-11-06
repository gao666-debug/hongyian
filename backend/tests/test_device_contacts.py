import asyncio
import tempfile
import unittest
import httpx
from backend.app import create_app
from backend.settings import Settings

BASE={'schema':1,'boot_id':7,'message_id':1,'revision':1,'epoch':0,'direction':'board','code':2,'reply_to':0,'seen':False,'request_id':''}
class DeviceContactTests(unittest.TestCase):
    def test_feed_permissions_reply_and_confirm(self):
        async def run():
            with tempfile.TemporaryDirectory() as tmp:
                app=create_app(Settings(db=tmp+'/x.db',password='test-password',webhook_token='x'*32,device_id='main',watch_device_id='watch'))
                async with httpx.AsyncClient(transport=httpx.ASGITransport(app=app),base_url='http://test') as c:
                    async def login(name='guardian',password='test-password'):
                        r=await c.post('/api/v1/auth/login',json={'username':name,'password':password});return {'Authorization':'Bearer '+r.json()['data']['access_token']}
                    owner=await login();self.assertEqual((await c.get('/api/v1/devices/main/messages')).status_code,401)
                    async def event(data):return await c.post('/api/v1/mock/messages?device_id=main',headers=owner,json={'name':'contact_event','content':data})
                    r=await event(BASE);self.assertEqual(r.status_code,200,r.text);cursor=r.json()['data']['cursor']
                    self.assertTrue((await event(BASE)).json()['data']['duplicate'])
                    self.assertEqual((await event({**BASE,'code':3})).status_code,409)
                    self.assertEqual((await event({**BASE,'seen':True})).status_code,409)
                    self.assertEqual((await event({**BASE,'seen':'true'})).status_code,422)
                    await c.post('/api/v1/household/members',headers=owner,json={'username':'viewer','password':'viewer-pass','role':'viewer'})
                    viewer=await login('viewer','viewer-pass')
                    self.assertEqual((await c.get('/api/v1/devices/main/messages',headers=viewer)).status_code,200)
                    self.assertEqual((await c.post('/api/v1/devices/main/messages/7/1/reply',headers=viewer,json={'code':1,'client_request_id':'a'})).status_code,202)
                    self.assertEqual((await c.get('/api/v1/devices/watch/messages',headers=owner)).status_code,409)
                    self.assertEqual((await c.get('/api/v1/devices/unknown/messages',headers=owner)).status_code,404)
                    body={'code':2,'client_request_id':'reply-one'}
                    r=await c.post('/api/v1/devices/main/messages/7/1/reply',headers=owner,json=body);self.assertEqual(r.status_code,202,r.text);ident=r.json()['data']['command_id'];self.assertFalse(r.json()['data']['human_confirmed'])
                    again=await c.post('/api/v1/devices/main/messages/7/1/reply',headers=owner,json=body);self.assertEqual(again.json()['data']['command_id'],ident)
                    self.assertEqual((await c.post('/api/v1/devices/main/messages/7/1/reply',headers=owner,json={**body,'code':1})).status_code,409)
                    await asyncio.sleep(.25)
                    command=(await c.get('/api/v1/commands/'+ident,headers=owner)).json()['data'];self.assertEqual(command['state'],'executed')
                    feed=(await c.get('/api/v1/devices/main/messages?after='+str(cursor),headers=owner)).json()['data'];reply=feed['items'][0];self.assertEqual(reply['direction'],'family');self.assertFalse(reply['seen'])
                    data={k:v for k,v in reply.items() if k not in ('cursor','received_at')};data.update(seen=True,revision=2)
                    self.assertEqual((await event(data)).status_code,200)
                    feed=(await c.get('/api/v1/devices/main/messages?after='+str(feed['next_cursor']),headers=owner)).json()['data'];self.assertTrue(feed['items'][0]['seen'])
                    r=await c.post('/api/v1/devices/main/messages/7/1/seen',headers=owner,json={'client_request_id':'seen-one'});self.assertEqual(r.status_code,202);await asyncio.sleep(.25)
                    feed=(await c.get('/api/v1/devices/main/messages?limit=1',headers=owner)).json()['data'];self.assertTrue(feed['has_more']);self.assertEqual(len(feed['items']),1)
                    app2=create_app(Settings(db=tmp+'/x.db',password='test-password',webhook_token='x'*32,device_id='main',watch_device_id='watch'))
                    self.assertEqual(app2.state.device_contacts.board_message('main',7,1)['seen'],True)
        asyncio.run(run())
    def test_live_webhook_and_command_payload(self):
        class Cloud:
            def __init__(self):self.sent=[]
            def command(self,device,name,params):self.sent.append((device,name,params));return 'executed',0,'cloud-id'
        async def run():
            with tempfile.TemporaryDirectory() as tmp:
                cloud=Cloud();settings=Settings(mode='live',db=tmp+'/x.db',password='test-password',webhook_token='x'*32,device_id='main',ak='x',sk='y',project_id='p')
                app=create_app(settings,cloud=cloud)
                async with httpx.AsyncClient(transport=httpx.ASGITransport(app=app),base_url='http://test') as c:
                    notice={'resource':'device.message','event':'report','notify_data':{'header':{'device_id':'main'},'body':{'content':{'name':'contact_event','content':BASE}}}}
                    self.assertEqual((await c.post('/api/v1/iotda/notifications',json=notice)).status_code,401)
                    r=await c.post('/api/v1/iotda/notifications',headers={'X-Webhook-Token':'x'*32},json=notice);self.assertEqual(r.status_code,200,r.text)
                    r=await c.post('/api/v1/auth/login',json={'username':'guardian','password':'test-password'});headers={'Authorization':'Bearer '+r.json()['data']['access_token']}
                    r=await c.post('/api/v1/devices/main/messages/7/1/reply',headers=headers,json={'code':3,'client_request_id':'live-one'});self.assertEqual(r.status_code,202,r.text);await asyncio.sleep(.05)
                    self.assertEqual(cloud.sent[0][:2],('main','contact'));p=cloud.sent[0][2];self.assertEqual(p['boot_id'],7);self.assertEqual(p['message_id'],1);self.assertEqual(p['code'],3);self.assertEqual(len(p['request_id']),64)
        asyncio.run(run())
