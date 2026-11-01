import json
import tempfile
import unittest
from decimal import Decimal
from pathlib import Path

from fastapi.testclient import TestClient

from backend.app import create_app
from backend.settings import Settings
from backend import watch as watch_model
from consume_cloud import notification


def report(properties, service_id='11'):
    return {'services': [{'service_id': service_id, 'properties': properties}]}


class FakeCloud:
    def shadow(self, device):
        return {'device_id': device, 'service_id': 'HomeSafety', 'online': True,
                'updated_at': None, 'properties': {'temp': 26}}

    def command(self, device, name):
        return 'executed', 0, 'cloud-watch'


class NormalizeTests(unittest.TestCase):
    def test_converts_mg_to_g_and_derives_fields(self):
        data = watch_model.normalize({'heart_rate': Decimal('72'), 'spo2': 98,
                                      'accel_x': -188, 'accel_y': -168, 'accel_z': 951,
                                      'steps': 137, 'fall_state': 2, 'fall_count': 3},
                                     '11', '2026-11-01T03:19:13Z')
        self.assertEqual(data['accel_g'], {'x': -0.188, 'y': -0.168, 'z': 0.951})
        self.assertEqual(data['motion_mg'], 983)
        self.assertEqual(data['dynamic_mg'], 17)
        self.assertTrue(data['fall_active'])
        self.assertFalse(data['fall_watch'])
        self.assertTrue(data['hr_measuring'])
        self.assertEqual(data['service_id'], '11')

    def test_zero_heart_rate_means_not_measured(self):
        data = watch_model.normalize({'heart_rate': 0, 'spo2': 0})
        self.assertFalse(data['hr_measuring'])
        self.assertEqual(data['heart_rate'], 0)

    def test_rejects_out_of_range_and_non_numeric(self):
        for bad in ({'heart_rate': 500}, {'spo2': 130}, {'steps': -1},
                    {'fall_state': 9}, {'accel_z': 99999}, {'heart_rate': 'abc'},
                    {'fall_count': True}):
            with self.assertRaises(watch_model.WatchReportError):
                watch_model.normalize(bad)
        with self.assertRaises(watch_model.WatchReportError):
            watch_model.normalize({'temp': 25})          # 没有手表的任何属性

    def test_picks_configured_service_and_falls_back_by_property_names(self):
        services = [{'service_id': 'HomeSafety', 'properties': {'temp': 25}},
                    {'service_id': '11', 'properties': {'heart_rate': 60}}]
        self.assertEqual(watch_model.pick_service(services, '11')['service_id'], '11')
        # 服务 ID 配置对不上时按属性名兜底，避免产品改 ID 就整条链路失联
        self.assertEqual(watch_model.pick_service(services, '')['service_id'], '11')
        self.assertEqual(watch_model.pick_service(services, 'renamed-service')['service_id'], '11')
        self.assertIsNone(watch_model.pick_service([services[0]], '11'))


class WatchApiTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.settings = Settings(db=str(Path(self.directory.name) / 'test.sqlite3'),
                                 password='local-test-password-only',
                                 webhook_token='webhook-test-secret-value-only',
                                 watch_device_id='watch-board-1')
        self.client = TestClient(create_app(self.settings))
        self.client.__enter__()
        token = self.client.post('/api/v1/auth/login',
                                 json={'username': 'guardian', 'password': self.settings.password}
                                 ).json()['data']['access_token']
        self.auth = {'Authorization': 'Bearer ' + token}
        self.device = self.settings.device_id
        self.properties = {'heart_rate': 72, 'spo2': 98, 'accel_x': -188, 'accel_y': -168,
                           'accel_z': 951, 'steps': 137, 'fall_state': 0, 'fall_count': 0}

    def tearDown(self):
        self.client.__exit__(None, None, None)
        self.directory.cleanup()

    def post_watch(self, properties=None, service_id='11'):
        return self.client.post('/api/v1/mock/watch?device_id=' + self.device, headers=self.auth,
                                json=report(properties or self.properties, service_id))

    def test_latest_and_hourly_bucket(self):
        self.assertEqual(self.post_watch().status_code, 200)
        self.post_watch({**self.properties, 'heart_rate': 84, 'steps': 200})
        latest = self.client.get('/api/v1/devices/%s/watch' % self.device, headers=self.auth).json()['data']
        self.assertEqual((latest['heart_rate'], latest['steps'], latest['motion_mg']), (84, 200, 983))
        hours = self.client.get('/api/v1/devices/%s/watch/hours' % self.device, headers=self.auth).json()['data']['items']
        self.assertEqual(len(hours), 1)
        self.assertEqual(hours[0]['samples'], 2)
        self.assertEqual((hours[0]['hr']['min'], hours[0]['hr']['max'], hours[0]['hr']['avg']), (72, 84, 78.0))
        self.assertEqual(hours[0]['steps_delta'], 63)

    def test_fall_counter_creates_events_and_pushes(self):
        pushed = []
        hub = self.client.app.state.hub
        original = hub.publish

        async def spy(device, event):
            pushed.append(event)
            await original(device, event)

        hub.publish = spy
        self.post_watch()
        pushed.clear()
        self.assertEqual(self.post_watch({**self.properties, 'fall_state': 2, 'fall_count': 1}).status_code, 200)
        self.assertEqual(sorted(event['type'] for event in pushed), ['watch', 'watch_fall'])
        self.assertEqual(pushed[0]['simulated'], True if self.settings.mode == 'mock' else False)
        events = self.client.get('/api/v1/devices/%s/watch/falls' % self.device,
                                 headers=self.auth).json()['data']['items']
        self.assertEqual(len(events), 1)
        self.assertEqual(events[0]['fall_count'], 1)
        # 次数不变时不应该重复产生事件
        self.post_watch({**self.properties, 'fall_state': 0, 'fall_count': 1})
        events = self.client.get('/api/v1/devices/%s/watch/falls' % self.device,
                                 headers=self.auth).json()['data']['items']
        self.assertEqual(len(events), 1)

    def test_invalid_report_is_rejected(self):
        self.assertEqual(self.post_watch({'heart_rate': 9000}).status_code, 422)
        self.assertEqual(self.post_watch({'temp': 25}, service_id='HomeSafety').status_code, 422)
        self.assertEqual(self.client.get('/api/v1/devices/%s/watch' % self.device, headers=self.auth).status_code, 404)

    def test_live_webhook_accepts_property_report(self):
        live = Settings(mode='live', db=self.settings.db, password=self.settings.password,
                        webhook_token=self.settings.webhook_token, ak='test-ak', sk='test-sk',
                        project_id='test-project', watch_device_id='watch-board-1')
        with TestClient(create_app(live, FakeCloud())) as client:
            token = client.post('/api/v1/auth/login',
                                json={'username': 'guardian', 'password': live.password}
                                ).json()['data']['access_token']
            auth = {'Authorization': 'Bearer ' + token}
            payload = {'resource': 'device.property', 'event': 'report',
                       'notify_data': {'header': {'device_id': 'watch-board-1'},
                                       'body': {'service_id': '11', 'properties': self.properties}}}
            self.assertEqual(client.post('/api/v1/iotda/notifications', json=payload).status_code, 401)
            result = client.post('/api/v1/iotda/notifications',
                                 headers={'X-Webhook-Token': live.webhook_token}, json=payload)
            self.assertEqual(result.status_code, 200)
            self.assertEqual(client.get('/api/v1/devices/%s/watch' % self.device,
                                        headers=auth).json()['data']['heart_rate'], 72)
            unknown = {'resource': 'device.property', 'event': 'report',
                       'notify_data': {'header': {'device_id': 'someone-else'},
                                       'body': {'service_id': '11', 'properties': self.properties}}}
            self.assertEqual(client.post('/api/v1/iotda/notifications', headers={'X-Webhook-Token': live.webhook_token},
                                         json=unknown).status_code, 404)


class ConsumerTests(unittest.TestCase):
    def test_notification_accepts_property_and_rejects_other_resources(self):
        payload = {'resource': 'device.property', 'event': 'report',
                   'notify_data': {'header': {'device_id': 'x'}, 'body': {'service_id': '11'}}}
        self.assertEqual(notification(json.dumps(payload).encode()), payload)
        with self.assertRaises(ValueError):
            notification({'resource': 'device.status', 'event': 'report'})


if __name__ == '__main__':
    unittest.main(verbosity=2)
