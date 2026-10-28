"""Receive IoTDA AMQP reports and persist through the authenticated local ingest API."""
import json
import os
from pathlib import Path
import ssl
import time

import requests
from proton import SSL, SSLDomain, Delivery
from proton.handlers import MessagingHandler
from proton.reactor import Container

from backend.settings import Settings, ROOT


def notification(body):
    if isinstance(body, bytes):
        body = body.decode('utf-8')
    payload = json.loads(body) if isinstance(body, str) else body
    if not isinstance(payload, dict):
        raise ValueError('Expected IoTDA notification')
    if payload.get('resource') != 'device.message' or payload.get('event') != 'report':
        raise ValueError('Unsupported notification type')
    return payload


class Receiver(MessagingHandler):
    def __init__(self, settings):
        super().__init__(auto_accept=False, prefetch=10)
        self.settings = settings
        self.connected = False

    def on_start(self, event):
        host = os.environ['AMQP_HOST']
        key = os.environ['AMQP_ACCESS_KEY']
        code = os.environ['AMQP_ACCESS_CODE']
        queue = os.environ['AMQP_QUEUE']
        instance = os.getenv('AMQP_INSTANCE_ID', '')
        username = f'accessKey={key}|timestamp={int(time.time()*1000)}|instanceId={instance}'
        domain = SSLDomain(SSLDomain.MODE_CLIENT)
        ca = ssl.get_default_verify_paths().cafile or requests.certs.where()
        domain.set_trusted_ca_db(ca)
        domain.set_peer_authentication(SSLDomain.VERIFY_PEER_NAME)
        self.connection = event.container.connect(
            f'amqps://{host}:5671', user=username, password=code,
            heartbeat=60, ssl_domain=domain, reconnect=False,
            allowed_mechs='PLAIN', allow_insecure_mechs=False)
        event.container.create_receiver(self.connection, source=queue)

    def on_connection_opened(self, event):
        self.connected = True
        print('[cloud-consumer] AMQP connected', flush=True)

    def on_message(self, event):
        try:
            payload = notification(event.message.body)
            device = payload['notify_data']['header']['device_id']
            if device != self.settings.device_id:
                event.delivery.update(Delivery.REJECTED)
                event.delivery.settle()
                return
            response = requests.post(
                os.getenv('AMQP_LOCAL_BACKEND', 'https://127.0.0.1:8443') + '/api/v1/iotda/notifications',
                json=payload, headers={'X-Webhook-Token': self.settings.webhook_token},
                verify=str(ROOT / '.local/tls/development-ca.crt'), timeout=(5, 10))
            if response.status_code == 422:
                print('[cloud-consumer] Unsupported message rejected', flush=True)
                event.delivery.update(Delivery.REJECTED)
            else:
                response.raise_for_status()
                event.delivery.update(Delivery.ACCEPTED)
                print('[cloud-consumer] Device message persisted', flush=True)
            event.delivery.settle()
        except (ValueError, KeyError, TypeError):
            event.delivery.update(Delivery.REJECTED)
            event.delivery.settle()
            print('[cloud-consumer] Invalid notification rejected', flush=True)
        except requests.RequestException:
            event.delivery.update(Delivery.RELEASED)
            event.delivery.settle()
            event.connection.close()
            event.container.stop()
            print('[cloud-consumer] Local ingestion unavailable; reconnecting', flush=True)

    def on_transport_error(self, event):
        print('[cloud-consumer] Transport failure; reconnecting', flush=True)
        event.container.stop()

    def on_connection_error(self, event):
        print('[cloud-consumer] Connection rejected; check AMQP configuration', flush=True)
        event.container.stop()

    def on_disconnected(self, event):
        event.container.stop()


def main():
    if not SSL.present():
        raise SystemExit('AMQP SDK lacks TLS support: install pkg-config/libssl-dev and rebuild python-qpid-proton')
    settings = Settings.load()
    if settings.mode != 'live':
        raise SystemExit('AMQP consumer requires live mode')
    required = ['AMQP_HOST', 'AMQP_ACCESS_KEY', 'AMQP_ACCESS_CODE', 'AMQP_QUEUE']
    if any(not os.getenv(name) for name in required):
        raise SystemExit('Configure AMQP_HOST, AMQP_ACCESS_KEY, AMQP_ACCESS_CODE, AMQP_QUEUE in .env')
    while True:
        try:
            Container(Receiver(settings)).run()
        except KeyboardInterrupt:
            return
        except Exception as error:
            print('[cloud-consumer] Retry after', type(error).__name__, flush=True)
        time.sleep(5)


if __name__ == '__main__':
    main()
