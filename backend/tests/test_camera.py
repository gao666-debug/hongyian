import asyncio
import tempfile
import time
import unittest
from unittest.mock import patch
from fastapi.testclient import TestClient
from backend.app import create_app
from backend.settings import Settings
from backend.camera import MAX_FRAME

JPEG = b'\xff\xd8test-frame\xff\xd9'


class CameraTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.settings = Settings(db=self.tmp.name+'/camera.db',password='test-password',
                                 webhook_token='x'*32,device_id='main',nearlink_device_id='env',watch_device_id='watch')
        self.app=create_app(self.settings)
        self.c=TestClient(self.app)
        self.owner=self.login('guardian','test-password')
        self.base='/api/v1/devices/main/camera'

    def tearDown(self):
        self.c.close();self.tmp.cleanup()

    def login(self,name,password):
        data=self.c.post('/api/v1/auth/login',json={'username':name,'password':password}).json()['data']
        return {'Authorization':'Bearer '+data['access_token']}

    def grant(self):
        r=self.c.post(self.base+'/provision',headers=self.owner)
        self.assertEqual(r.status_code,201)
        return {'X-Camera-Token':r.json()['data']['camera_token'],'Content-Type':'image/jpeg'}

    def test_three_devices_kinds_and_persistent_migration(self):
        data=self.c.get('/api/v1/household',headers=self.owner).json()['data']
        self.assertEqual({d['device_id']:d['kind'] for d in data['devices']},
                         {'main':'gateway','env':'nearlink','watch':'wearable'})
        login=self.c.post('/api/v1/auth/login',json={'username':'guardian','password':'test-password'}).json()['data']
        self.assertEqual(set(login['device_ids']),{'main','env','watch'})
        self.assertEqual(len(login['devices']),3)
        with self.app.state.store.connection() as db: db.execute("UPDATE home_devices SET kind='nearlink' WHERE device='watch'")
        create_app(self.settings)
        self.assertEqual([d for d in self.c.get('/api/v1/household',headers=self.owner).json()['data']['devices'] if d['device_id']=='watch'][0]['kind'],'wearable')

    def test_authentication_rotation_offline_and_bounds(self):
        self.assertEqual(self.c.get(self.base).status_code,401)
        auth=self.grant()
        self.assertEqual(self.c.get(self.base+'/frame',headers=self.owner).status_code,503)
        self.assertEqual(self.c.post(self.base+'/frame',content=JPEG).status_code,401)
        self.assertEqual(self.c.post(self.base+'/frame',headers=auth,content=b'bad').status_code,415)
        self.assertEqual(self.c.post(self.base+'/frame',headers=auth,content=b'\xff\xd8'+b'a'*MAX_FRAME+b'\xff\xd9').status_code,413)
        self.assertEqual(self.c.post(self.base+'/frame',headers=auth,content=JPEG).status_code,200)
        self.assertEqual(self.c.get(self.base+'/frame',headers=self.owner).content,JPEG)
        self.assertEqual(self.c.post(self.base+'/frame',headers=auth,content=JPEG).status_code,429)
        self.assertTrue(self.c.get(self.base,headers=self.owner).json()['data']['online'])
        with patch('backend.camera.time.time',return_value=time.time()+30):
            self.assertFalse(self.c.get(self.base,headers=self.owner).json()['data']['online'])
            self.assertEqual(self.c.get(self.base+'/frame',headers=self.owner).status_code,503)
        self.assertNotIn('camera_token',self.c.get(self.base,headers=self.owner).text)
        self.assertEqual(self.c.get('/api/v1/household',headers={'Authorization':'Bearer '+auth['X-Camera-Token']}).status_code,401)
        self.grant()
        self.assertEqual(self.c.post(self.base+'/frame',headers=auth,content=JPEG).status_code,401)
        self.assertEqual(self.c.get(self.base+'/frame',headers=self.owner).status_code,503)
        self.assertEqual(self.c.delete(self.base,headers=self.owner).status_code,200)
        self.assertFalse(self.c.get(self.base,headers=self.owner).json()['data']['configured'])

    def test_family_isolation_viewer_and_stream_revocation(self):
        self.c.post('/api/v1/household/members',headers=self.owner,json={'username':'viewer','password':'viewer-pass','role':'viewer'})
        viewer=self.login('viewer','viewer-pass')
        auth=self.grant();self.c.post(self.base+'/frame',headers=auth,content=JPEG)
        self.assertEqual(self.c.get(self.base+'/frame',headers=viewer).status_code,200)
        self.assertEqual(self.c.post(self.base+'/provision',headers=viewer).status_code,403)
        self.assertEqual(self.c.post('/api/v1/devices/watch/camera/provision',headers=self.owner).status_code,422)
        self.app.state.store.bootstrap_user('outsider','outsider-pass')
        with self.app.state.store.connection() as db: db.execute("INSERT INTO home_members VALUES('other','outsider','owner')")
        outsider=self.login('outsider','outsider-pass')
        # A watch moved to another household must not reveal the primary home's normalized watch data.
        self.app.state.store.put('mock','main','watch',{'heart_rate':77})
        with self.app.state.store.connection() as db: db.execute("UPDATE home_devices SET home='other' WHERE device='watch'")
        self.assertEqual(self.c.get('/api/v1/devices/watch/watch',headers=outsider).status_code,404)
        family=self.c.get('/api/v1/household',headers=outsider).json()['data']
        self.assertIsNone(family['devices'][0]['watch'])
        self.assertEqual(self.c.get(self.base+'/frame',headers=outsider).status_code,404)
        async def run():
            from backend.store import digest
            service=self.app.state.cameras
            user=self.app.state.store.session(digest(viewer['Authorization'][7:]))
            async def connected(): return False
            service.viewers['main']=1
            generator=service.frames('main',user,connected)
            self.assertIn(JPEG,await anext(generator))
            self.c.post('/api/v1/auth/logout',headers=viewer)
            with self.assertRaises(StopAsyncIteration): await anext(generator)
            self.assertEqual(service.viewers['main'],0)
        asyncio.run(run())

    def test_restart_discards_frame_not_device_credential(self):
        auth=self.grant();self.c.post(self.base+'/frame',headers=auth,content=JPEG)
        again=TestClient(create_app(self.settings))
        try:
            self.assertFalse(again.get(self.base,headers=self.owner).json()['data']['online'])
            self.assertEqual(again.post(self.base+'/frame',headers=auth,content=JPEG).status_code,200)
        finally:again.close()
