# 居家安全后端

Python 3.10、FastAPI、SQLite、华为云官方 IoTDA SDK。App 使用登录令牌访问后端，后端以云端应用身份查询影子和下发命令，不占用开发板 MQTT 设备连接。

## 最新状态（2026-10-17）

请以 [联调状态更新](docs/联调状态更新_20261017.md) 为准。
实板已经通过非TLS MQTT 1883连接IoTDA并收到属性PUBACK；板端8883 TLS暂缓，不阻挡当前方案联调。
本机真云影子、模拟器刷新、AMQP报警/健康/统计入库此前已实测。公网服务器迁移后的实板全链路仍待验收，不能等同于本机已验证。
App公网地址：https://backend.example.com；公开IP证书已适配，服务部署状态需按服务器实测确认。

## 服务器运行

详细步骤见 [服务器部署说明](docs/服务器部署说明.md)。实际约定目录为 /home/deploy/app（说明中的示例目录需替换）。
```bash
cd /home/deploy/app
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt
cp .env.example .env
chmod 600 .env
# 编辑.env，填入部署密码、WEBHOOK_TOKEN及云端参数，设BACKEND_MODE=live
.venv/bin/python run.py --host 127.0.0.1 --port 8000
```
独立AMQP进程：
```bash
AMQP_LOCAL_BACKEND=http://127.0.0.1:8000 .venv/bin/python consume_cloud.py
```
Uvicorn单worker；两个进程分别管理。nginx公网8443转发到HTTP 127.0.0.1:8000，保留WS升级头。
健康检查 /healthz，匿名业务接口应401。API健康不单独证明云端消息或设备在线。
AMQP队列原配置REPLACE_WITH_LOCAL_VALUE78，INSTANCE_ID为空；同一队列只保留一个接收实例，避免消息分流。

## 配置与鉴权

模板 .env.example 默认mock；服务器应配置live及IOTDA_PROJECT_ID、HUAWEICLOUD_SDK_AK/SK、AMQP凭据。凭据另行安全配置，不在普通代码交付包内。
APP_USERNAME默认guardian，APP_PASSWORD与WEBHOOK_TOKEN在服务器设置。已有SQLite账号不会因修改环境密码自动更新，升级保留数据库，不要误删历史。
登录POST /api/v1/auth/login；后续请求携带Bearer令牌，WS使用一次性票据。详见 [App对接回执](docs/App对接回执.md)。
真实云API失败返回错误，不伪造模拟成功。mock响应显式simulated=true，不代表实测。

## 本机HTTPS演示（可选）

本机目录 /home/gwb/oh_code/eldercare-backend，与服务器目录不同。
```bash
.venv/bin/python tools_init.py
.venv/bin/python tools_tls.py --ip <本机局域网IP>
.venv/bin/python run.py --host 0.0.0.0 --port 8443 --cert .local/tls/server.crt --key .local/tls/server.key
```
本机AMQP默认通知目标https://127.0.0.1:8443，使用.local/tls/development-ca.crt验证；不要与服务器HTTP8000配置混用。
仅公开证书可分享，不分享私钥。本机网络转发按deploy目录脚本配置。

## 测试

```bash
.venv/bin/python -m unittest discover -s tests -v
```
包含鉴权、幂等命令、回执、报警去重、统计、WebSocket、AMQP和真云数值序列化等测试。自动化通过不代表服务器实板端到端验收通过。


# 生活提醒更新（2026-10-17）

新增夜灯与每日服药提醒，说明见 [生活提醒联调_v3.4.0.md](docs/生活提醒联调_v3.4.0.md)。服务器须更新 backend/app.py、backend/cloud.py 后重启现有 API/AMQP 服务，保留 .env 与数据库。本次不新增依赖或数据库迁移。

# 手表数据接入（2026-11-01）

WS63 手表固件（`device/203_watch`）每 10 秒向 IoTDA 上报一次属性，后端把这条链路接进现有模型，App 不需要再区分两颗芯片。

## 数据流

```
手表 hy_cloud.c  ──MQTT 1883──▶  IoTDA 设备影子
                                    │  属性上报 device.property/report
                                    ▼
                            AMQP 订阅队列
                                    │  consume_cloud.py（同一进程，多认一种 resource）
                                    ▼
                POST /api/v1/iotda/notifications  （X-Webhook-Token）
                                    │
                    backend/watch.py 归一化 + 校验 + mg→g
                                    ▼
        SQLite: watch(最新值) / watch_hour(小时桶) / watch_fall(摔倒事件)
                                    │
                            WebSocket 推送 type=watch / watch_fall
                                    ▼
                                    App
```

## 改动点

| 文件 | 作用 |
| --- | --- |
| `backend/watch.py` | 新增。挑服务、范围校验、单位换算、派生字段（`motion_mg`、`posture`、`fall_active`） |
| `backend/app.py` | webhook 同时收 `device.message` 与 `device.property`；新增 `/watch` 三个查询接口与 mock 灌数接口；摔倒计数增加时落事件并实时推送 |
| `backend/store.py` | 新增 `watch_hour()`：小时桶（心率/血氧 min·max·均值、步数增量、摔倒次数），避免把 10 秒一条全量入库 |
| `backend/cloud.py` | 影子查询按配置的服务 ID 找主板，找不到退回手表并附带归一化结果；命令下发不再写死 `HomeSafety` |
| `backend/settings.py` | 新增 `WATCH_DEVICE_ID` / `IOTDA_SERVICE_ID` / `WATCH_SERVICE_ID` |
| `consume_cloud.py` | 放行 `device.property`，设备白名单加入手表设备 ID |

## 接口

```bash
GET  /api/v1/devices/{device}/watch          最新一条归一化数据（404 表示还没上报过）
GET  /api/v1/devices/{device}/watch/hours    小时桶，limit 1~720，默认 24
GET  /api/v1/devices/{device}/watch/falls    摔倒事件，按 fall_count 递增去重
POST /api/v1/mock/watch?device_id=...        mock 模式灌一条属性上报（联调 App 用）
```

`{device}` 用 `DEVICE_ID`；`WATCH_DEVICE_ID` 只是受理上报时的白名单，数据统一挂在 `DEVICE_ID` 下。

## 两条约定

- `heart_rate` / `spo2` 为 0 表示**这次没测到**（手指没贴上），不是真实读数。接口里另给 `hr_measuring` 标志，小时桶也不把 0 计入均值。
- `fall_state` 是此刻状态（0 正常 / 1 冲击后观察中 / 2 疑似摔倒），`fall_count` 是上电以来累计次数（重启归零）。后端只在 `fall_count` **变大**时产生摔倒事件。

## 配置

```bash
# .env
WATCH_DEVICE_ID=6ae6a5c1e094d615927efdbd_DEVICE_ID   # 手表在 IoTDA 上的设备 ID
WATCH_SERVICE_ID=11                                  # 必须是产品物模型里那个服务 ID
IOTDA_SERVICE_ID=HomeSafety                          # 主板服务 ID，保持原值
```

服务 ID 写错时平台会**静默丢弃**属性上报：连接不断、控制台显示在线、后端一条数据都收不到。排查方法见 `device/203_watch/docs/上云接入说明.md`。
