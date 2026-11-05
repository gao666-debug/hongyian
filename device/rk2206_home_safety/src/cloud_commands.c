/* SPDX-License-Identifier: Apache-2.0 */
#include "cloud_commands.h"
#include "living.h"
#include "iotda_config.h"
#include "safety_runtime.h"
#include "cJSON.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#define REQUEST_PREFIX "$oc/devices/" IOTDA_DEVICE_ID "/sys/commands/request_id="
#define RESPONSE_PREFIX "$oc/devices/" IOTDA_DEVICE_ID "/sys/commands/response/request_id="
typedef struct { char request[65]; unsigned int command, serial; int used, ready, sent, result; } receipt_t;
static receipt_t receipts[16];
static unsigned int serial;
/* Accessed only by the MQTT task; callback never publishes recursively. */
void cloud_command_accept(const char *topic, const char *body, size_t length)
{
    if (strncmp(topic, REQUEST_PREFIX, strlen(REQUEST_PREFIX))) return;
    const char *request = topic + strlen(REQUEST_PREFIX);
    size_t len = strlen(request);
    if (!len || len > 64) return;
    for (size_t i = 0; i < len; ++i)
        if (!isalnum((unsigned char)request[i]) && request[i] != '-' && request[i] != '_') return;
    int slot = -1;
    unsigned int oldest = ~0U;
    for (int i = 0; i < 16; ++i) {
        if (receipts[i].used && !strcmp(receipts[i].request, request)) {
            receipts[i].sent = 0; /* Retry the receipt without executing a duplicate command. */
            return;
        }
        if (!receipts[i].used) { slot = i; oldest = 0; }
        else if (oldest && receipts[i].sent && receipts[i].serial < oldest) {
            slot = i; oldest = receipts[i].serial;
        }
    }
    if (slot < 0) { printf("[iotda] command receipt queue full\r\n"); return; }
    receipt_t *r = &receipts[slot];
    memset(r, 0, sizeof(*r));
    strcpy(r->request, request); r->used = 1; r->serial = ++serial;
    r->ready = 1; r->result = 1;
    if (!length || length > 1024 || memchr(body, 0, length)) return;
    char text[1025]; memcpy(text, body, length); text[length] = 0;
    cJSON *j = cJSON_ParseWithOpts(text, NULL, 1);
    cJSON *service = cJSON_GetObjectItemCaseSensitive(j, "service_id");
    cJSON *name = cJSON_GetObjectItemCaseSensitive(j, "command_name");
    cJSON *paras = cJSON_GetObjectItemCaseSensitive(j, "paras");
    int code = 0;
    if (cJSON_IsObject(j) && cJSON_IsString(service) && !strcmp(service->valuestring, IOTDA_SERVICE_ID) &&
        cJSON_IsString(name) && cJSON_IsObject(paras)) {
        if (!strcmp(name->valuestring, "ack") && !cJSON_GetArraySize(paras)) code = SAFETY_CMD_ACK;
        if (!strcmp(name->valuestring, "sos") && !cJSON_GetArraySize(paras)) code = SAFETY_CMD_SOS;
        if (!strcmp(name->valuestring, "mute") && !cJSON_GetArraySize(paras)) code = SAFETY_CMD_MUTE;
    }
    if (cJSON_IsString(name) && cJSON_IsObject(paras)) {
        if (!strcmp(name->valuestring,"set_light")) code=SAFETY_CMD_LIGHT;
        if (!strcmp(name->valuestring,"set_living")) code=SAFETY_CMD_LIVING;
        if (!strcmp(name->valuestring,"confirm_medicine")) code=SAFETY_CMD_MEDICINE;
        if (!strcmp(name->valuestring,"set_medicine_plan")) code=SAFETY_CMD_MEDICINE_PLAN;
        if (!strcmp(name->valuestring,"set_medicine_state")) code=SAFETY_CMD_MEDICINE_STATE;
    }
    char payload[384];
    if (!cJSON_PrintPreallocated(paras,payload,sizeof(payload),0)) code=0;
    if (code>=SAFETY_CMD_LIVING && code<=SAFETY_CMD_LIGHT && (living_validate(code,payload) || !cJSON_IsString(service) || strcmp(service->valuestring,IOTDA_SERVICE_ID))) code=0;
    if (code) {
        int rc = safety_command_submit_payload(code, "cloud", payload, &r->command);
        if (rc == 0) r->ready = 0;
        else r->result = 2;
    }
    cJSON_Delete(j);
}
int cloud_response_next(char *topic, size_t ts, char *body, size_t bs, unsigned int *slot)
{
    for (unsigned int i = 0; i < 16; ++i) {
        receipt_t *r = &receipts[i];
        if (!r->used || r->sent) continue;
        if (!r->ready) {
            safety_command_t c;
            if (!safety_command_get(r->command, &c)) { r->ready = 1; r->result = 3; }
            else if (c.result >= 0) { r->ready = 1; r->result = c.result; }
        }
        if (!r->ready) continue;
        int t = snprintf(topic, ts, RESPONSE_PREFIX "%s", r->request);
        int b = snprintf(body, bs, "{\"result_code\":%d,\"response_name\":\"COMMAND_RESPONSE\"}", r->result);
        if (t < 0 || (size_t)t >= ts || b < 0 || (size_t)b >= bs) return -1;
        *slot = i; return 1;
    }
    return 0;
}
void cloud_response_sent(unsigned int slot) { if (slot < 16) receipts[slot].sent = 1; }
