"""Regressions for cross-chat retries, concurrent operations and live delivery."""
import asyncio
from concurrent.futures import ThreadPoolExecutor
import tempfile
import unittest

from fastapi import HTTPException
from fastapi.testclient import TestClient

from backend.app import create_app
from backend.settings import Settings
from backend.store import digest


class MessagingIsolationTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.app = create_app(Settings(db=self.tmp.name + '/test.db', password='test-password',
                                     webhook_token='x' * 32, device_id='main'))
        self.svc = self.app.state.chats
        for name in ('bob', 'carol'):
            self.app.state.store.bootstrap_user(name, 'test-password')
            self.svc.accept_invite(self.svc.make_invite('guardian')['code'], name)
        self.ab = self.svc.direct('guardian', 'bob')['conversation_id']
        self.ac = self.svc.direct('guardian', 'carol')['conversation_id']

    def tearDown(self):
        self.tmp.cleanup()

    def test_retry_is_scoped_and_conflicting_payload_is_rejected(self):
        first = self.svc.send_text(self.ab, 'guardian', 'private for bob', 'same-key')
        other = self.svc.send_text(self.ac, 'guardian', 'private for carol', 'same-key')
        reply = self.svc.send_text(self.ab, 'bob', 'reply', 'same-key')
        self.assertEqual(len({first['id'], other['id'], reply['id']}), 3)
        self.assertEqual(other['body'], 'private for carol')
        self.assertEqual(self.svc.send_text(self.ab, 'guardian', 'private for bob', 'same-key')['id'], first['id'])
        with self.assertRaises(HTTPException) as error:
            self.svc.send_text(self.ab, 'guardian', 'changed payload', 'same-key')
        self.assertEqual(error.exception.status_code, 409)
        # Existing 1.9.0 global keys remain retryable only by their real sender.
        with self.app.state.store.connection() as db:
            db.execute('UPDATE messages SET client_id=? WHERE id=?', ('legacy', first['id']))
        self.assertEqual(self.svc.send_text(self.ab, 'guardian', 'private for bob', 'legacy')['id'], first['id'])
        self.assertNotEqual(self.svc.send_text(self.ac, 'guardian', 'not leaked', 'legacy')['id'], first['id'])
        self.svc.remove_contact('guardian', 'bob')
        with self.assertRaises(HTTPException) as error:
            self.svc.send_text(self.ab, 'bob', 'cannot send after unfriend')
        self.assertEqual(error.exception.status_code, 403)
        self.assertTrue(self.svc.history(self.ab, 'bob')['items'])

    def test_concurrent_retry_and_invite_are_atomic(self):
        with ThreadPoolExecutor(max_workers=4) as pool:
            rows = list(pool.map(lambda _: self.svc.send_text(self.ab, 'guardian', 'once', 'concurrent'), range(4)))
        self.assertEqual(len({row['id'] for row in rows}), 1)
        self.app.state.store.bootstrap_user('new1', 'test-password')
        self.app.state.store.bootstrap_user('new2', 'test-password')
        code = self.svc.make_invite('bob')['code']
        def accept(user):
            try:
                self.svc.accept_invite(code, user)
                return 200
            except HTTPException as exc:
                return exc.status_code
        with ThreadPoolExecutor(max_workers=2) as pool:
            self.assertEqual(sorted(pool.map(accept, ('new1', 'new2'))), [200, 409])

    def test_group_owner_cannot_leave_and_keep_admin_access(self):
        gid = self.svc.create_group('guardian', 'family', ['bob'])['conversation_id']
        with self.assertRaises(HTTPException) as error:
            self.svc.remove_member(gid, 'guardian', 'guardian')
        self.assertEqual(error.exception.status_code, 409)
        self.svc.remove_member(gid, 'bob', 'bob')
        with self.assertRaises(HTTPException):
            self.svc.history(gid, 'bob')

    def test_streaming_upload_size_and_membership(self):
        with TestClient(self.app) as client:
            token = client.post('/api/v1/auth/login', json={
                'username': 'guardian', 'password': 'test-password'}).json()['data']['access_token']
            auth = {'Authorization': 'Bearer ' + token, 'Content-Type': 'image/jpeg'}
            from unittest.mock import patch
            with patch('backend.messaging.MAX_MEDIA_BYTES', 16):
                response = client.post('/api/v1/media', params={'conversation_id': self.ab},
                                       headers=auth, content=iter([b'\xff\xd8' + b'x' * 8, b'x' * 8]))
                self.assertEqual(response.status_code, 413)
            self.assertEqual(client.post('/api/v1/media', params={'conversation_id': 'not-member'},
                                         headers=auth, content=b'\xff\xd8').status_code, 403)

    def test_sender_other_devices_receive_message_and_logout_closes_socket(self):
        with TestClient(self.app) as client:
            token = client.post('/api/v1/auth/login', json={
                'username': 'guardian', 'password': 'test-password'}).json()['data']['access_token']
            auth = {'Authorization': 'Bearer ' + token}
            ticket = client.post('/api/v1/ws-ticket?device_id=main', headers=auth).json()['data']['ticket']
            with client.websocket_connect('/api/v1/ws?device_id=main&ticket=' + ticket) as ws:
                self.assertEqual(ws.receive_json()['type'], 'hello')
                message = client.post('/api/v1/conversations/' + self.ab + '/messages', headers=auth,
                                      json={'body': 'sync my other phone', 'client_id': 'phone'}).json()['data']
                event = ws.receive_json()
                self.assertEqual((event['type'], event['data']['id']), ('message', message['id']))
                self.app.state.store.logout(digest(token))
                async def wake():
                    await self.app.state.hub.publish_user('guardian', {'type': 'message', 'conversation_id': self.ab, 'data': message})
                client.portal.call(wake)
                from starlette.websockets import WebSocketDisconnect
                with self.assertRaises(WebSocketDisconnect) as error:
                    ws.receive_json()
                self.assertEqual(error.exception.code, 4401)
