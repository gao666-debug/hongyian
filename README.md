> 2026-11-12 更新：App **1.16.1**、后端 **1.13.0 + 报警提醒接口**、摄像头固件 **camera-v0.3.2**。
> 本轮把「房门」「活动记录」从三级页压平为二级页（进入即见全部内容，返回按钮固定顶部），并修复流畅模式流任务分配失败导致 `/stream-ticket` 恒 503 的问题。
> 部署步骤见 [部署说明_1.16.1](docs/部署说明_1.16.1.md)。
>
> 上一轮交接：[功能进度与顺序](docs/交付与部署_20261106.md)、[后端部署及消息接口](docs/设备消息与账号部署_1.12.0.md)、[App 使用说明](app/docs/App1.15.0使用说明.md)。

# 颐鸿安（Yihongan）

基于 OpenHarmony 与星闪的居家养老安全监护系统。

## 目录

- `backend/`：FastAPI 后端、SQLite、IoTDA 应用接口、AMQP 消费者与测试。
- `app/`：HarmonyOS ArkTS App，含总览、报警处理、趋势与生活提醒。
- `device/203_watch/`：WS63 手表固件（1.28″ 圆屏 + MPU6050 + MAX3010x + 摔倒检测 + IoTDA 上云），含整板自检、点亮排查与上云接入说明。
- `docs/`：架构规划与协作说明。

## 手表 → 云端 → 后端

WS63 手表每 10 秒把心率、血氧、三轴加速度、步数、摔倒状态上报到 IoTDA；
后端通过 AMQP 订阅属性上报通知，校验归一化后落库并通过 WebSocket 推给 App：

```
手表 MQTT ─▶ IoTDA ─▶ AMQP ─▶ consume_cloud.py ─▶ /api/v1/iotda/notifications
        ─▶ backend/watch.py ─▶ SQLite(watch/watch_hour/watch_fall) ─▶ WS
```

新接口 `GET /api/v1/devices/{device}/watch`、`/watch/hours`、`/watch/falls`，
字段约定与配置见 [`backend/README.md`](backend/README.md#手表数据接入2026-11-01)，
设备侧步骤见 [`device/203_watch/docs/上云接入说明.md`](device/203_watch/docs/上云接入说明.md)。


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

当前支持主板、环境节点与手表的家庭设备展示，以及摄像头按需观看、App 主动呼救、批量处置和夜灯控制。App 1.12.1 / API 1.8.1 / 主板 v3.5.50，近期变更、部署顺序与实测边界见 [本轮更新](docs/本轮更新_20261105.md) 和 [交付与验收清单](docs/交付与验收清单.md)。Push Kit 暂缓，系统推送尚未接通。本仓库不包含板卡 SDK、固件二进制、运行数据库、真实环境配置或生产密钥（手表上云参数模板见 `device/203_watch/src/hy_cloud_cfg.example.h`）。

上传代码不会自动部署服务器或更新手机、开发板。
