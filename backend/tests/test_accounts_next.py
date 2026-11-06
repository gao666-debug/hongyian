import asyncio
import tempfile
import unittest
import httpx
from backend.app import create_app
from backend.settings import Settings

class AccountNextTests(unittest.TestCase):
    def execute(self,fn):
        async def run():
            with tempfile.TemporaryDirectory() as tmp:
                app=create_app(Settings(db=tmp+'/test.db',password='test-password',webhook_token='x'*32,device_id='main'))
                async with httpx.AsyncClient(transport=httpx.ASGITransport(app=app),base_url='http://test') as c:
                    async def login(name='guardian',password='test-password',agent='Test phone'):
                        r=await c.post('/api/v1/auth/login',headers={'User-Agent':agent},json={'username':name,'password':password})
                        self.assertEqual(r.status_code,200,r.text)
                        return {'Authorization':'Bearer '+r.json()['data']['access_token']}
                    await fn(app,c,login)
        asyncio.run(run())
    def test_invitation_permissions_and_membership(self):
        async def scenario(app,c,login):
            owner=await login()
            r=await c.post('/api/v1/household/invitations',headers=owner,json={'role':'viewer'})
            self.assertEqual(r.status_code,201,r.text);invite=r.json()['data']
            body={'username':'alice','password':'alice-pass-123','invitation_token':invite['invitation_token']}
            self.assertEqual((await c.post('/api/v1/auth/register-invite',json=body)).status_code,201)
            self.assertEqual((await c.post('/api/v1/auth/register-invite',json={**body,'username':'bob'})).status_code,410)
            alice=await login('alice','alice-pass-123')
            self.assertEqual((await c.get('/api/v1/devices/main/shadow',headers=alice)).status_code,200)
            self.assertEqual((await c.post('/api/v1/household/invitations',headers=alice,json={'role':'caregiver'})).status_code,403)
            self.assertEqual((await c.post('/api/v1/commands',headers=alice,json={'device_id':'main','command_name':'ack','client_request_id':'a'})).status_code,403)
            data=(await c.get('/api/v1/household/invitations',headers=owner)).json()['data'];self.assertNotIn(invite['invitation_token'],str(data));self.assertNotIn('token_hash',str(data))
            self.assertEqual((await c.delete('/api/v1/household/members/alice/membership',headers=owner)).status_code,200)
            self.assertEqual((await c.get('/api/v1/household',headers=alice)).status_code,401)
            alice=await login('alice','alice-pass-123')
            no_home=await c.get('/api/v1/household',headers=alice);self.assertEqual(no_home.status_code,200);self.assertEqual(no_home.json()['data']['devices'],[])
            self.assertEqual((await c.get('/api/v1/devices/main/shadow',headers=alice)).status_code,404)
            r=await c.post('/api/v1/household/invitations',headers=owner,json={'role':'caregiver'});token=r.json()['data']['invitation_token']
            self.assertEqual((await c.post('/api/v1/household/invitations/accept',headers=alice,json={'invitation_token':token})).status_code,200)
            self.assertEqual((await c.get('/api/v1/household',headers=alice)).json()['data']['role'],'caregiver')
            rows=(await c.get('/api/v1/household/audit',headers=owner)).json()['data']['items'];self.assertTrue(any(r['action']=='membership.remove' for r in rows))
        self.execute(scenario)
    def test_sessions_password_and_self_deletion(self):
        async def scenario(app,c,login):
            owner=await login();await c.post('/api/v1/household/members',headers=owner,json={'username':'alice','password':'alice-pass-123','role':'caregiver'})
            first=await login('alice','alice-pass-123','Phone A');second=await login('alice','alice-pass-123','Phone B')
            items=(await c.get('/api/v1/auth/sessions',headers=first)).json()['data']['items'];self.assertEqual(len(items),2);self.assertEqual(sum(x['current'] for x in items),1)
            self.assertNotIn('token_hash',str(items));other=next(x['id'] for x in items if not x['current'])
            self.assertEqual((await c.delete('/api/v1/auth/sessions/'+other,headers=owner)).status_code,404)
            self.assertEqual((await c.post('/api/v1/auth/sessions/revoke-others',headers=first)).status_code,200)
            self.assertEqual((await c.get('/api/v1/household',headers=second)).status_code,401)
            self.assertEqual((await c.post('/api/v1/auth/password',headers=first,json={'old_password':'wrong','new_password':'new-alice-pass'})).status_code,403)
            self.assertEqual((await c.post('/api/v1/auth/password',headers=first,json={'old_password':'alice-pass-123','new_password':'new-alice-pass'})).status_code,200)
            self.assertEqual((await c.get('/api/v1/household',headers=first)).status_code,401)
            self.assertEqual((await c.post('/api/v1/auth/login',json={'username':'alice','password':'alice-pass-123'})).status_code,401)
            alice=await login('alice','new-alice-pass')
            self.assertEqual((await c.request('DELETE','/api/v1/account',headers=alice,json={'password':'wrong'})).status_code,403)
            self.assertEqual((await c.request('DELETE','/api/v1/account',headers=alice,json={'password':'new-alice-pass'})).status_code,200)
            self.assertEqual((await c.get('/api/v1/household',headers=alice)).status_code,401)
            self.assertEqual((await c.request('DELETE','/api/v1/account',headers=owner,json={'password':'test-password'})).status_code,409)
            with app.state.store.connection() as db:
                self.assertFalse(db.execute("SELECT 1 FROM users WHERE name='alice'").fetchone());self.assertFalse(db.execute("SELECT 1 FROM sessions WHERE username='alice'").fetchone())
        self.execute(scenario)
    def test_invitation_revocation_and_expiry(self):
        async def scenario(app,c,login):
            owner=await login()
            for expired in (False,True):
                r=await c.post('/api/v1/household/invitations',headers=owner,json={'role':'property'});invite=r.json()['data']
                if expired:
                    with app.state.store.connection() as db:db.execute('UPDATE account_invitations SET expires=0 WHERE id=?',(invite['id'],))
                else:self.assertEqual((await c.delete('/api/v1/household/invitations/'+invite['id'],headers=owner)).status_code,200)
                r=await c.post('/api/v1/auth/register-invite',json={'username':'alice','password':'alice-pass-123','invitation_token':invite['invitation_token']});self.assertEqual(r.status_code,410)
        self.execute(scenario)
