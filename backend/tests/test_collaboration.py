import asyncio
import tempfile
import unittest

import httpx
from fastapi.testclient import TestClient
from starlette.websockets import WebSocketDisconnect

from backend.app import create_app
from backend.settings import Settings


class CollaborationTests(unittest.TestCase):
    def test_incident_socket_filters_private_data_and_revokes_deleted_account(self):
        with tempfile.TemporaryDirectory() as tmp:
            app=create_app(Settings(db=tmp+'/test.db',password='test-password',webhook_token='x'*32,device_id='main'))
            with TestClient(app) as c:
                def auth(name,password):
                    token=c.post('/api/v1/auth/login',json={'username':name,'password':password}).json()['data']['access_token']
                    return {'Authorization':'Bearer '+token}
                owner=auth('guardian','test-password')
                c.post('/api/v1/household/members',headers=owner,json={'username':'worker','password':'test-pass-2','role':'community'})
                worker=auth('worker','test-pass-2')
                ticket=c.post('/api/v1/ws-ticket?device_id=main',headers=worker).json()['data']['ticket']
                with c.websocket_connect('/api/v1/ws?device_id=main&ticket='+ticket) as ws:
                    self.assertEqual(ws.receive_json()['type'],'hello')
                    c.portal.call(app.state.hub.publish,'main',{'type':'health','data':{'private':'medical'}})
                    c.portal.call(app.state.hub.publish,'main',{'type':'alarm','data':{'event_id':1}})
                    self.assertEqual(ws.receive_json()['type'],'alarm')
                    self.assertEqual(c.delete('/api/v1/household/members/worker',headers=owner).status_code,200)
                    c.portal.call(app.state.hub.publish,'main',{'type':'alarm','data':{'event_id':2}})
                    with self.assertRaises(WebSocketDisconnect) as closed: ws.receive_json()
                    self.assertEqual(closed.exception.code,4401)

    def test_roles_claims_revocation_and_history(self):
        async def run():
            with tempfile.TemporaryDirectory() as tmp:
                app = create_app(Settings(db=tmp+'/test.db', password='test-password',
                    webhook_token='x'*32, device_id='main', watch_device_id='watch'))
                async with httpx.AsyncClient(transport=httpx.ASGITransport(app=app),base_url='http://test') as c:
                    async def login(name, password='test-pass-2'):
                        r=await c.post('/api/v1/auth/login',json={'username':name,'password':password})
                        self.assertEqual(r.status_code,200,r.text)
                        return {'Authorization':'Bearer '+r.json()['data']['access_token']}
                    owner=await login('guardian','test-password')
                    for name,role in [('worker','community'),('guard','property'),('family','caregiver'),('observer','viewer')]:
                        r=await c.post('/api/v1/household/members',headers=owner,json={'username':name,'password':'test-pass-2','role':role})
                        self.assertEqual(r.status_code,201,r.text)
                    worker=await login('worker'); guard=await login('guard'); viewer=await login('observer')
                    # Member credentials are independent, including accounts created by the owner.
                    self.assertEqual((await c.post('/api/v1/auth/login',json={'username':'worker','password':'test-password'})).status_code,401)
                    home=(await c.get('/api/v1/household',headers=worker)).json()['data']
                    self.assertNotIn('view',home['permissions'])
                    self.assertEqual([m['username'] for m in home['members']],['worker'])
                    self.assertTrue(all('properties' not in n and 'watch' not in n for n in home['devices']))
                    for path in ['shadow','health','medicine/history','watch','watch/hours','watch/falls','door-records','reports','stats','camera','captures']:
                        self.assertEqual((await c.get('/api/v1/devices/main/'+path,headers=worker)).status_code,403,path)
                    self.assertEqual((await c.post('/api/v1/commands',headers=worker,json={'device_id':'main','command_name':'ack','client_request_id':'x'})).status_code,403)
                    event=(await c.post('/api/v1/devices/main/sos',headers=owner,json={'client_request_id':'event-1'})).json()['data']
                    key=f"{event['boot_id']}:{event['event_id']}"
                    root='/api/v1/devices/main/alarms/'+key
                    self.assertEqual((await c.get('/api/v1/devices/main/alarms',headers=worker)).status_code,200)
                    self.assertEqual((await c.post(root+'/claim',headers=viewer)).status_code,403)
                    responses=await asyncio.gather(c.post(root+'/claim',headers=worker),c.post(root+'/claim',headers=guard))
                    self.assertEqual(sorted(r.status_code for r in responses),[200,409])
                    winner=worker if responses[0].status_code==200 else guard
                    loser=guard if winner is worker else worker
                    self.assertEqual((await c.delete(root+'/claim',headers=loser)).status_code,403)
                    body={'note':'已联系社区上门查看','resolution':'handled','client_request_id':'note-1'}
                    self.assertEqual((await c.post(root+'/notes',headers=loser,json=body)).status_code,409)
                    self.assertEqual((await c.post(root+'/notes',headers=winner,json=body)).status_code,201)
                    duplicate=await c.post(root+'/notes',headers=winner,json=body)
                    self.assertTrue(duplicate.json()['data']['duplicate'])
                    self.assertEqual(len((await c.get(root+'/notes',headers=owner)).json()['data']['items']),1)
                    self.assertEqual((await c.post(root+'/notes',headers=winner,json={**body,'note':'different'})).status_code,409)
                    self.assertEqual((await c.post(root+'/claim',headers=worker)).status_code,409)
                    self.assertEqual((await c.delete('/api/v1/household/members/guardian',headers=owner)).status_code,409)
                    self.assertEqual((await c.delete('/api/v1/household/members/family',headers=worker)).status_code,403)
                    # Demotion revokes sessions and unused WebSocket tickets.
                    second=await login('worker')
                    ticket=(await c.post('/api/v1/ws-ticket?device_id=main',headers=worker)).json()['data']['ticket']
                    self.assertEqual((await c.patch('/api/v1/household/members/worker',headers=owner,json={'role':'viewer'})).status_code,200)
                    for h in (worker,second): self.assertEqual((await c.get('/api/v1/household',headers=h)).status_code,401)
                    self.assertIsNone(app.state.store.consume_ticket(ticket,'main'))
                    # Deletion retains previous notes and removes all login sessions.
                    target='worker' if winner is worker else 'guard'
                    self.assertEqual((await c.delete('/api/v1/household/members/'+target,headers=owner)).status_code,200)
                    self.assertEqual((await c.post('/api/v1/auth/login',json={'username':target,'password':'test-pass-2'})).status_code,401)
                    history=(await c.get(root+'/notes',headers=owner)).json()['data']['items']
                    self.assertEqual(len(history),1)
                    self.assertTrue(history[0]['account_deleted'])
                    self.assertEqual((await c.post('/api/v1/household/members',headers=owner,json={'username':target,'password':'test-pass-2','role':'viewer'})).status_code,409)
        asyncio.run(run())
