import tempfile
import unittest
from fastapi.testclient import TestClient
from backend.app import create_app
from backend.settings import Settings


class MessagingAccountTests(unittest.TestCase):
    def test_delete_account_revokes_chat_and_preserves_history(self):
        with tempfile.TemporaryDirectory() as tmp:
            app = create_app(Settings(db=tmp+'/test.db', password='test-password',
                                      webhook_token='x'*32, device_id='main'))
            with TestClient(app) as client:
                def login(name):
                    response = client.post('/api/v1/auth/login', json={'username':name, 'password':'test-password'})
                    return {'Authorization':'Bearer '+response.json()['data']['access_token']}
                owner = login('guardian')
                for name in ('family', 'worker'):
                    self.assertEqual(client.post('/api/v1/household/members', headers=owner, json={
                        'username':name, 'password':'test-password', 'role':'community'}).status_code, 201)
                family, worker = login('family'), login('worker')
                svc = app.state.chats
                svc.accept_invite(svc.make_invite('family')['code'], 'worker')
                cid = svc.direct('family', 'worker')['conversation_id']
                gid = svc.create_group('family', 'care group', ['worker'])['conversation_id']
                note = svc.send_text(cid, 'family', 'preserve this record', 'one')
                old_invite = svc.make_invite('family')['code']
                self.assertEqual(client.get('/api/v1/contacts', headers=family).status_code, 200)
                self.assertEqual(client.delete('/api/v1/household/members/family', headers=owner).status_code, 200)
                self.assertEqual(client.get('/api/v1/contacts', headers=family).status_code, 401)
                self.assertEqual(svc.list_contacts('worker'), [])
                self.assertEqual(svc.members_of(gid), ['worker'])
                self.assertTrue(svc.is_group_owner(gid, 'worker'))
                self.assertEqual(svc.history(cid, 'worker')['items'][0]['id'], note['id'])
                self.assertEqual(client.post('/api/v1/contacts/accept', headers=worker, json={'code':old_invite}).status_code, 404)
                self.assertEqual(client.post('/api/v1/conversations/'+cid+'/messages', headers=worker, json={'body':'cannot send'}).status_code, 403)
                self.assertEqual(client.post('/api/v1/household/members', headers=owner, json={
                    'username':'family','password':'test-password','role':'viewer'}).status_code, 409)
