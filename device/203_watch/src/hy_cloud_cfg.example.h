/*
 * hy_cloud_cfg.example.h —— 上云参数模板
 *
 * 复制成 hy_cloud_cfg.h 再填自己的值（hy_cloud_cfg.h 已在 .gitignore 里，
 * 不会提交；里面是 WiFi 密码和设备密钥，不能进仓库）：
 *
 *     cp src/hy_cloud_cfg.example.h src/hy_cloud_cfg.h
 *
 * 每个值的来源见 docs/上云接入说明.md。
 */
#ifndef HY_CLOUD_CFG_H
#define HY_CLOUD_CFG_H

/* ---------------- 总开关 ---------------- */
/* 0 = 只跑本地传感器，不联网（省电、不影响 200Hz 采样） */
#define HY_CLOUD_ENABLE          1

/* ---------------- WiFi ---------------- */
/* WS63 只支持 2.4GHz，手机热点要关掉 5GHz */
#define HY_WIFI_SSID             "你的热点名"
#define HY_WIFI_PWD              "你的热点密码"

/* ---------------- 华为云 IoTDA ---------------- */
/* 控制台「总览 -> 接入信息」里的 MQTT 接入地址，不带端口。
 * 设备侧是 iotda-device；iotda-app 是应用侧（AMQP/HTTP），设备连不上。 */
#define HY_HW_DOMAIN             "xxxxxx.st1.iotda-device.cn-east-3.myhuaweicloud.com"
#define HY_HW_PORT               1883          /* 1883 明文 / 8883 加密 */

/* 控制台「设备 -> 设备详情」里的设备 ID 和原始密钥。
 * 注意：DEVICES-CONNECT-KEY-*.txt 里的 password 是签名结果，不是密钥本身。 */
#define HY_HW_DEVICE_ID          "xxxxxxxxxxxxxxxxxxxxxxxx_DEVICE_ID"
#define HY_HW_DEVICE_SECRET      "填注册设备时给的原始密钥"

/* 华为云「密码签名类型」：1 = 平台校验时间戳（推荐，防重放）。
 * 明文 1883 时平台强制校验，所以必须为 1，且板子要先 SNTP 拿到 UTC 时间。 */
#define HY_HW_SIG_TYPE           1

/* 上报用的服务 ID，必须和产品物模型里那个服务的【服务 ID】逐字一致。
 * 对不上时平台会静默丢弃属性上报：连接不断、控制台仍显示在线、但物模型没有数据。 */
#define HY_HW_SERVICE_ID         "11"

/* ---------------- 上报节奏 ---------------- */
#define HY_REPORT_PERIOD_MS      10000         /* 常规属性上报周期 */
#define HY_MQTT_KEEPALIVE_S      120           /* 平台允许 30~1200 */

/* ---------------- 网络超时 ---------------- */
#define HY_WIFI_TIMEOUT_MS       45000
#define HY_WIFI_RETRY_MS         15000
#define HY_SNTP_TIMEOUT_MS       3000
#define HY_MQTT_RETRY_MS         10000

#endif /* HY_CLOUD_CFG_H */
