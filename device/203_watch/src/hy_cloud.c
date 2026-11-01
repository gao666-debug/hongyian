/*
 * hy_cloud.c —— 华为云 IoTDA 上云任务
 *
 * 流程：
 *   WiFi 关联 -> SNTP 取 UTC -> 算三元组 -> MQTT 连 IoTDA -> 周期上报属性
 * 断了就从 WiFi 这一层重新来，不做半吊子重连。
 *
 * 时间戳说明：ClientId 里的时间戳是 "YYYYMMDDHH"（UTC，精确到小时），
 * password = hex(HMAC-SHA256(key = 时间戳字符串, msg = 设备密钥))。
 * 明文 1883 接入时平台会强制校验这个时间戳，所以每小时的整点前后
 * 重连一次是安全的；连接本身维持着就不用管。
 */
#include "hy_cloud.h"
#include "hy_cloud_cfg.h"
#include "hy_net.h"
#include "hy_mqtt.h"
#include "hy_sha256.h"
#include "hy_sensor.h"

#include <stdio.h>
#include <string.h>

#include "soc_osal.h"
#include "tcxo.h"
#include "watchdog.h"

#define HY_CLOUD_LOG          "[hy-cloud]"
#define HY_CLOUD_TOPIC_LEN    192
#define HY_CLOUD_PAY_LEN      384
#define HY_CLOUD_CID_LEN      160
#define HY_CLOUD_PASS_LEN     80
#define HY_CLOUD_PING_MS      60000
#define HY_CLOUD_RETRY_MAX    3

static hy_mqtt_t g_mqtt;
static int      g_online = 0;

/* SNTP 基准，避免每次都发 NTP 请求 */
static uint32_t g_utc_base;
static uint64_t g_utc_base_ms;
static int      g_utc_valid = 0;
static int      g_last_fall_events = -1;

int hy_cloud_online(void)
{
    return g_online;
}

static uint32_t hy_utc_now(void)
{
    if (g_utc_valid == 0) {
        return 0;
    }
    return g_utc_base + (uint32_t)((uapi_tcxo_get_ms() - g_utc_base_ms) / 1000ULL);
}

static int hy_cloud_sync_time(void)
{
    int i;

    for (i = 0; i < HY_CLOUD_RETRY_MAX; i++) {
        uint32_t utc = 0;

        if (hy_net_sntp(&utc, HY_SNTP_TIMEOUT_MS) == 0) {
            g_utc_base = utc;
            g_utc_base_ms = (uint64_t)uapi_tcxo_get_ms();
            g_utc_valid = 1;
            return 0;
        }
        printf("%s SNTP 第 %d 次失败\r\n", HY_CLOUD_LOG, i + 1);
        osal_msleep(1000);
    }

    return -1;
}

/* 生成 ClientId 和 Password */
static void hy_cloud_auth(char *client_id, char *password)
{
    char stamp[12];
    char hexbuf[65];
    uint8_t mac[32];
    const char *secret = HY_HW_DEVICE_SECRET;
    size_t slen = strlen(secret);

    hy_net_utc_stamp(hy_utc_now(), stamp);
    hy_hmac_sha256((const uint8_t *)stamp, (uint32_t)strlen(stamp),
                   (const uint8_t *)secret, (uint32_t)slen, mac);
    hy_hex_encode(mac, 32, hexbuf);

    (void)snprintf(client_id, HY_CLOUD_CID_LEN, "%s_0_%d_%s",
                   HY_HW_DEVICE_ID, HY_HW_SIG_TYPE, stamp);
    (void)snprintf(password, HY_CLOUD_PASS_LEN, "%s", hexbuf);
}

static int hy_cloud_mqtt_open(void)
{
    char client_id[HY_CLOUD_CID_LEN];
    char password[HY_CLOUD_PASS_LEN];
    int ret;

    hy_cloud_auth(client_id, password);

    printf("%s 连接 %s:%d\r\n", HY_CLOUD_LOG, HY_HW_DOMAIN, HY_HW_PORT);
    printf("%s clientId=%s\r\n", HY_CLOUD_LOG, client_id);

    ret = hy_mqtt_connect(&g_mqtt, HY_HW_DOMAIN, (uint16_t)HY_HW_PORT,
                          client_id, HY_HW_DEVICE_ID, password,
                          (uint16_t)HY_MQTT_KEEPALIVE_S);
    if (ret != HY_MQTT_OK) {
        printf("%s MQTT 连接失败 ret=%d (auth_code=%d)\r\n",
               HY_CLOUD_LOG, ret, g_mqtt.last_connack);
        if (ret == HY_MQTT_ERR_AUTH) {
            printf("%s 三元组对不上：先核对设备ID / 密钥 / 域名，再确认板子时间\r\n", HY_CLOUD_LOG);
        }
        return ret;
    }

    printf("%s 已连上华为云 IoTDA\r\n", HY_CLOUD_LOG);
    g_online = 1;
    return 0;
}

static int hy_cloud_report(void)
{
    char topic[HY_CLOUD_TOPIC_LEN];
    char payload[HY_CLOUD_PAY_LEN];
    int n;

    (void)snprintf(topic, sizeof(topic),
                   "$oc/devices/%s/sys/properties/report", HY_HW_DEVICE_ID);

    n = snprintf(payload, sizeof(payload),
                 "{\"services\":[{\"service_id\":\"%s\",\"properties\":{"
                 "\"heart_rate\":%d,\"spo2\":%d,\"accel_x\":%d,\"accel_y\":%d,\"accel_z\":%d,"
                 "\"steps\":%d,\"fall_state\":%d,\"fall_count\":%d}}]}",
                 HY_HW_SERVICE_ID,
                 g_hy.hr, g_hy.spo2, g_hy.ax, g_hy.ay, g_hy.az,
                 g_hy.steps, g_hy.fall_state, g_hy.fall_events);

    if (n <= 0 || n >= (int)sizeof(payload)) {
        printf("%s JSON 组装溢出\r\n", HY_CLOUD_LOG);
        return -1;
    }

    if (hy_mqtt_publish(&g_mqtt, topic, payload, n) != HY_MQTT_OK) {
        printf("%s 上报失败\r\n", HY_CLOUD_LOG);
        g_online = 0;
        return -1;
    }

    printf("%s 上报 hr=%d spo2=%d steps=%d fall=%d(%d次) 线上=%d\r\n",
           HY_CLOUD_LOG, g_hy.hr, g_hy.spo2, g_hy.steps,
           g_hy.fall_state, g_hy.fall_events, g_online);

    return 0;
}

/* 摔倒立即补报一条，不等周期 */
static void hy_cloud_check_fall(void)
{
    if (g_last_fall_events == g_hy.fall_events) {
        return;
    }

    g_last_fall_events = g_hy.fall_events;
    if (g_hy.fall_events > 0 && g_hy.fall_state == 2) {
        printf("%s 检测到摔倒，立即上报\r\n", HY_CLOUD_LOG);
        (void)hy_cloud_report();
    }
}

void hy_cloud_task(void)
{
    uint64_t next_report = 0;
    uint64_t next_ping = 0;

    printf("%s 上云任务启动，服务=%s 周期=%dms\r\n",
           HY_CLOUD_LOG, HY_HW_SERVICE_ID, HY_REPORT_PERIOD_MS);

    for (;;) {
        /* ---- 1. 联网 ---- */
        if (hy_net_is_up() == 0) {
            g_online = 0;
            if (hy_net_wifi_connect(HY_WIFI_SSID, HY_WIFI_PWD, HY_WIFI_TIMEOUT_MS) != 0) {
                printf("%s WiFi 连接失败，%d 秒后重试\r\n", HY_CLOUD_LOG, HY_WIFI_RETRY_MS / 1000);
                uapi_watchdog_kick();
                osal_msleep(HY_WIFI_RETRY_MS);
                continue;
            }
        }

        /* ---- 2. 取时 ---- */
        if (g_utc_valid == 0) {
            if (hy_cloud_sync_time() != 0) {
                printf("%s 取不到网络时间，60 秒后重试\r\n", HY_CLOUD_LOG);
                osal_msleep(60000);
                continue;
            }
        }

        /* ---- 3. 连 IoTDA ---- */
        if (hy_cloud_mqtt_open() != 0) {
            uapi_watchdog_kick();
            osal_msleep(HY_MQTT_RETRY_MS);
            continue;
        }

        /* ---- 4. 在线循环 ---- */
        next_report = (uint64_t)uapi_tcxo_get_ms();
        next_ping = next_report + HY_CLOUD_PING_MS;

        while (hy_net_is_up() != 0) {
            uint64_t now = (uint64_t)uapi_tcxo_get_ms();

            uapi_watchdog_kick();
            hy_cloud_check_fall();

            if (now >= next_report) {
                if (hy_cloud_report() != 0) {
                    break;
                }
                next_report = now + HY_REPORT_PERIOD_MS;
            }

            if (now >= next_ping) {
                if (hy_mqtt_ping(&g_mqtt) != HY_MQTT_OK) {
                    printf("%s 心跳失败，重连\r\n", HY_CLOUD_LOG);
                    break;
                }
                next_ping = now + HY_CLOUD_PING_MS;
            }

            if (hy_mqtt_probe(&g_mqtt) != 0) {
                printf("%s 链路已断，重连\r\n", HY_CLOUD_LOG);
                break;
            }

            osal_msleep(500);
        }

        g_online = 0;
        hy_mqtt_close(&g_mqtt);
        osal_msleep(1000);
    }
}
