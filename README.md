# 鸿颐安（Hongyian）

基于 OpenHarmony 与星闪的居家养老安全监护系统。

## 目录

- `backend/`：FastAPI 后端、SQLite、IoTDA 应用接口、AMQP 消费者与测试。
- `app/`：HarmonyOS ArkTS App，含总览、报警处理、趋势与生活提醒。
- `docs/`：架构规划与协作说明。

## 本地运行后端

```bash
cd backend
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
python tools_init.py
python run.py
```

默认使用 mock 模式。真实联云需在本机 `.env` 配置 IoTDA 应用凭据，启动 `python consume_cloud.py`。参考 `backend/.env.example`；已有数据库改密码使用 `tools_reset_login.py`。

## App 构建

使用 DevEco Studio 打开 `app/`，配置本机 SDK 与签名，再运行模拟器或真机。仓库中的后端地址为 `https://backend.example.com`，设备标识为 `demo-device`，须按部署环境替换。自签证书部署需提供对应受信任证书，证书文件不包含任何私钥。

当前业务仍以单主板为基础；多设备聚合、星闪节点、摄像头与 App 联动属于后续规划，详见 `docs/架构与协作规划.md`。本仓库不包含板卡 SDK、固件二进制、运行数据库、真实环境配置或生产密钥。

上传代码不会自动部署服务器或更新手机、开发板。
