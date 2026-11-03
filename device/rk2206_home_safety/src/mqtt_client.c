/*
 * Copyright (c) 2024 iSoftStone Education Co., Ltd.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "mqtt_client.h"
#include "cloud_commands.h"
#include "mqtt_tls.h"
#include "iotda_config.h"
#include "wifi_backup.h"
#include "cJSON.h"
#include "MQTTClient.h"
#include "config_network.h"
#include "event_log.h"
#include "home_safety.h"
#include "los_task.h"
#include "los_tick.h"
#include "lz_hardware.h"
#include "safety_runtime.h"
#include "living.h"
#include "safety_stats.h"
#include "sntp_client.h"
#include <stdio.h>
#include <string.h>
#define MQTT_TOPIC IOTDA_REPORT_TOPIC
#define MQTT_BUF 8192
static unsigned char s_sendbuf[MQTT_BUF], s_readbuf[MQTT_BUF];
static safety_config_t s_network_cfg;
typedef struct
{
    unsigned int seq, id, boot, epoch, up;
    int code, value, ended, duration;
    safety_state_t state;
} alarm_item_t;
static alarm_item_t s_queue[16];
static unsigned int s_head, s_count, s_seq, s_dropped;
void mqtt_config_init(void)
{
    safety_config_get(&s_network_cfg);
    snprintf(s_network_cfg.broker, sizeof(s_network_cfg.broker), "%s", IOTDA_HOST);
    snprintf(s_network_cfg.client_id, sizeof(s_network_cfg.client_id), "%s", IOTDA_DEVICE_ID);
    s_network_cfg.port = IOTDA_PORT;
}
const char *mqtt_device_id(void)
{
    return IOTDA_DEVICE_ID;
}
const char *mqtt_wifi_ssid(void)
{
    return s_network_cfg.wifi_ssid;
}
const char *mqtt_wifi_pass(void)
{
    return s_network_cfg.wifi_pass;
}
/* Retained for callers of the old internal API; all publishing uses the shared snapshot. */
void mqtt_update(double t, double h, double gas, double light, bool body, unsigned int inactive,
                 int alarm, int status, int smoke)
{
    safety_state_t state;
    safety_state_get(&state);
    state.temp = t;
    state.humi = h;
    state.gas = gas;
    state.light = light;
    state.body = body;
    state.body_valid = 1;
    state.inactive = inactive / 1000;
    state.alarm = alarm;
    state.status = status;
    state.smoke = smoke;
    state.sample_up = (unsigned int)(LOS_TickCountGet() / 1000);
    safety_state_set(&state);
}
void mqtt_alarm_event(int code, int value, unsigned int id, int ended, int duration)
{
    alarm_item_t a = {0};
    a.id = id;
    a.ended = ended;
    a.duration = duration;
    a.boot = safety_boot_id();
    a.epoch = sntp_get_epoch();
    a.up = (unsigned int)(LOS_TickCountGet() / 1000);
    a.code = code;
    a.value = value;
    safety_state_get(&a.state);
    safety_lock();
    if (s_count == 16)
    {
        s_dropped++;
        safety_unlock();
        printf("[mqtt] alarm queue full, event remains in local history\r\n");
        return;
    }
    a.seq = ++s_seq;
    s_queue[(s_head + s_count) % 16] = a;
    s_count++;
    safety_unlock();
}
void mqtt_alarm(int code, int value)
{
    mqtt_alarm_event(code, value, event_log_last_id(), 0, 0);
}
void mqtt_queue_info(unsigned int *pending, unsigned int *dropped)
{
    safety_lock();
    *pending = s_count;
    *dropped = s_dropped;
    safety_unlock();
}
static int peek(alarm_item_t *a)
{
    safety_lock();
    if (!s_count)
    {
        safety_unlock();
        return 0;
    }
    *a = s_queue[s_head];
    safety_unlock();
    return 1;
}
static void pop(unsigned int seq)
{
    safety_lock();
    if (s_count && s_queue[s_head].seq == seq)
    {
        s_head = (s_head + 1) % 16;
        s_count--;
    }
    safety_unlock();
}
static bool linked(void)
{
    WifiLinkedInfo i;
    memset(&i, 0, sizeof(i));
    return GetLinkedInfo(&i) == WIFI_SUCCESS && i.connState == WIFI_CONNECTED && i.ipAddress != 0;
}
/* Only the network task calls these recovery counters. No board reboot. */
static unsigned int wifi_first, wifi_current, wifi_recoveries, cloud_failures;
static uint64_t wifi_recovery_at;
static bool wifi_recovery_started;
static void wifi_cloud_ok(void)
{
    cloud_failures = wifi_recoveries = 0;
}
static void wifi_cloud_failed(uint64_t now)
{
    if (cloud_failures < 4) cloud_failures++;
    if (cloud_failures < 4 || (wifi_recovery_started && now-wifi_recovery_at < 120000)) return;
    wifi_first = wifi_recoveries ? (wifi_current+1)%3 : wifi_current;
    printf("[wifi] cloud transport failed repeatedly: %s, start_index=%u\r\n",
           wifi_recoveries ? "try alternate hotspot" : "refresh current hotspot",wifi_first);
    wifi_recoveries++;
    cloud_failures = 0;
    wifi_recovery_started = true;
    wifi_recovery_at = now;
    SetWifiModeOff();
    LOS_Msleep(300);
}
int mqtt_connect_wifi(void)
{
    if (linked()) return 0; /* Keep a working connection; no forced switch. */
    const char *ssids[] = {s_network_cfg.wifi_ssid, WIFI_BACKUP_SSID, WIFI_EXTRA_SSID};
    const char *passwords[] = {s_network_cfg.wifi_pass, WIFI_BACKUP_PASSWORD, WIFI_EXTRA_PASSWORD};
    unsigned char mode[] = "STA";
    FlashInit();
    for (unsigned int step = 0; step < sizeof(ssids)/sizeof(ssids[0]); ++step) {
        unsigned int i = (wifi_first + step) % 3;
        bool duplicate = false;
        for (unsigned int j = 0; j < i; ++j)
            if (!strcmp(ssids[j], ssids[i])) duplicate = true;
        if (!ssids[i][0] || duplicate) continue;
        SetWifiModeOff();
        LOS_Msleep(300);
        if (VendorSet(VENDOR_ID_WIFI_MODE, mode, sizeof(mode)) != 0 ||
            VendorSet(VENDOR_ID_WIFI_ROUTE_SSID, (unsigned char *)ssids[i], strlen(ssids[i]) + 1) != 0 ||
            VendorSet(VENDOR_ID_WIFI_ROUTE_PASSWD, (unsigned char *)passwords[i], strlen(passwords[i]) + 1) != 0) {
            printf("[wifi] configure hotspot failed index=%u\r\n", i);
            continue;
        }
        printf("[wifi] trying %s (%s)\r\n", ssids[i], i ? "backup" : "primary");
        int rc = SetWifiModeOn(); /* SDK waits for connection and DHCP. */
        if (rc == WIFI_SUCCESS && linked()) {
            wifi_current = i;
            printf("[wifi] connected %s\r\n", ssids[i]);
            return 0;
        }
        printf("[wifi] hotspot unavailable %s rc=%d\r\n", ssids[i], rc);
    }
    return -1;
}
static int publish(MQTTClient *c, const char *topic, const char *payload, enum QoS qos)
{
    MQTTMessage m = {0};
    m.qos = qos;
    m.payload = (void *)payload;
    m.payloadlen = strlen(payload);
    return MQTTPublish(c, topic, &m);
}
/* Only properties defined in the cloud HomeSafety model are sent. */
static int properties_json(const safety_state_t *s, char *out, int n)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return -1;
    cJSON *services = cJSON_AddArrayToObject(root, "services");
    cJSON *service = cJSON_CreateObject();
    if (!services || !service) { cJSON_Delete(service); cJSON_Delete(root); return -1; }
    cJSON_AddItemToArray(services, service);
    int ok = cJSON_AddStringToObject(service, "service_id", IOTDA_SERVICE_ID) != NULL;
    cJSON *props = cJSON_AddObjectToObject(service, "properties");
    if (!props) { cJSON_Delete(root); return -1; }
#define PROP(name) ok = (cJSON_AddNumberToObject(props, #name, s->name) != NULL) && ok
    /* Failed environmental reads are omitted instead of reporting fabricated zeros. */
    if (s->env_valid) { PROP(temp); PROP(humi); }
    PROP(gas); PROP(light); if (s->body_valid) { PROP(body); } PROP(inactive); PROP(alarm); PROP(status);
    PROP(help); PROP(fall); PROP(gas_alarm); PROP(smoke);
    if (s->door_valid) { PROP(door); }
#undef PROP
    ok = ok && cJSON_PrintPreallocated(root, out, n, 0);
    cJSON_Delete(root);
    return ok ? 0 : -1;
}
static int event_json(const alarm_item_t *a, char *out, int n)
{
    int rc = snprintf(out, n,
        "{\"name\":\"alarm_event\",\"id\":\"%u-%u-%u-%u\",\"content\":{"
        "\"schema\":1,\"boot_id\":%u,\"event_id\":%u,\"code\":%d,\"value\":%d,"
        "\"phase\":\"%s\",\"duration\":%d,\"epoch\":%u,\"up\":%u}}",
        a->boot, a->id, a->ended, a->seq, a->boot, a->id, a->code, a->value,
        a->ended ? "end" : "start", a->duration, a->epoch, a->up);
    return rc < 0 || rc >= n ? -1 : 0;
}
static int health_message_json(char *payload, int n)
{
    safety_state_t s;
    safety_state_get(&s);
    unsigned int uptime = (unsigned int)(LOS_TickCountGet()/1000);
    safety_quality_t quality;
    safety_state_quality(&s, uptime, &quality);
    int rc = snprintf(payload, n,
        "{\"name\":\"device_health\",\"content\":{\"schema\":1,\"fw\":\"%s\","
        "\"boot_id\":%u,\"epoch\":%u,\"up\":%u,\"env_valid\":%d,\"imu_valid\":%d,"
        "\"door_enabled\":%d,\"door_valid\":%d,\"wireless_btn_enabled\":%d,\"mute\":%d,\"tls\":%s,"
        "\"sample_up\":%u,\"sample_age_s\":%d,\"data_state\":\"%s\",\"data_stale\":%s,"
        "\"env_available\":%s,\"fall_detection_available\":%s,\"time_synced\":%s}}",
        SAFETY_FW, safety_boot_id(), sntp_get_epoch(), uptime,
        s.env_valid, s.imu_valid, DOOR_SENSOR_ENABLED, s.door_valid, WIRELESS_BTN_ENABLED, s.mute,
        IOTDA_TLS_ENABLED ? "true" : "false", s.sample_up, quality.sample_age_s, quality.data_state,
        quality.stale ? "true" : "false", quality.env_available ? "true" : "false",
        quality.fall_detection_available ? "true" : "false", sntp_get_epoch() ? "true" : "false");
    if(rc<2 || rc>=n) return FAILURE;
    char living[1024];if(living_json(living,sizeof(living)))return FAILURE;
    int extra=snprintf(payload+rc-2,n-rc+2,",\"living\":%s}}",living);
    return extra<0 || extra>=n-rc+2?FAILURE:SUCCESS;
}
static int publish_health(MQTTClient *client, char *payload, int n)
{
    if (health_message_json(payload, n) != SUCCESS) return FAILURE;
    return publish(client, "$oc/devices/" IOTDA_DEVICE_ID "/sys/messages/up", payload, QOS1);
}
static int stats_message_json(char *payload, int n)
{
    const char *prefix = "{\"name\":\"activity_stats\",\"content\":";
    int offset = strlen(prefix);
    if (n <= offset + 2) return -1;
    memcpy(payload, prefix, offset);
    if (safety_stats_json(payload + offset, n - offset - 1) != 0) return -1;
    int len = strlen(payload);
    payload[len] = '}'; payload[len + 1] = 0;
    return 0;
}
static int publish_pending(MQTTClient *client, char *payload, int n)
{
    alarm_item_t a;
    if (!peek(&a)) return SUCCESS;
    /* Alarm transitions carry the sampled state. Local /events retains history IDs. */
    if (properties_json(&a.state, payload, n) != 0 ||
        publish(client, MQTT_TOPIC, payload, QOS1) != SUCCESS)
        return FAILURE;
    if (event_json(&a, payload, n) != 0 ||
        publish(client, "$oc/devices/" IOTDA_DEVICE_ID "/sys/messages/up", payload, QOS1) != SUCCESS)
        return FAILURE;
    pop(a.seq);
    return SUCCESS;
}
static void receive_command(MessageData *message)
{
    char topic[256];
    MQTTString *name = message->topicName;
    int n = name->cstring ? (int)strlen(name->cstring) : name->lenstring.len;
    if (n <= 0 || n >= (int)sizeof(topic)) return;
    memcpy(topic, name->cstring ? name->cstring : name->lenstring.data, n);
    topic[n] = 0;
    cloud_command_accept(topic, message->message->payload, message->message->payloadlen);
}
static int publish_responses(MQTTClient *client)
{
    char topic[256], body[128];
    unsigned int slot;
    int rc = cloud_response_next(topic, sizeof(topic), body, sizeof(body), &slot);
    if (rc < 0) return FAILURE;
    if (rc == 1) {
        if (publish(client, topic, body, QOS1) != SUCCESS) return FAILURE;
        cloud_response_sent(slot);
    }
    return SUCCESS;
}
static void mqtt_task(void *arg)
{
    static MQTTClient client;
    Network net;
    MQTTPacket_connectData data = MQTTPacket_connectData_initializer;
    static char payload[MQTT_BUF];
    unsigned int backoff = 1000;
    NetworkInit(&net);
    MQTTClientInit(&client, &net, 5000, s_sendbuf, sizeof(s_sendbuf), s_readbuf, sizeof(s_readbuf));
    while (1)
    {
        safety_mqtt_connected(0);
        if (!linked() && mqtt_connect_wifi() != 0)
        {
            LOS_Msleep(5000);
            continue;
        }
        if (!linked())
        {
            LOS_Msleep(1000);
            continue;
        }
#if IOTDA_TLS_ENABLED
        if (!sntp_get_epoch()) { printf("[iotda] waiting for UTC sync before TLS\r\n"); LOS_Msleep(5000); continue; }
#endif
        NetworkInit(&net);
        int rc = NetworkConnect(&net, s_network_cfg.broker, s_network_cfg.port);
        if (rc != 0)
        {
            printf("[iotda] TCP connect failed rc=%d, retry %u ms\r\n", rc, backoff);
            NetworkDisconnect(&net);
            wifi_cloud_failed(LOS_TickCountGet());
            LOS_Msleep(backoff);
            if (backoff < 16000)
                backoff *= 2;
            continue;
        }
#if IOTDA_TLS_ENABLED
        if (safety_tls_connect(&net, IOTDA_HOST) != 0) {
            NetworkDisconnect(&net); LOS_Msleep(backoff);
            if (backoff < 16000) backoff *= 2;
            continue;
        }
#endif
        /* Reuse the task's mutex rather than leaking a new one on every reconnect. */
        client.isconnected = 0;
        client.ping_outstanding = 0;
        data.clientID.cstring = IOTDA_CLIENT_ID;
        data.username.cstring = IOTDA_DEVICE_ID;
        data.password.cstring = IOTDA_PASSWORD;
        data.MQTTVersion = 4;
        data.keepAliveInterval = 60;
        data.cleansession = 1;
        rc = MQTTConnect(&client, &data);
        if (rc == 0)
            rc = MQTTSubscribe(&client, "$oc/devices/" IOTDA_DEVICE_ID "/sys/commands/#", QOS1, receive_command);
        if (rc == 0)
        {
            safety_mqtt_connected(1);
            wifi_cloud_ok();
            backoff = 1000;
            uint64_t next_report = 0, next_health = 0, next_stats = 0, health_changed_after=0;
            unsigned living_sent=~0U;
            printf("[iotda] connected host=%s service=%s port=%d tls=%d\r\n", IOTDA_HOST, IOTDA_SERVICE_ID, IOTDA_PORT, IOTDA_TLS_ENABLED);
            while (linked() && MQTTIsConnected(&client))
            {
                if (publish_responses(&client) != SUCCESS) { printf("[iotda] exit stage=command-response\r\n"); break; }
                if (publish_pending(&client, payload, sizeof(payload)) != SUCCESS) { printf("[iotda] exit stage=alarm-publish\r\n"); break; }
                safety_config_t cfg;
                safety_config_get(&cfg);
                uint64_t now = LOS_TickCountGet();
                if (now >= next_report)
                {
                    safety_state_t state;
                    safety_state_get(&state);
                    safety_quality_t quality;
                    safety_state_quality(&state, (unsigned int)(now/1000), &quality);
                    if (!quality.stale)
                    {
                    if (properties_json(&state, payload, sizeof(payload)) != 0 ||
                        publish(&client, MQTT_TOPIC, payload, QOS1) != SUCCESS) { printf("[iotda] exit stage=properties-publish\r\n"); break; }
                    printf("[iotda] properties PUBACK received\r\n");
                    safety_mqtt_report_ok((unsigned int)(LOS_TickCountGet()/1000), sntp_get_epoch());
                    next_report = now + cfg.report_ms;
                    }
                }
                if (now >= next_health || (living_sent!=living_serial() && now>=health_changed_after)) {
                    unsigned snapshot_serial=living_serial();
                    if (publish_health(&client, payload, sizeof(payload)) != SUCCESS) { printf("[iotda] exit stage=health-publish\r\n"); break; }
                    living_sent=snapshot_serial;health_changed_after=now+1000;
                    next_health = now + 60000;
                }
                if (now >= next_stats) {
                    if (stats_message_json(payload, sizeof(payload)) != 0 ||
                        publish(&client, "$oc/devices/" IOTDA_DEVICE_ID "/sys/messages/up", payload, QOS1) != SUCCESS) { printf("[iotda] exit stage=stats-publish\r\n"); break; }
                    next_stats = now + 300000;
                }
                /* Monotonic Paho timers now bound this receive/keepalive cycle even before SNTP. */
                if (MQTTYield(&client, 100) != SUCCESS) { printf("[iotda] exit stage=receive-or-keepalive\r\n"); break; }
                LOS_Msleep(50);
            }
        }
        printf("[iotda] disconnected/connect result=%d wifi_link=%d\r\n", rc, linked());
        safety_mqtt_connected(0);
#if IOTDA_TLS_ENABLED
        safety_tls_close();
#endif
        NetworkDisconnect(&net);
        LOS_Msleep(backoff);
        if (backoff < 16000)
            backoff *= 2;
    }
}
void mqtt_start(void)
{
    unsigned int id;
    TSK_INIT_PARAM_S task = {0};
    task.pfnTaskEntry = (TSK_ENTRY_FUNC)mqtt_task;
    task.uwStackSize = 16384;
    task.pcName = "mqtt task";
    /* CPU-heavy TLS must not starve the monitor (24) or watchdog (26). */
    task.usTaskPrio = 27;
    if (LOS_TaskCreate(&id, &task) != LOS_OK)
        printf("[mqtt] task creation failed\r\n");
}
