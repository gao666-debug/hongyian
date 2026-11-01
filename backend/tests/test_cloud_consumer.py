import json
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch
import requests
from proton import Delivery
from backend.settings import Settings
from consume_cloud import Receiver, notification


class CloudConsumerTests(unittest.TestCase):
    def setUp(self):
        self.settings = Settings()
        self.payload = {'resource': 'device.message', 'event': 'report',
                        'notify_data': {'header': {'device_id': self.settings.device_id},
                                        'body': {'content': {'name': 'alarm_event'}}}}
        self.event = SimpleNamespace(message=SimpleNamespace(body=json.dumps(self.payload)),
                                     delivery=Mock(), connection=Mock(), container=Mock())

    def test_accepts_json_bytes_and_rejects_unrelated_messages(self):
        self.assertEqual(notification(json.dumps(self.payload).encode()), self.payload)
        # 手表的属性上报也要放行，其余资源类型仍然拒收
        self.assertEqual(notification({'resource': 'device.property', 'event': 'report'})['resource'], 'device.property')
        with self.assertRaises(ValueError):
            notification({'resource': 'device.status', 'event': 'report'})

    @patch('consume_cloud.requests.post')
    def test_acknowledges_after_ingestion_succeeds(self, post):
        post.return_value.status_code = 200
        post.return_value.raise_for_status.return_value = None
        Receiver(self.settings).on_message(self.event)
        self.assertEqual(post.call_args.kwargs['json'], self.payload)
        self.event.delivery.update.assert_called_once_with(Delivery.ACCEPTED)
        self.event.delivery.settle.assert_called_once()

    @patch('consume_cloud.requests.post', side_effect=requests.ConnectionError())
    def test_failed_ingestion_is_released_for_redelivery(self, post):
        Receiver(self.settings).on_message(self.event)
        self.event.delivery.update.assert_called_once_with(Delivery.RELEASED)
        self.event.container.stop.assert_called_once()

    @patch('consume_cloud.requests.post')
    def test_other_devices_are_never_ingested(self, post):
        self.payload['notify_data']['header']['device_id'] = 'unconfigured-device'
        self.event.message.body = self.payload
        Receiver(self.settings).on_message(self.event)
        post.assert_not_called()
        self.event.delivery.update.assert_called_once_with(Delivery.REJECTED)
