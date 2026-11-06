#include "iotda_config.h"
/* SPDX-License-Identifier: Apache-2.0 */
#include "safety_runtime.h"
#include "cJSON.h"
#include "cmsis_os2.h"
#include "home_safety.h"
#include "kv_store.h"
#include "los_tick.h"
#include "iot_i2c.h"
#include "iot_errno.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static osMutexId_t s_lock, s_storage, s_i2c;
static safety_config_t s_cfg;
static safety_state_t s_state;
static unsigned int s_boot, s_next = 1;
static int s_mqtt;
static safety_mqtt_diag_t s_mqtt_diag;
void safety_mqtt_report_ok(unsigned int uptime, unsigned int epoch)
{
    safety_lock();
    s_mqtt_diag.report_received = 1;
    s_mqtt_diag.report_up = uptime;
    s_mqtt_diag.report_epoch = epoch;
    safety_unlock();
}
void safety_mqtt_diag_get(safety_mqtt_diag_t *out)
{
    safety_lock();
    *out = s_mqtt_diag;
    out->connected = s_mqtt;
    safety_unlock();
}
static int s_i2c_ready;
int safety_i2c_prepare_locked(void)
{
    if (s_i2c_ready) return 0;
    unsigned int rc = IoTI2cInit(EI2C0_M2, EI2C_FRE_100K);
    if (rc != IOT_SUCCESS) {
        printf("[i2c] shared bus init failed rc=%u\r\n", rc);
        return -1;
    }
    s_i2c_ready = 1;
    printf("[i2c] shared bus ready\r\n");
    return 0;
}
static unsigned int s_i2c_failures;
static uint64_t s_i2c_recovered_ms;
static bool s_i2c_recovered;
void safety_i2c_result_locked(bool ok)
{
    if(ok){s_i2c_failures=0;return;}
    if(s_i2c_failures<3)s_i2c_failures++;
    uint64_t now=LOS_TickCountGet();
    if(s_i2c_failures<3 || (s_i2c_recovered && now-s_i2c_recovered_ms<10000))return;
    /* Only recover after the SDK transfer returned and released its own lock. */
    unsigned int rc=IoTI2cDeinit(EI2C0_M2);
    printf("[i2c] recovery after 3 failed transfers, deinit rc=%u\r\n",rc);
    if(rc==IOT_SUCCESS)s_i2c_ready=0;
    s_i2c_recovered=true;s_i2c_recovered_ms=now;s_i2c_failures=0;
}
static unsigned int s_cfg_slot;
static safety_command_t s_pending[8], s_results[16];
static unsigned int s_head, s_count, s_result_head;
void safety_lock(void)
{
    osMutexAcquire(s_lock, osWaitForever);
}
void safety_unlock(void)
{
    osMutexRelease(s_lock);
}
void safety_storage_lock(void)
{
    osMutexAcquire(s_storage, osWaitForever);
}
void safety_storage_unlock(void)
{
    osMutexRelease(s_storage);
}
static int cfg_valid(const safety_config_t *c)
{
    return c->wifi_ssid[0] && strlen(c->wifi_ssid) <= 32 && strlen(c->wifi_pass) <= 64 &&
           c->broker[0] && c->client_id[0] && c->port > 0 && c->port <= 65535 &&
           c->report_ms >= 1000 && c->report_ms <= 3600000 && c->noact_ms >= 10000 &&
           c->noact_ms <= 86400000 && c->mute_ms >= 1000 && c->mute_ms <= 600000 &&
           isfinite(c->temp_low) && isfinite(c->temp_high) && c->temp_low >= -40 &&
           c->temp_high <= 125 && c->temp_low < c->temp_high && isfinite(c->humi_low) &&
           isfinite(c->humi_high) && c->humi_low >= 0 && c->humi_high <= 100 &&
           c->humi_low < c->humi_high && isfinite(c->gas_limit) && c->gas_limit >= 1 &&
           c->gas_limit <= 100000;
}
int safety_runtime_init(void)
{
    char v[128];
    safety_config_t c = {"GB",
                         "75359461aaa",
                         IOTDA_HOST,
                         IOTDA_DEVICE_ID,
                         1883,
                         15000,
                         60000,
                         30000,
                         10,
                         35,
                         20,
                         80,
                         800,
                         true};
    s_lock = osMutexNew(NULL);
    s_storage = osMutexNew(NULL);
    s_i2c = osMutexNew(NULL);
    if (!s_lock || !s_storage || !s_i2c)
    {
        printf("[safety] mutex creation failed\r\n");
        return -1;
    }
    if (UtilsGetValue("boot_id", v, sizeof(v)) >= 0)
        s_boot = (unsigned int)strtoul(v, NULL, 10);
    s_boot++;
    if (!s_boot)
        s_boot = 1;
    snprintf(v, sizeof(v), "%u", s_boot);
    UtilsSetValue("boot_id", v);
    if (UtilsGetValue("cfg_active", v, sizeof(v)) >= 0)
        s_cfg_slot = (unsigned int)strtoul(v, NULL, 10) % 2;
    char key[20];
    snprintf(key, sizeof(key), "cfg_wifi%u", s_cfg_slot);
    if (UtilsGetValue(key, v, sizeof(v)) >= 0)
    {
        cJSON *j = cJSON_Parse(v);
        cJSON *ssid = cJSON_GetObjectItemCaseSensitive(j, "s"),
              *pass = cJSON_GetObjectItemCaseSensitive(j, "p");
        if (cJSON_IsString(ssid) && cJSON_IsString(pass) &&
            strlen(ssid->valuestring) < sizeof(c.wifi_ssid) &&
            strlen(pass->valuestring) < sizeof(c.wifi_pass))
        {
            strcpy(c.wifi_ssid, ssid->valuestring);
            strcpy(c.wifi_pass, pass->valuestring);
        }
        cJSON_Delete(j);
    }
    snprintf(key, sizeof(key), "cfg_host%u", s_cfg_slot);
    if (UtilsGetValue(key, v, sizeof(v)) >= 0 && strlen(v) < sizeof(c.broker))
        strcpy(c.broker, v);
    snprintf(key, sizeof(key), "cfg_cid%u", s_cfg_slot);
    if (UtilsGetValue(key, v, sizeof(v)) >= 0 && strlen(v) < sizeof(c.client_id))
        strcpy(c.client_id, v);
    snprintf(key, sizeof(key), "cfg_num%u", s_cfg_slot);
    if (UtilsGetValue(key, v, sizeof(v)) >= 0)
    {
        unsigned int demo = 1;
        double tl, th, hl, hh, gl;
        unsigned int po, r, no, mu;
        if (sscanf(v, "%u %u %u %u %lf %lf %lf %lf %lf %u", &po, &r, &no, &mu, &tl, &th, &hl, &hh,
                   &gl, &demo) == 10)
        {
            c.port = po;
            c.report_ms = r;
            c.noact_ms = no;
            c.mute_ms = mu;
            c.temp_low = tl;
            c.temp_high = th;
            c.humi_low = hl;
            c.humi_high = hh;
            c.gas_limit = gl;
            c.demo = demo != 0;
        }
    }
    if (!cfg_valid(&c))
    {
        safety_config_t d = {"GB",
                             "75359461aaa",
                             IOTDA_HOST,
                             IOTDA_DEVICE_ID,
                             1883,
                             15000,
                             60000,
                             30000,
                             10,
                             35,
                             20,
                             80,
                             800,
                             true};
        c = d;
    }
    /* Cloud identity is paired with build credentials, never a saved public broker. */
    snprintf(c.broker, sizeof(c.broker), "%s", IOTDA_HOST);
    snprintf(c.client_id, sizeof(c.client_id), "%s", IOTDA_DEVICE_ID);
    c.port = IOTDA_PORT;
    s_cfg = c;
    printf("[safety] fw=%s boot=%u profile=%s\r\n", SAFETY_FW, s_boot, c.demo ? "demo" : "normal");
    return 0;
}
unsigned int safety_boot_id(void)
{
    return s_boot;
}
void safety_config_get(safety_config_t *c)
{
    safety_lock();
    *c = s_cfg;
    safety_unlock();
}
int safety_config_set(const safety_config_t *c)
{
    char numeric[128], wifi[128], key[20], active[4];
    int rc = 0;
    unsigned int next;
    if (strcmp(c->broker, IOTDA_HOST) || strcmp(c->client_id, IOTDA_DEVICE_ID) ||
        c->port != IOTDA_PORT)
        return -1;
    if (!cfg_valid(c))
        return -1;
    cJSON *j = cJSON_CreateObject();
    if (!j)
        return -2;
    cJSON_AddStringToObject(j, "s", c->wifi_ssid);
    cJSON_AddStringToObject(j, "p", c->wifi_pass);
    int ok = cJSON_PrintPreallocated(j, wifi, sizeof(wifi), 0);
    cJSON_Delete(j);
    if (!ok)
        return -1;
    int len = snprintf(numeric, sizeof(numeric), "%u %u %u %u %.2f %.2f %.2f %.2f %.2f %u", c->port,
                       c->report_ms, c->noact_ms, c->mute_ms, c->temp_low, c->temp_high,
                       c->humi_low, c->humi_high, c->gas_limit, c->demo ? 1 : 0);
    if (len < 0 || len >= (int)sizeof(numeric) - 1)
        return -1;
    safety_storage_lock();
    next = 1 - s_cfg_slot;
    snprintf(key, sizeof(key), "cfg_wifi%u", next);
    if (UtilsSetValue(key, wifi) < 0)
        rc = -2;
    snprintf(key, sizeof(key), "cfg_host%u", next);
    if (!rc && UtilsSetValue(key, c->broker) < 0)
        rc = -2;
    snprintf(key, sizeof(key), "cfg_cid%u", next);
    if (!rc && UtilsSetValue(key, c->client_id) < 0)
        rc = -2;
    snprintf(key, sizeof(key), "cfg_num%u", next);
    if (!rc && UtilsSetValue(key, numeric) < 0)
        rc = -2;
    snprintf(active, sizeof(active), "%u", next);
    if (!rc && UtilsSetValue("cfg_active", active) < 0)
        rc = -2;
    if (!rc)
    {
        safety_lock();
        s_cfg = *c;
        s_cfg_slot = next;
        safety_unlock();
    }
    safety_storage_unlock();
    return rc;
}
int safety_config_json(char *out, int n)
{
    safety_config_t c;
    cJSON *j = cJSON_CreateObject();
    int ok;
    safety_config_get(&c);
    if (!j)
        return -1;
    cJSON_AddStringToObject(j, "wifi_ssid", c.wifi_ssid);
    cJSON_AddStringToObject(j, "broker", c.broker);
    cJSON_AddStringToObject(j, "client_id", c.client_id);
    cJSON_AddNumberToObject(j, "port", c.port);
    cJSON_AddNumberToObject(j, "report_ms", c.report_ms);
    cJSON_AddNumberToObject(j, "noact_ms", c.noact_ms);
    cJSON_AddNumberToObject(j, "mute_ms", c.mute_ms);
    cJSON_AddNumberToObject(j, "temp_low", c.temp_low);
    cJSON_AddNumberToObject(j, "temp_high", c.temp_high);
    cJSON_AddNumberToObject(j, "humi_low", c.humi_low);
    cJSON_AddNumberToObject(j, "humi_high", c.humi_high);
    cJSON_AddNumberToObject(j, "gas_limit", c.gas_limit);
    cJSON_AddBoolToObject(j, "demo", c.demo);
    cJSON_AddBoolToObject(j, "wifi_pass_set", c.wifi_pass[0] != 0);
    ok = cJSON_PrintPreallocated(j, out, n, 0);
    cJSON_Delete(j);
    return ok ? 0 : -1;
}
int safety_config_apply_json(const char *text)
{
    safety_config_t c;
    const char *end;
    cJSON *j = cJSON_ParseWithOpts(text, &end, 1), *x;
    int rc = -1;
    if (!j || !cJSON_IsObject(j))
    {
        cJSON_Delete(j);
        return -1;
    }
    safety_config_get(&c);
    const char *names[] = {"wifi_ssid", "wifi_pass", "broker",    "client_id", "port",
                           "report_ms", "noact_ms",  "mute_ms",   "temp_low",  "temp_high",
                           "humi_low",  "humi_high", "gas_limit", "demo"};
    for (cJSON *item = j->child; item; item = item->next)
    {
        bool known = false;
        for (unsigned int i = 0; i < sizeof(names) / sizeof(names[0]); i++)
            if (!strcmp(item->string, names[i]))
                known = true;
        if (!known)
            goto done;
        for (cJSON *prev = j->child; prev != item; prev = prev->next)
            if (!strcmp(prev->string, item->string))
                goto done;
    }
#define STR(k, f)                                                                                  \
    x = cJSON_GetObjectItemCaseSensitive(j, k);                                                    \
    if (x)                                                                                         \
    {                                                                                              \
        if (!cJSON_IsString(x) || strlen(x->valuestring) >= sizeof(c.f))                           \
            goto done;                                                                             \
        strcpy(c.f, x->valuestring);                                                               \
    }
#define UINT(k, f)                                                                                 \
    x = cJSON_GetObjectItemCaseSensitive(j, k);                                                    \
    if (x)                                                                                         \
    {                                                                                              \
        if (!cJSON_IsNumber(x) || !isfinite(x->valuedouble) || x->valuedouble < 0 ||               \
            x->valuedouble > 86400000 || floor(x->valuedouble) != x->valuedouble)                  \
            goto done;                                                                             \
        c.f = (unsigned int)x->valuedouble;                                                        \
    }
#define DBL(k, f)                                                                                  \
    x = cJSON_GetObjectItemCaseSensitive(j, k);                                                    \
    if (x)                                                                                         \
    {                                                                                              \
        if (!cJSON_IsNumber(x))                                                                    \
            goto done;                                                                             \
        c.f = x->valuedouble;                                                                      \
    }
    STR("wifi_ssid", wifi_ssid);
    STR("wifi_pass", wifi_pass);
    STR("broker", broker);
    STR("client_id", client_id);
    UINT("port", port);
    UINT("report_ms", report_ms);
    UINT("noact_ms", noact_ms);
    UINT("mute_ms", mute_ms);
    DBL("temp_low", temp_low);
    DBL("temp_high", temp_high);
    DBL("humi_low", humi_low);
    DBL("humi_high", humi_high);
    DBL("gas_limit", gas_limit);
    x = cJSON_GetObjectItemCaseSensitive(j, "demo");
    if (x)
    {
        if (!cJSON_IsBool(x))
            goto done;
        c.demo = cJSON_IsTrue(x);
        if (!cJSON_GetObjectItemCaseSensitive(j, "noact_ms"))
            c.noact_ms = c.demo ? 60000 : 1800000;
    }
    rc = safety_config_set(&c);
done:
    cJSON_Delete(j);
    return rc;
}
int safety_command_submit(int code, const char *source, unsigned int *id)
{ return safety_command_submit_payload(code, source, "{}", id); }
int safety_command_submit_payload(int code, const char *source, const char *payload, unsigned int *id)
{
    safety_command_t *c;
    if (code < SAFETY_CMD_ACK || code > SAFETY_CMD_CONTACT || !payload || strlen(payload) >= 384)
        return -1;
    safety_lock();
    if (s_count == 8)
    {
        safety_unlock();
        return -2;
    }
    c = &s_pending[(s_head + s_count) % 8];
    memset(c, 0, sizeof(*c));
    c->id = s_next++;
    if (!s_next)
        s_next = 1;
    snprintf(c->payload, sizeof(c->payload), "%s", payload);
    c->code = code;
    c->result = -1;
    snprintf(c->source, sizeof(c->source), "%s", source);
    *id = c->id;
    s_count++;
    safety_unlock();
    return 0;
}
int safety_command_take(safety_command_t *c)
{
    safety_lock();
    if (!s_count)
    {
        safety_unlock();
        return 0;
    }
    *c = s_pending[s_head];
    s_head = (s_head + 1) % 8;
    s_count--;
    s_results[s_result_head] = *c;
    s_result_head = (s_result_head + 1) % 16;
    safety_unlock();
    return 1;
}
void safety_command_done(const safety_command_t *c, int result)
{
    safety_lock();
    for (int i = 0; i < 16; i++)
        if (s_results[i].id == c->id)
        {
            s_results[i].result = result;
            break;
        }
    safety_unlock();
}
int safety_command_get(unsigned int id, safety_command_t *c)
{
    unsigned int i;
    safety_lock();
    for (i = 0; i < s_count; i++)
        if (s_pending[(s_head + i) % 8].id == id)
        {
            *c = s_pending[(s_head + i) % 8];
            safety_unlock();
            return 1;
        }
    for (i = 0; i < 16; i++)
        if (id && s_results[i].id == id)
        {
            *c = s_results[i];
            safety_unlock();
            return 1;
        }
    safety_unlock();
    return 0;
}
void safety_state_set(const safety_state_t *s)
{
    safety_lock();
    s_state = *s;
    s_state.sample_received = 1;
    safety_unlock();
}
void safety_state_get(safety_state_t *s)
{
    safety_lock();
    *s = s_state;
    safety_unlock();
}
void safety_health_set(int env, int imu, int mute)
{
    safety_lock();
    s_state.env_valid = env;
    s_state.imu_valid = imu;
    s_state.mute = mute;
    safety_unlock();
}
void safety_state_quality(const safety_state_t *s, unsigned int uptime, safety_quality_t *out)
{
    int valid_time = s->sample_received && uptime >= s->sample_up;
    unsigned int age = valid_time ? uptime - s->sample_up : 0;
    out->sample_age_s = valid_time && age <= 2147483647U ? (int)age : -1;
    out->stale = !valid_time || age > 10;
    out->data_state = !s->sample_received ? "waiting" : out->stale ? "stale" : "ready";
    out->env_available = !out->stale && s->env_valid;
    out->fall_detection_available = 0; /* Fixed gateway has no human fall sensor. */
}
void safety_mqtt_connected(int c)
{
    safety_lock();
    s_mqtt = c;
    safety_unlock();
}
int safety_mqtt_is_connected(void)
{
    int c;
    safety_lock();
    c = s_mqtt;
    safety_unlock();
    return c;
}

static volatile unsigned int s_i2c_lock_timeouts;
unsigned int safety_i2c_lock_timeouts(void) { return s_i2c_lock_timeouts; }
int safety_i2c_try_lock(unsigned int ticks)
{
    if(osMutexAcquire(s_i2c,ticks)==0)return 1;
    s_i2c_lock_timeouts++;
    return 0;
}
void safety_i2c_lock(void)
{
    osMutexAcquire(s_i2c, osWaitForever);
}
void safety_i2c_unlock(void)
{
    osMutexRelease(s_i2c);
}

