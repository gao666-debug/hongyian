import asyncio
import tempfile
import unittest
import httpx
from backend.app import create_app
from backend.settings import Settings
from test_remote_care import health
class ContactTextTests(unittest.TestCase):
    def test_family_text_identity_unicode_read_and_control_isolation(self):
        async def run():
            with tempfile.TemporaryDirectory() as tmp:
                app=create_app(Settings(db=tmp+'/x.db',password='test-password',webhook_token='x'*32,device_id='main'))
                app.state.store.put('mock','main','health',health())
                async with httpx.AsyncClient(transport=httpx.ASGITransport(app=app),base_url='http://test') as c:
                    async def login(name='guardian',pw='test-password'):
                        r=await c.post('/api/v1/auth/login',json={'username':name,'password':pw});return {'Authorization':'Bearer '+r.json()['data']['access_token']}
                    owner=await login();await c.post('/api/v1/household/members',headers=owner,json={'username':'alice','password':'alice-pass','role':'viewer'});alice=await login('alice','alice-pass')
                    event={'schema':1,'boot_id':7,'message_id':1,'revision':1,'epoch':0,'direction':'board','code':4,'reply_to':0,'seen':False,'request_id':'web-text-one','text':'我今天很好 <你好> 😀','author':'someone'}
                    r=await c.post('/api/v1/mock/messages?device_id=main',headers=owner,json={'name':'contact_event','content':event});self.assertEqual(r.status_code,200,r.text)
                    feed=(await c.get('/api/v1/devices/main/messages',headers=alice)).json()['data'];self.assertTrue(feed['text_supported']);self.assertEqual(feed['items'][0]['author'],'NFC网页');self.assertIn('<你好>',feed['items'][0]['text'])
                    body={'boot_id':7,'text':'好的，晚上给你打电话 😀','client_request_id':'alice-text'}
                    r=await c.post('/api/v1/devices/main/messages/chat',headers=alice,json=body);self.assertEqual(r.status_code,202,r.text);ident=r.json()['data']['command_id']
                    self.assertEqual((await c.post('/api/v1/devices/main/messages/chat',headers=alice,json=body)).json()['data']['command_id'],ident)
                    self.assertEqual((await c.post('/api/v1/devices/main/messages/chat',headers=alice,json={**body,'text':'changed'})).status_code,409)
                    self.assertEqual((await c.post('/api/v1/commands',headers=alice,json={'device_id':'main','command_name':'ack','client_request_id':'denied'})).status_code,403)
                    await asyncio.sleep(.25);feed=(await c.get('/api/v1/devices/main/messages',headers=alice)).json()['data'];reply=next(x for x in feed['items'] if x['direction']=='family');self.assertEqual(reply['author'],'alice');self.assertEqual(reply['text'],body['text']);self.assertFalse(reply['seen'])
                    data={k:v for k,v in reply.items() if k not in ('cursor','received_at')};data.update(seen=True,revision=2,author='forged')
                    r=await c.post('/api/v1/mock/messages?device_id=main',headers=owner,json={'name':'contact_event','content':data});self.assertEqual(r.status_code,200,r.text)
                    feed=(await c.get('/api/v1/devices/main/messages',headers=alice)).json()['data'];self.assertEqual(feed['items'][-1]['author'],'alice');self.assertTrue(feed['items'][-1]['seen'])
                    for text in ('   ','\0x','x'*121,'中'*41):
                        r=await c.post('/api/v1/devices/main/messages/chat',headers=alice,json={**body,'text':text,'client_request_id':'invalid'});self.assertEqual(r.status_code,422,r.text)
                    r=await c.post('/api/v1/devices/main/messages/chat',headers=alice,json={**body,'boot_id':8});self.assertEqual(r.status_code,409)
                    app.state.store.put('mock','main','health',{**health(),'contact_text':False})
                    self.assertEqual((await c.post('/api/v1/devices/main/messages/chat',headers=alice,json={**body,'client_request_id':'old-firmware'})).status_code,409)
        asyncio.run(run())
