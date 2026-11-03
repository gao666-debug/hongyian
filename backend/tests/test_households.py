import asyncio
import tempfile
import unittest
import httpx
from backend.app import create_app
from backend.settings import Settings

class HouseholdTests(unittest.TestCase):
    def test_accounts_and_permissions(self):
        async def run():
            with tempfile.TemporaryDirectory() as tmp:
                app=create_app(Settings(db=tmp+'/test.db',password='test-password',webhook_token='x'*32,device_id='main',watch_device_id='watch'))
                async with httpx.AsyncClient(transport=httpx.ASGITransport(app=app),base_url='http://test') as c:
                    self.assertEqual((await c.get('/api/v1/household')).status_code,401)
                    async def auth(name,password):
                        r=await c.post('/api/v1/auth/login',json={'username':name,'password':password})
                        return {'Authorization':'Bearer '+r.json()['data']['access_token']}
                    owner=await auth('guardian','test-password')
                    home=(await c.get('/api/v1/household',headers=owner)).json()['data']
                    self.assertEqual(len(home['devices']),2)
                    body={'username':'observer','password':'test-pass-2','role':'viewer'}
                    self.assertEqual((await c.post('/api/v1/household/members',headers=owner,json=body)).status_code,201)
                    self.assertEqual((await c.post('/api/v1/household/members',headers=owner,json=body)).status_code,409)
                    viewer=await auth('observer','test-pass-2')
                    self.assertEqual((await c.get('/api/v1/devices/main/shadow',headers=viewer)).status_code,200)
                    self.assertEqual((await c.post('/api/v1/commands',headers=viewer,json={'device_id':'main','command_name':'ack','client_request_id':'1'})).status_code,403)
                    self.assertEqual((await c.get('/api/v1/devices/other/shadow',headers=viewer)).status_code,404)
                    self.assertEqual((await c.post('/api/v1/household/members',headers=viewer,json=body)).status_code,403)
                    # Two phones use independent sessions; logging out one does not disconnect another.
                    second=await auth('guardian','test-password')
                    self.assertNotEqual(owner['Authorization'],second['Authorization'])
                    self.assertEqual((await c.post('/api/v1/auth/logout',headers=owner)).status_code,200)
                    self.assertEqual((await c.get('/api/v1/household',headers=owner)).status_code,401)
                    self.assertEqual((await c.get('/api/v1/household',headers=second)).status_code,200)
                    self.assertEqual((await c.get('/api/v1/household',headers=viewer)).status_code,200)

        asyncio.run(run())
