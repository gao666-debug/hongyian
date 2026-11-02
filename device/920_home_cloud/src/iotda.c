/**
 * 华为云 IoTDA 上报实现
 */

#include "cfg.h"
#include "iotda.h"
#include "net.h"

#include "stdio.h"
#include "string.h"
#include "common_def.h"
#include "soc_osal.h"
#include "MQTTClient.h"
#include "MQTTClientPersistence.h"

/* MQTTClient_init 不在公开头文件里，SDK 的 patch 内部实现，需要自行声明 */
extern int MQTTClient_init(void);

#define IOTDA_LOG               "[IOTDA] "

static MQTTClient g_client;
static td_bool    g_connected = TD_FALSE;
static td_char    g_topic_prop[128];
static td_char    g_topic_msg[128];
static td_u32     g_msg_seq = 0;
static td_u32     g_boot_id = 0;

/* paho 的 messageArrived 必须返回 int（1=成功，0=让库重投） */
static td_s32 on_mqtt_msg(td_void *context, td_char *topic_name, td_s32 topic_len,
                          MQTTClient_message *message)
{
    unused(context);
    unused(topic_len);

    osal_printk(IOTDA_LOG "downlink <- %s\r\n", (topic_name != NULL) ? topic_name : "?");
    if (message != NULL && message->payload != NULL) {
        osal_printk(IOTDA_LOG "  %.*s\r\n", message->payloadlen, (td_char *)message->payload);
        MQTTClient_freeMessage(&message);
    }
    if (topic_name != NULL) {
        MQTTClient_free(topic_name);
    }
    return 1;
}

static td_void on_mqtt_connlost(td_void *context, td_char *cause)
{
    unused(context);
    g_connected = TD_FALSE;
    osal_printk(IOTDA_LOG "connection lost: %s\r\n", (cause != NULL) ? cause : "-");
}

static td_void on_mqtt_delivered(td_void *context, MQTTClient_deliveryToken dt)
{
    unused(context);
    unused(dt);
}

static td_s32 iotda_publish(const td_char *topic, const td_char *payload)
{
    MQTTClient_message msg = MQTTClient_message_initializer;
    MQTTClient_deliveryToken token;
    td_s32 rc;

    if (g_connected == TD_FALSE) {
        return -1;
    }
    msg.payload = (void *)payload;
    msg.payloadlen = (int)strlen(payload);
    msg.qos = 0;
    msg.retained = 0;

    rc = MQTTClient_publishMessage(g_client, topic, &msg, &token);
    if (rc != MQTTCLIENT_SUCCESS) {
        osal_printk(IOTDA_LOG "publish rc=%d\r\n", rc);
        return rc;
    }
    rc = MQTTClient_waitForCompletion(g_client, token, 5000);
    if (rc != MQTTCLIENT_SUCCESS) {
        osal_printk(IOTDA_LOG "publish wait rc=%d\r\n", rc);
    }
    return rc;
}

td_s32 iotda_connect(td_void)
{
    MQTTClient_connectOptions opts = MQTTClient_connectOptions_initializer;
    td_char addr[128];
    td_char topic_cmd[128];
    td_s32 rc;

    (void)snprintf(addr, sizeof(addr), "tcp://%s:%d", CFG_IOTDA_HOST, CFG_IOTDA_PORT);

    /* 支持重连：先清掉上一次的客户端实例 */
    if (g_client != NULL) {
        MQTTClient_destroy(&g_client);
        g_client = NULL;
        g_connected = TD_FALSE;
    }
    (void)snprintf(g_topic_prop, sizeof(g_topic_prop),
                   "$oc/devices/%s/sys/properties/report", CFG_IOTDA_DEVICE_ID);
    (void)snprintf(g_topic_msg, sizeof(g_topic_msg),
                   "$oc/devices/%s/sys/messages/up", CFG_IOTDA_DEVICE_ID);
    (void)snprintf(topic_cmd, sizeof(topic_cmd),
                   "$oc/devices/%s/sys/commands/#", CFG_IOTDA_DEVICE_ID);

    MQTTClient_init();
    rc = MQTTClient_create(&g_client, addr, CFG_IOTDA_CLIENT_ID, MQTTCLIENT_PERSISTENCE_NONE, NULL);
    if (rc != MQTTCLIENT_SUCCESS) {
        osal_printk(IOTDA_LOG "create failed rc=%d\r\n", rc);
        return rc;
    }

    opts.keepAliveInterval = 60;
    opts.cleansession = 1;
    opts.username = CFG_IOTDA_DEVICE_ID;
    opts.password = CFG_IOTDA_PASSWORD;

    /* 回调必须在 connect 之前挂上 */
    MQTTClient_setCallbacks(g_client, NULL, on_mqtt_connlost, on_mqtt_msg, on_mqtt_delivered);

    rc = MQTTClient_connect(g_client, &opts);
    if (rc != MQTTCLIENT_SUCCESS) {
        osal_printk(IOTDA_LOG "connect FAILED rc=%d (1=协议 2=clientId 3=服务 4=密码 5=未授权)\r\n", rc);
        MQTTClient_destroy(&g_client);
        return rc;
    }
    g_connected = TD_TRUE;

    /* boot_id 每次开机唯一且递增：后端用 (boot_id, up) 判新旧，
     * 写死会让重启后的上报被判成“过期”而静默丢弃 */
    g_boot_id = net_epoch_now();
    if (g_boot_id == 0) {
        g_boot_id = net_uptime_sec() + 1;
    }

    osal_printk(IOTDA_LOG "connect OK, service=%s\r\n", CFG_IOTDA_SERVICE_ID);

    /* 下行命令通道（后端 /api/v1/commands 会用到） */
    if (MQTTClient_subscribe(g_client, topic_cmd, 1) == MQTTCLIENT_SUCCESS) {
        osal_printk(IOTDA_LOG "subscribed commands/#\r\n");
    } else {
        osal_printk(IOTDA_LOG "subscribe commands failed\r\n");
    }
    return 0;
}

td_s32 iotda_report(const sensors_snapshot_t *s)
{
    td_char payload[512];
    td_u32 now = net_epoch_now();
    td_u32 up = net_uptime_sec();
    const td_char *door = (s->door_open != 0) ? "OPEN" : "CLOSED";
    td_s32 rc1, rc2;

    if (g_connected == TD_FALSE) {
        return -1;
    }

    /* 1) 属性上报：字段名必须与物模型一致（light/smoke/body/door） */
    (void)snprintf(payload, sizeof(payload),
        "{\"services\":[{\"service_id\":\"%s\",\"properties\":{"
        "\"light\":%u,\"smoke\":%u,\"body\":%u,\"door\":\"%s\"}}]}",
        CFG_IOTDA_SERVICE_ID,
        (unsigned)s->light_mv, (unsigned)s->smoke_mv, (unsigned)s->pir, door);
    rc1 = iotda_publish(g_topic_prop, payload);

    /* 2) 自定义消息：后端 /api/v1/iotda/notifications 只接受这一种格式。
     * id 必须每条唯一，否则后端 INSERT OR IGNORE 会当重复丢掉。
     * Health 模型必填：fw/boot_id/epoch/up/env_valid/imu_valid/
     *                   door_enabled/wireless_btn_enabled/mute/tls */
    g_msg_seq++;
    (void)snprintf(payload, sizeof(payload),
        "{\"name\":\"device_health\",\"id\":\"ws63-%u-%u\",\"content\":{"
        "\"fw\":\"%s\",\"boot_id\":%u,\"epoch\":%u,\"up\":%u,"
        "\"env_valid\":1,\"imu_valid\":0,\"door_enabled\":1,"
        "\"wireless_btn_enabled\":0,\"mute\":0,\"tls\":false,"
        "\"light\":%u,\"smoke\":%u,\"body\":%u,\"door\":\"%s\"}}",
        (unsigned)now, (unsigned)g_msg_seq,
        CFG_FW_VERSION, (unsigned)g_boot_id, (unsigned)now, (unsigned)up,
        (unsigned)s->light_mv, (unsigned)s->smoke_mv, (unsigned)s->pir, door);
    rc2 = iotda_publish(g_topic_msg, payload);

    osal_printk(IOTDA_LOG "report light=%u smoke=%u body=%u door=%s (prop=%d msg=%d)\r\n",
                (unsigned)s->light_mv, (unsigned)s->smoke_mv,
                (unsigned)s->pir, door, rc1, rc2);

    return (rc1 == MQTTCLIENT_SUCCESS && rc2 == MQTTCLIENT_SUCCESS) ? 0 : -1;
}
