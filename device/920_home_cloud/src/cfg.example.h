/* ⚠️ 本文件已脱敏：WiFi 与 IoTDA 密钥为占位符，实际烧录前必须填写真实值。
 *    device_id / client_id / service_id 保留原值，用于核对联调配置。 */
/**
 * 920_home_cloud 统一配置
 *
 * 所有需要改的东西都在这个文件里，别的 .c 不要出现魔法数字。
 * 改完只要重新编译烧录即可，不用到处找。
 */

#ifndef HOME_CLOUD_CFG_H
#define HOME_CLOUD_CFG_H

/* ---------------- Wi-Fi（只支持 2.4GHz，5G 热点连不上） ---------------- */
#define CFG_WIFI_SSID               "YOUR_WIFI_SSID"
#define CFG_WIFI_PASS               "YOUR_WIFI_PASSWORD"

/* ---------------- 华为云 IoTDA 设备参数 ----------------
 * clientId 里的时间戳是 UTC（实测确认），下面这组是控制台直接给的现成组合。
 * 实测时间戳不按小时严格校验，尚不能由跨小时试连推断永久有效。
 * ⚠️ 含设备密钥，属敏感信息，本文件不要提交到公开仓库。 */
#define CFG_IOTDA_HOST              "YOUR_INSTANCE.st1.iotda-device.cn-east-3.myhuaweicloud.com"
#define CFG_IOTDA_PORT              1883        /* 明文；改 8883 需配 TLS */
#define CFG_IOTDA_DEVICE_ID         "demo-environment"
#define CFG_IOTDA_CLIENT_ID         "demo-environment_0_0_2026110103"
#define CFG_IOTDA_PASSWORD          "YOUR_IOTDA_MQTT_PASSWORD" /* 已计算的 MQTT password，不是原始密钥 */

/* 物模型服务 ID。⚠️ 必须与控制台「产品 → 物模型」里的服务ID 完全一致，
 * 对不上平台会丢弃整条属性上报（消息跟踪里会写 service ID is not defined）。 */
#define CFG_IOTDA_SERVICE_ID        "11"

/* ---------------- 上报节奏 ---------------- */
#define CFG_REPORT_PERIOD_MS        10000       /* 定时全量上报：10 秒 */
#define CFG_EVENT_REPORT_MIN_MS     1000        /* 事件上报的最小间隔，防抖 */

/* ---------------- 传感器引脚（扩展板） ---------------- */
#define CFG_LIGHT_ADC_CH            5           /* 光敏，adc 通道 5 */
#define CFG_SMOKE_ADC_CH            0           /* 烟雾，adc 通道 0 */
#define CFG_PIR_PIN                 14          /* 人体感应，GPIO14 */
#define CFG_DOOR_PIN                6           /* 门磁，GPIO6 */

/* 门磁极性：磁铁靠近时读高电平 → 门=关闭 */
#define CFG_DOOR_MAGNET_NEAR_IS_HIGH    1
#define CFG_DOOR_DEBOUNCE               3       /* 连续 3 次判定稳定 */

/* ---------------- 灯（扩展板 IO10 灯组） ---------------- */
#define CFG_LAMP_PIN                10          /* 开门提示灯 */
#define CFG_TAIL_LED_PIN            9           /* 主板灯带接口，本板实测点不亮，拉低 */
#define CFG_LAMP_HOLD_MS            15000       /* 关门后灯继续亮 15 秒 */

/* ---------------- 任务参数 ---------------- */
#define CFG_SENSOR_TICK_MS          200         /* 传感器任务周期 */
#define CFG_ADC_EVERY_TICKS         5           /* 每 5 个 tick（1 秒）读一次 ADC */
#define CFG_SENSOR_TASK_PRIO        25
#define CFG_SENSOR_STACK_SIZE       0x1000
#define CFG_APP_TASK_PRIO           13
#define CFG_APP_STACK_SIZE          0x4000      /* MQTT + JSON 组包，给足 */

/* 固件版本号，会随 device_health 上报 */
#define CFG_FW_VERSION              "ws63-home-1.0"

#endif /* HOME_CLOUD_CFG_H */
