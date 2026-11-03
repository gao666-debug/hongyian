"""Regression coverage for combining production 1.4 and household permissions."""
import json
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import Mock, patch

from fastapi.testclient import TestClient
from proton import Delivery
from backend.app import create_app
from backend.cloud import HuaweiCloud
from backend.settings import Settings
from consume_cloud import Receiver


class TeamIntegrationTests(unittest.TestCase):
    def settings(self, tmp, **extra):
        return Settings(db=tmp+'/test.db', password='test-password', webhook_token='x'*32,
                        device_id='main,room', watch_device_id='watch', **extra)

    def login(self, client, name='guardian', password='test-password'):
        data=client.post('/api/v1/auth/login', json={'username':name,'password':password}).json()['data']
        return {'Authorization':'Bearer '+data['access_token']}

    def test_comma_configuration_and_household_remain_compatible(self):
        with tempfile.TemporaryDirectory() as tmp:
            settings=self.settings(tmp)
            with TestClient(create_app(settings)) as client:
                nodes=client.get('/api/v1/household',headers=self.login(client)).json()['data']['devices']
                self.assertEqual({n['device_id'] for n in nodes},{'main','room','watch'})
                self.assertEqual(settings.primary_device,'main')
                self.assertNotIn('main,room',settings.allowed_device_ids)

    def test_live_watch_and_environment_do_not_mix(self):
        with tempfile.TemporaryDirectory() as tmp:
            settings=self.settings(tmp,mode='live',ak='test',sk='test',project_id='test')
            with TestClient(create_app(settings,cloud=object())) as client:
                def send(device,props):
                    return client.post('/api/v1/iotda/notifications',headers={'X-Webhook-Token':'x'*32},json={
                        'resource':'device.property','event':'report','notify_data':{'header':{'device_id':device},
                        'body':{'services':[{'service_id':'11','properties':props}]}}})
                first={'heart_rate':72,'spo2':98,'steps':137,'fall_state':2,'fall_count':1}
                self.assertEqual(send('watch',first).status_code,200)
                self.assertEqual(send('watch',first).json()['data']['fall_events'],0)
                self.assertTrue(send('room',{'light':553,'smoke':152,'body':0,'door':'CLOSED'}).json()['data']['ignored'])
                self.assertEqual(send('unknown',first).status_code,404)
                auth=self.login(client)
                latest=client.get('/api/v1/devices/main/watch',headers=auth).json()['data']
                self.assertEqual(latest['heart_rate'],72)
                falls=client.get('/api/v1/devices/main/watch/falls',headers=auth).json()['data']['items']
                self.assertEqual(len(falls),1)
                with client.app.state.store.connection() as db:
                    self.assertEqual(db.execute("SELECT count(*) FROM documents WHERE device='main,room'").fetchone()[0],0)

    def test_capture_round_trip_and_viewer_write_rejection(self):
        with tempfile.TemporaryDirectory() as tmp:
            with TestClient(create_app(self.settings(tmp))) as client:
                owner=self.login(client)
                self.assertEqual(client.post('/api/v1/household/members',headers=owner,json={
                    'username':'viewer1','password':'viewer-password','role':'viewer'}).status_code,201)
                viewer=self.login(client,'viewer1','viewer-password')
                base='/api/v1/devices/main'
                self.assertEqual(client.post(base+'/capture-request',headers=viewer,json={}).status_code,403)
                self.assertEqual(client.post(base+'/captures',headers=owner,content=b'not jpeg').status_code,415)
                jpeg=b'\xff\xd8test\xff\xd9'
                upload=client.post(base+'/captures',headers=owner,content=jpeg)
                self.assertEqual(upload.status_code,201)
                cid=upload.json()['data']['capture_id']
                self.assertEqual(client.get(base+'/captures/'+cid,headers=viewer).content,jpeg)
                self.assertEqual(client.delete(base+'/captures/'+cid,headers=viewer).status_code,403)
                self.assertEqual(client.delete(base+'/captures/'+cid,headers=owner).status_code,200)
                self.assertEqual(client.get(base+'/captures/'+cid,headers=owner).status_code,404)

    def test_dead_letter_is_archived_before_rejection(self):
        with tempfile.TemporaryDirectory() as tmp:
            settings=self.settings(tmp)
            event=SimpleNamespace(message=SimpleNamespace(body={'event':'report','resource':'device.property',
                'notify_data':{'header':{'device_id':'watch'},'body':{}}}),delivery=Mock())
            with patch('consume_cloud.DEAD_LETTER_DIR',Path(tmp)/'dead-letter'), patch('consume_cloud.requests.post') as post:
                post.return_value.status_code=422
                receiver=Receiver(settings)
                receiver.on_message(event)
                self.assertEqual(receiver.dead_letters,1)
                self.assertEqual(len(list((Path(tmp)/'dead-letter').glob('*.jsonl'))),1)
                event.delivery.update.assert_called_once_with(Delivery.REJECTED)

    def test_shadow_accepts_non_home_safety_service(self):
        cloud=object.__new__(HuaweiCloud)
        cloud.client=Mock()
        cloud.client.show_device.return_value.status='ONLINE'
        cloud.client.show_device_shadow.return_value.to_dict.return_value={'shadow':[
            {'service_id':'11','reported':{'properties':{'light':553,'door':'CLOSED'},'event_time':'20261102T080000Z'}}]}
        result=cloud.shadow('room')
        self.assertEqual(result['properties']['light'],553)
        self.assertEqual(result['service_id'],'11')
        self.assertEqual(result['updated_at'],'2026-11-02T08:00:00Z')
