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
