import tempfile
import unittest
from fastapi.testclient import TestClient
from backend.app import create_app
from backend.settings import Settings

class NearlinkTests(unittest.TestCase):
    def test_reports_keep_device_identity(self):
        with tempfile.TemporaryDirectory() as tmp:
            settings=Settings(mode='live',db=tmp+'/db',password='test-password',webhook_token='x'*32,device_id='main',nearlink_device_id='room-a',ak='test',sk='test',project_id='test',cloud_endpoint='https://example.invalid')
            with TestClient(create_app(settings,cloud=object())) as c:
                payload={'event':'report','resource':'device.property','notify_data':{'header':{'device_id':'room-a'},'body':{'services':[{'service_id':'HomeSafety','properties':{'light':553,'smoke':152,'body':0,'door':'CLOSED'}}]}}}
                self.assertEqual(c.post('/api/v1/iotda/notifications',json=payload,headers={'X-Webhook-Token':'x'*32}).status_code,200)
                token=c.post('/api/v1/auth/login',json={'username':'guardian','password':'test-password'}).json()['data']['access_token']
                home=c.get('/api/v1/household',headers={'Authorization':'Bearer '+token}).json()['data']
                node=next(n for n in home['devices'] if n['device_id']=='room-a')
                self.assertEqual(node['properties']['door'],'CLOSED')
                self.assertIsNone(next(n for n in home['devices'] if n['device_id']=='main')['properties'])
                payload['notify_data']['body']['services'][0]['properties']['body']='1'
                self.assertEqual(c.post('/api/v1/iotda/notifications',json=payload,headers={'X-Webhook-Token':'x'*32}).status_code,422)
