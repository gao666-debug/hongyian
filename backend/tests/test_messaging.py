import asyncio
import tempfile
import unittest

import httpx

from backend.app import create_app
from backend.settings import Settings

JPEG = b'\xff\xd8\xff\xe0' + b'\x00' * 200


def m4a(size=200):
    return b'\x00\x00\x00\x20ftypM4A ' + b'\x00' * size


class MessagingTests(unittest.TestCase):
    def test_contacts_conversations_and_media(self):
        async def run():
            with tempfile.TemporaryDirectory() as tmp:
                app = create_app(Settings(db=tmp + '/test.db', password='test-password',
                                          webhook_token='x' * 32, device_id='main'))
                app.state.store.bootstrap_user('bob', 'bob-password')
                app.state.store.bootstrap_user('carol', 'carol-password')

                async with httpx.AsyncClient(transport=httpx.ASGITransport(app=app),
                                             base_url='http://test') as c:
                    async def auth(name, password):
                        r = await c.post('/api/v1/auth/login',
                                         json={'username': name, 'password': password})
                        return {'Authorization': 'Bearer ' + r.json()['data']['access_token']}

                    alice = await auth('guardian', 'test-password')
                    bob = await auth('bob', 'bob-password')
                    carol = await auth('carol', 'carol-password')

                    # ---------- 好友：邀请码 ----------
                    self.assertEqual((await c.get('/api/v1/contacts')).status_code, 401)
                    code = (await c.post('/api/v1/contacts/invite', headers=alice)).json()['data']['code']
                    self.assertEqual(len(code), 6)
                    self.assertEqual((await c.post('/api/v1/contacts/accept', headers=bob,
                                                   json={'code': 'ZZZZZZ'})).status_code, 404)
                    self.assertEqual((await c.post('/api/v1/contacts/accept', headers=alice,
                                                   json={'code': code})).status_code, 422)
                    self.assertEqual((await c.post('/api/v1/contacts/accept', headers=bob,
                                                   json={'code': code})).status_code, 200)
                    # 一次性：第二个人用同一个码失败
                    self.assertEqual((await c.post('/api/v1/contacts/accept', headers=carol,
                                                   json={'code': code})).status_code, 409)
                    items = (await c.get('/api/v1/contacts', headers=alice)).json()['data']['items']
                    self.assertEqual([x['username'] for x in items], ['bob'])
                    self.assertEqual(len((await c.get('/api/v1/contacts', headers=bob))
                                         .json()['data']['items']), 1)

                    # ---------- 单聊 ----------
                    self.assertEqual((await c.post('/api/v1/conversations/direct', headers=alice,
                                                   json={'peer': 'carol'})).status_code, 403)
                    conv = (await c.post('/api/v1/conversations/direct', headers=alice,
                                         json={'peer': 'bob'})).json()['data']
                    cid = conv['conversation_id']
                    self.assertEqual((conv['kind'], conv['peer'], conv['member_count']),
                                     ('direct', 'bob', 2))
                    again = (await c.post('/api/v1/conversations/direct', headers=bob,
                                          json={'peer': 'guardian'})).json()['data']
                    self.assertEqual(again['conversation_id'], cid)

                    # ---------- 文本消息 / 历史 / 未读 / 幂等 ----------
                    m1 = (await c.post('/api/v1/conversations/%s/messages' % cid, headers=alice,
                                       json={'body': 'hello bob', 'client_id': 'k1'})).json()['data']
                    self.assertEqual(m1['kind'], 'text')
                    dup = (await c.post('/api/v1/conversations/%s/messages' % cid, headers=alice,
                                        json={'body': 'hello bob', 'client_id': 'k1'})).json()['data']
                    self.assertEqual(dup['id'], m1['id'])
                    await c.post('/api/v1/conversations/%s/messages' % cid, headers=bob,
                                 json={'body': 'hi alice', 'client_id': 'k2'})
                    hist = (await c.get('/api/v1/conversations/%s/messages' % cid,
                                        headers=alice)).json()['data']
                    self.assertEqual([x['body'] for x in hist['items']], ['hello bob', 'hi alice'])
                    self.assertEqual((await c.get('/api/v1/messages/unread', headers=alice))
                                     .json()['data']['unread'], 1)
                    self.assertEqual((await c.get('/api/v1/messages/unread', headers=bob))
                                     .json()['data']['unread'], 0)
                    await c.post('/api/v1/conversations/%s/read' % cid, headers=alice)
                    self.assertEqual((await c.get('/api/v1/messages/unread', headers=alice))
                                     .json()['data']['unread'], 0)
                    # 非成员读不到
                    self.assertEqual((await c.get('/api/v1/conversations/%s/messages' % cid,
                                                  headers=carol)).status_code, 403)

                    # ---------- 媒体：图片 ----------
                    up = await c.post('/api/v1/media?conversation_id=' + cid,
                                      headers={**alice, 'Content-Type': 'image/jpeg'}, content=JPEG)
                    self.assertEqual(up.status_code, 201, up.text)
                    media = up.json()['data']
                    self.assertEqual((media['kind'], media['size']), ('image', len(JPEG)))
                    msg = (await c.post('/api/v1/conversations/%s/messages/media' % cid,
                                        headers=alice,
                                        json={'media_id': media['id'], 'client_id': 'k3'})).json()['data']
                    self.assertEqual(msg['kind'], 'image')
                    self.assertEqual(msg['body'], media['id'])
                    got = await c.get('/api/v1/media/' + media['id'], headers=bob)
                    self.assertEqual((got.status_code, got.content), (200, JPEG))
                    self.assertEqual((await c.get('/api/v1/media/' + media['id'],
                                                  headers=carol)).status_code, 403)

                    # ---------- 媒体：类型 / 内容 / 时长校验 ----------
                    self.assertEqual((await c.post('/api/v1/media?conversation_id=' + cid,
                                                   headers={**alice, 'Content-Type': 'application/zip'},
                                                   content=b'PK\x03\x04' + b'\x00' * 50)).status_code, 415)
                    self.assertEqual((await c.post('/api/v1/media?conversation_id=' + cid,
                                                   headers={**alice, 'Content-Type': 'image/jpeg'},
                                                   content=b'not an image at all')).status_code, 415)
                    voice = await c.post('/api/v1/media?conversation_id=%s&duration=3' % cid,
                                         headers={**alice, 'Content-Type': 'audio/mp4'}, content=m4a())
                    self.assertEqual(voice.status_code, 201, voice.text)
                    self.assertEqual(voice.json()['data']['kind'], 'voice')
                    self.assertEqual((await c.post('/api/v1/media?conversation_id=%s&duration=90' % cid,
                                                   headers={**alice, 'Content-Type': 'audio/mp4'},
                                                   content=m4a())).status_code, 422)

                    # ---------- 群聊 ----------
                    grp = await c.post('/api/v1/conversations', headers=alice,
                                       json={'name': '家里群', 'members': ['bob']})
                    self.assertEqual(grp.status_code, 201, grp.text)
                    gid = grp.json()['data']['conversation_id']
                    self.assertEqual((grp.json()['data']['kind'],
                                      grp.json()['data']['member_count']), ('group', 2))
                    # 媒体不能跨会话引用（单聊里的图片不能直接发到群）
                    self.assertEqual((await c.post('/api/v1/conversations/%s/messages/media' % gid,
                                                   headers=alice,
                                                   json={'media_id': media['id']})).status_code, 422)
                    # 非好友不能拉进群
                    self.assertEqual((await c.post('/api/v1/conversations', headers=alice,
                                                   json={'name': 'x', 'members': ['carol']})).status_code, 403)
                    # 超过 10 人（Pydantic 层拒绝）
                    self.assertEqual((await c.post('/api/v1/conversations', headers=alice,
                                                   json={'name': 'x',
                                                         'members': ['m%d' % i for i in range(10)]}
                                                   )).status_code, 422)
                    # 权限：只有群主能改名 / 加人 / 踢人
                    self.assertEqual((await c.patch('/api/v1/conversations/%s' % gid, headers=bob,
                                                    json={'name': '改名'})).status_code, 403)
                    self.assertEqual((await c.post('/api/v1/conversations/%s/members' % gid,
                                                   headers=alice,
                                                   json={'username': 'carol'})).status_code, 200)
                    self.assertEqual((await c.patch('/api/v1/conversations/%s' % gid, headers=alice,
                                                    json={'name': '颐鸿安之家'})).json()['data']['name'],
                                     '颐鸿安之家')
                    # 群消息扇出到 bob（先清掉他在单聊里的未读，避免累计干扰）
                    await c.post('/api/v1/conversations/%s/read' % cid, headers=bob)
                    self.assertEqual((await c.get('/api/v1/messages/unread', headers=bob))
                                     .json()['data']['unread'], 0)
                    await c.post('/api/v1/conversations/%s/messages' % gid, headers=alice,
                                 json={'body': '群消息', 'client_id': 'g1'})
                    self.assertEqual((await c.get('/api/v1/messages/unread', headers=bob))
                                     .json()['data']['unread'], 1)
                    # 踢人后立刻失去访问权
                    self.assertEqual((await c.delete('/api/v1/conversations/%s/members/carol' % gid,
                                                     headers=alice)).status_code, 200)
                    self.assertEqual((await c.get('/api/v1/conversations/%s/messages' % gid,
                                                  headers=carol)).status_code, 403)
                    # 成员可自行退群
                    self.assertEqual((await c.delete('/api/v1/conversations/%s/members/bob' % gid,
                                                     headers=bob)).status_code, 200)
                    self.assertEqual((await c.get('/api/v1/conversations/%s/messages' % gid,
                                                  headers=bob)).status_code, 403)

                    # ---------- 会话列表 ----------
                    convs = (await c.get('/api/v1/conversations', headers=alice)).json()['data']['items']
                    kinds = sorted(x['kind'] for x in convs)
                    self.assertEqual(kinds, ['direct', 'group'])

                    # ---------- 删除好友 ----------
                    self.assertEqual((await c.delete('/api/v1/contacts/bob', headers=alice)).status_code, 200)
                    self.assertEqual((await c.get('/api/v1/contacts', headers=alice))
                                     .json()['data']['items'], [])

        asyncio.run(run())


if __name__ == '__main__':
    unittest.main()
