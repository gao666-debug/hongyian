#include "local_page.h"
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

#include "net_server.h"
#include "cJSON.h"
#include "config_network.h"
#include "event_log.h"
#include "home_safety.h"
#include "los_task.h"
#include "los_tick.h"
#include "lwip/ip_addr.h"
#include "lwip/netif.h"
#include "lwip/sockets.h"
#include "lz_hardware.h"
#include "mqtt_client.h"
#include "iotda_config.h"
#include "safety_runtime.h"
#include "living.h"
#include "care_policy.h"
#include "local_access.h"
#include "safety_stats.h"
#include "sntp_client.h"
#include <netinet/in.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#define NET_AP_SSID "HomeSafety"
#define NET_AP_PASS "12345678"
#define NET_HTTP_PORT 8080
#define NET_MAX_BUF 8192
static const unsigned char NET_AP_IP[4] = {192, 168, 2, 1}, NET_AP_GW[4] = {192, 168, 2, 1},
                           NET_AP_MASK[4] = {255, 255, 255, 0};
void net_server_on_activity(void)
{
    safety_stats_activity();
}
void net_server_on_alarm(int code)
{
    safety_stats_alarm(code);
}
/* Legacy wrapper; the monitor uses the structured command queue directly. */
int net_server_take_cmd(void)
{
    safety_command_t c;
    if (!safety_command_take(&c))
        return NET_CMD_NONE;
    return c.code;
}
void net_server_update(double temp, double humi, double gas, bool body, unsigned int inactive,
                       int alarm, int status, int help, int fall, int gas_alarm, int door,
                       double light, int smoke)
{
    safety_state_t s;
    safety_state_get(&s);
    s.temp = temp;
    s.humi = humi;
    s.gas = gas;
    s.body = body;
    s.inactive = inactive / 1000;
    s.alarm = alarm;
    s.status = status;
    s.help = help;
    s.fall = fall;
    s.gas_alarm = gas_alarm;
    s.door = door;
    s.light = light;
    s.smoke = smoke;
    s.sample_up = (unsigned int)(LOS_TickCountGet() / 1000);
    safety_state_set(&s);
}
static void wifi_sta_info(char *ip, int n, int *rssi, int *linked)
{
    WifiLinkedInfo info;
    memset(&info, 0, sizeof(info));
    ip[0] = 0;
    *rssi = 0;
    *linked = 0;
    if (net_get_mode() == NET_MODE_AP) { snprintf(ip,n,"192.168.2.1"); return; }
    if (GetLinkedInfo(&info) == WIFI_SUCCESS && info.connState == WIFI_CONNECTED && info.ipAddress)
    {
        ip4_addr_t a;
        a.addr = info.ipAddress;
        ip4addr_ntoa_r(&a, ip, n);
        *rssi = info.rssi;
        *linked = 1;
    }
}
void net_server_local_ip(char *ip, int size)
{
    int rssi, linked;
    if (!ip || size < 16) return;
    wifi_sta_info(ip, size, &rssi, &linked);
}
int safety_status_json(char *out, int n)
{
    safety_state_t s;
    char ip[16];
    int rssi, linked, ok;
    cJSON *j = cJSON_CreateObject();
    if (!j)
        return -1;
    safety_state_get(&s);
    wifi_sta_info(ip, sizeof(ip), &rssi, &linked);
    safety_quality_t quality;
    safety_state_quality(&s, (unsigned int)(LOS_TickCountGet() / 1000), &quality);
    safety_config_t cfg;
    safety_config_get(&cfg);
    cJSON_AddStringToObject(j, "device_id", mqtt_device_id());
    cJSON_AddBoolToObject(j, "demo", cfg.demo);
    cJSON_AddNumberToObject(j, "report_ms", cfg.report_ms);
    cJSON_AddNumberToObject(j, "noact_ms", cfg.noact_ms);
    cJSON_AddStringToObject(j, "fw", SAFETY_FW);
    cJSON_AddStringToObject(j, "mode", net_get_mode() == NET_MODE_AP ? "AP" : "STA");
    cJSON_AddStringToObject(j, "dev", "TX-SMART-R");
    cJSON_AddNumberToObject(j, "uptime", LOS_TickCountGet() / 1000);
    cJSON_AddStringToObject(j, "power", "usb");
    cJSON_AddNumberToObject(j, "battery", 100);
    cJSON_AddBoolToObject(j, "battery_simulated", 1);
#define NUM(k, f) cJSON_AddNumberToObject(j, k, s.f)
    NUM("temp", temp);
    NUM("humi", humi);
    NUM("gas", gas);
    NUM("light", light);
    NUM("body", body);
    NUM("inactive", inactive);
    NUM("alarm", alarm);
    NUM("status", status);
    NUM("help", help);
    NUM("fall", fall);
    NUM("gas_alarm", gas_alarm);
    NUM("door", door);
    NUM("door_valid", door_valid);
    NUM("smoke", smoke);
    NUM("env_valid", env_valid);
    NUM("env_cached", env_cached);
    NUM("env_age_s", env_age_s);
    NUM("body_valid", body_valid);
    NUM("imu_valid", imu_valid);
    NUM("mute", mute);
    NUM("sample_up", sample_up);
    cJSON_AddNumberToObject(j, "sample_age_s", quality.sample_age_s);
    cJSON_AddStringToObject(j, "data_state", quality.data_state);
    cJSON_AddBoolToObject(j, "data_stale", quality.stale);
    cJSON_AddBoolToObject(j, "env_available", quality.env_available);
    cJSON_AddBoolToObject(j, "fall_detection_available", quality.fall_detection_available);
    cJSON_AddBoolToObject(j, "time_synced", sntp_get_epoch() != 0);
    cJSON_AddBoolToObject(j, "tls", IOTDA_TLS_ENABLED);
    cJSON_AddNumberToObject(j, "epoch", sntp_get_epoch());
    cJSON_AddNumberToObject(j, "ap", IsHotspotActive() == WIFI_HOTSPOT_ACTIVE);
    cJSON_AddNumberToObject(j, "sta", linked);
    cJSON_AddStringToObject(j, "ip", ip);
    cJSON_AddNumberToObject(j, "rssi", rssi);
    cJSON_AddNumberToObject(j, "boot_id", safety_boot_id());
    cJSON_AddStringToObject(j,"device_id",IOTDA_DEVICE_ID);
    cJSON_AddNumberToObject(j, "event_revision", event_log_last_id());
    cJSON_AddNumberToObject(j, "mqtt", safety_mqtt_is_connected());
    safety_mqtt_diag_t cloud;
    safety_mqtt_diag_get(&cloud);
    unsigned int uptime = (unsigned int)(LOS_TickCountGet()/1000);
    cJSON_AddBoolToObject(j, "mqtt_report_received", cloud.report_received);
    cJSON_AddNumberToObject(j, "mqtt_last_report_up", cloud.report_up);
    cJSON_AddNumberToObject(j, "mqtt_last_report_epoch", cloud.report_epoch);
    cJSON_AddNumberToObject(j, "mqtt_report_age_s",
        cloud.report_received && uptime >= cloud.report_up ? (double)(uptime-cloud.report_up) : -1);
    cJSON_AddBoolToObject(j, "door_enabled", DOOR_SENSOR_ENABLED);
    cJSON_AddBoolToObject(j, "wireless_btn_enabled", WIRELESS_BTN_ENABLED);
    unsigned int pending, dropped;
    mqtt_queue_info(&pending, &dropped);
    cJSON_AddNumberToObject(j, "mqtt_pending", pending);
    cJSON_AddNumberToObject(j, "mqtt_dropped", dropped);
    char life[1024];
    if(!living_json(life,sizeof(life)))cJSON_AddItemToObject(j,"living",cJSON_Parse(life));
    ok = cJSON_PrintPreallocated(j, out, n, 0);
    cJSON_Delete(j);
    return ok ? 0 : -1;
}
static int build_events_json(char *out, int n)
{
    cJSON *j = cJSON_CreateObject(), *a = cJSON_CreateArray();
    int i, ok;
    if (!j || !a)
    {
        cJSON_Delete(j);
        cJSON_Delete(a);
        return -1;
    }
    safety_event_t entries[16];
    int count = event_log_snapshot(entries);
    for (i = 0; i < count; i++)
    {
        safety_event_t e = entries[i];
        cJSON *x = cJSON_CreateObject();
        if (!x)
        {
            cJSON_Delete(j);
            cJSON_Delete(a);
            return -1;
        }
        cJSON_AddNumberToObject(x, "id", e.id);
        cJSON_AddNumberToObject(x, "boot_id", e.boot);
        cJSON_AddNumberToObject(x, "up", e.up);
        cJSON_AddNumberToObject(x, "epoch", e.epoch);
        cJSON_AddStringToObject(x, "tag", e.tag);
        cJSON_AddStringToObject(x, "name", event_log_tag_cn(e.tag));
        cJSON_AddNumberToObject(x, "val", e.val);
        cJSON_AddNumberToObject(x, "dur", e.dur);
        cJSON_AddItemToArray(a, x);
    }
    cJSON_AddItemToObject(j, "events", a);
    cJSON_AddNumberToObject(j, "last_id", event_log_last_id());
    ok = cJSON_PrintPreallocated(j, out, n, 0);
    cJSON_Delete(j);
    return ok ? 0 : -1;
}
static int build_info_json(char *out, int n)
{
    char ip[16];
    int rssi, sta;
    wifi_sta_info(ip, sizeof(ip), &rssi, &sta);
    return snprintf(
               out, n,
               "{\"dev\":\"TX-SMART-R\",\"fw\":\"%s\",\"power\":\"usb\",\"battery\":100,\"battery_"
               "simulated\":true,\"ap_ssid\":\"HomeSafety\",\"ip\":\"%s\",\"port\":8080,\"mode\":"
               "\"%s\",\"uptime\":%u,\"epoch\":%u,\"sta\":%d,\"boot_id\":%u,\"mqtt\":%d}",
               SAFETY_FW, net_get_mode() == NET_MODE_AP ? "192.168.2.1" : ip,
               net_get_mode() == NET_MODE_AP ? "AP" : "STA",
               (unsigned int)(LOS_TickCountGet() / 1000), sntp_get_epoch(), sta, safety_boot_id(),
               safety_mqtt_is_connected()) < n
               ? 0
               : -1;
}
static int send_all(int fd, const char *data, int n)
{
    int sent = 0;
    uint64_t start = LOS_TickCountGet();
    while (sent < n)
    {
        if (LOS_TickCountGet() - start >= 5000)
            return -1;
        int r = send(fd, data + sent, n - sent, 0);
        if (r <= 0)
        {
            perror("[http] send");
            printf("[http] send failed fd=%d rc=%d\n", fd, r);
            return -1;
        }
        sent += r;
    }
    return 0;
}
static void http_reply(int fd, int code, const char *body)
{
    const char *reason = code == 200   ? "OK"
                         : code == 202 ? "Accepted"
                         : code == 400 ? "Bad Request"
                         : code == 404 ? "Not Found"
                         : code == 405 ? "Method Not Allowed"
                         : code == 413 ? "Payload Too Large"
                         : code == 503 ? "Service Unavailable"
                                       : "Internal Server Error";
    char hdr[256];
    int len = snprintf(hdr, sizeof(hdr),
                       "HTTP/1.1 %d %s\r\nContent-Type: application/json; "
                       "charset=utf-8\r\nCache-Control: no-store\r\nAccess-Control-Allow-Origin: "
                       "*\r\nContent-Length: %u\r\nConnection: close\r\n\r\n",
                       code, reason, (unsigned int)strlen(body));
    if (send_all(fd, hdr, len) == 0)
        send_all(fd, body, strlen(body));
}
static void http_handle_client(int fd)
{
    static char rx[2048], body[NET_MAX_BUF];
    char method[8], url[160], version[16];
    char *end = NULL, *payload = NULL;
    int used = 0, length = 0, status = 200, rc = 0;
    bool have_length = false, local_control = false, have_token = false;
    char access_token[65] = {0};
    uint64_t start = LOS_TickCountGet();
    struct timeval tv = {0, 300000}; /* Idle browser preconnections must not block control requests for seconds. */
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    rx[0] = 0;
    while (!end && used < (int)sizeof(rx) - 1 && LOS_TickCountGet() - start < 5000)
    {
        int r = recv(fd, rx + used, sizeof(rx) - 1 - used, 0);
        if (r <= 0)
        {
            http_reply(fd, 400, "{\"ok\":false,\"error\":\"incomplete_request\"}");
            return;
        }
        if (!used) {
            tv.tv_sec=2;tv.tv_usec=0; /* Allow fragmented real requests after the first byte. */
            setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof(tv));
        }
        used += r;
        rx[used] = 0;
        end = strstr(rx, "\r\n\r\n");
    }
    if (!end)
    {
        http_reply(fd, 413, "{\"ok\":false,\"error\":\"request_too_large\"}");
        return;
    }
    if (sscanf(rx, "%7s %159s %15s", method, url, version) != 3 ||
        (strcmp(version, "HTTP/1.1") && strcmp(version, "HTTP/1.0")))
    {
        http_reply(fd, 400, "{\"ok\":false}");
        return;
    }
    {
        char *line = strstr(rx, "\r\n");
        while (line && line < end)
        {
            char *next = strstr(line + 2, "\r\n");
            if (!next)
                break;
            if (strncasecmp(line + 2, "Content-Length:", 15) == 0)
            {
                char *stop;
                long v = strtol(line + 17, &stop, 10);
                if (have_length || stop != next || v < 0 || v > 1024)
                {
                    http_reply(fd, 400, "{\"ok\":false,\"error\":\"invalid_length\"}");
                    return;
                }
                length = v;
                have_length = true;
            }
            if (strncasecmp(line + 2, "Transfer-Encoding:", 18) == 0)
            {
                http_reply(fd, 400, "{\"ok\":false,\"error\":\"transfer_encoding_unsupported\"}");
                return;
            }
            if (strncasecmp(line + 2,"X-Care-Token:",13)==0) {
                const char *value=line+15;while(value<next&&*value==' ')value++;
                if(have_token||next-value!=64){http_reply(fd,400,"{\"error\":\"invalid_pair_header\"}");return;}
                memcpy(access_token,value,64);access_token[64]=0;have_token=true;
            }
            /* Cross-origin scripts cannot send this header without preflight.
             * This is a browser guard, not account authentication. */
            if (next - (line + 2) == 15 && !strncasecmp(line + 2, "X-Care-Local: 1", 15))
                local_control = true;
            line = next;
        }
    }
    payload = end + 4;
    if ((payload - rx) + length >= (int)sizeof(rx))
    {
        http_reply(fd, 413, "{\"ok\":false}");
        return;
    }
    while (used < (payload - rx) + length && LOS_TickCountGet() - start < 5000)
    {
        int r = recv(fd, rx + used, sizeof(rx) - 1 - used, 0);
        if (r <= 0)
            break;
        used += r;
        rx[used] = 0;
    }
    if (used < (payload - rx) + length)
    {
        http_reply(fd, 400, "{\"ok\":false,\"error\":\"incomplete_body\"}");
        return;
    }
    payload[length] = 0;
    char *query = strchr(url, '?');
    if (query)
        *query++ = 0;
    if (strcmp(method, "GET") && !(strcmp(method, "POST") == 0 &&
        (!strcmp(url, "/config") || !strcmp(url, "/ack") || !strcmp(url, "/mute") ||
         !strcmp(url, "/pair/request") || !strcmp(url, "/pair/cancel") || !strcmp(url, "/messages/send") || !strcmp(url, "/messages/reply") || !strcmp(url, "/messages/seen") || !strcmp(url, "/care-pause") || !strcmp(url, "/care-policy") || !strcmp(url, "/living") || !strcmp(url, "/medicine") || !strcmp(url, "/light"))))
    {
        http_reply(fd, 405, "{\"ok\":false}");
        return;
    }
    if (!strcmp(url, "/") || !strcmp(url, "/index.html")) {
        char header[192];
        int len=snprintf(header,sizeof(header),"HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nCache-Control: no-store\r\nContent-Length: %u\r\nConnection: close\r\n\r\n",(unsigned)(sizeof(LOCAL_PAGE)-1));
        if(send_all(fd,header,len)==0)send_all(fd,LOCAL_PAGE,sizeof(LOCAL_PAGE)-1);
        return;
    }
    uint64_t access_now = LOS_TickCountGet();
    if (!strcmp(url,"/pair/status")) {
        if(strcmp(method,"GET")){http_reply(fd,405,"{\"error\":\"get_required\"}");return;}
        snprintf(body,sizeof(body),"{\"paired\":%s,\"boot_id\":%u,\"ttl_seconds\":1800}",local_pair_identity(access_token,access_now)?"true":"false",safety_boot_id());
        http_reply(fd,200,body);return;
    }
    if (!strcmp(url,"/pair/request")) {
        if(strcmp(method,"POST")||!have_length||!local_control){http_reply(fd,403,"{\"error\":\"pair_request_required\"}");return;}
        cJSON *j=cJSON_ParseWithOpts(payload,NULL,1),*token=cJSON_GetObjectItemCaseSensitive(j,"token");unsigned id=0;
        int result=!cJSON_IsObject(j)||cJSON_GetArraySize(j)!=1||!cJSON_IsString(token)?1:local_pair_request(token->valuestring,access_now,&id);
        cJSON_Delete(j);
        if(result){http_reply(fd,result==1?400:409,"{\"error\":\"pair_request_rejected\"}");return;}
        snprintf(body,sizeof(body),"{\"request_id\":%u,\"expires_in\":60,\"boot_id\":%u}",id,safety_boot_id());http_reply(fd,202,body);return;
    }
    if (!strcmp(url,"/pair/cancel")) {
        if(strcmp(method,"POST")||!local_control){http_reply(fd,403,"{\"error\":\"post_required\"}");return;}
        local_pair_cancel(access_token);http_reply(fd,200,"{\"ok\":true}");return;
    }
    /* Mutations never use GET: prefetched links must not ACK or mute alarms. */
    if(!strcmp(method,"GET")&&(!strcmp(url,"/ack")||!strcmp(url,"/mute")||!strcmp(url,"/messages/send")||!strcmp(url,"/messages/reply")||!strcmp(url,"/messages/seen")||!strcmp(url,"/care-pause"))){http_reply(fd,405,"{\"error\":\"post_required\"}");return;}
    unsigned client_identity=local_pair_identity(access_token,access_now);
    if(strcmp(url,"/status") && !client_identity){http_reply(fd,401,"{\"error\":\"physical_pairing_required\"}");return;}
    if(!strcmp(method,"POST")&&!local_control){http_reply(fd,403,"{\"error\":\"local_header_required\"}");return;}
    if(!strcmp(url,"/messages")){rc=local_messages_json(body,sizeof(body));}
    else if(!strcmp(url,"/messages/send")) {
        cJSON *j=have_length&&!strstr(payload,"\\u0000")?cJSON_ParseWithOpts(payload,NULL,1):NULL;
        cJSON *text=cJSON_GetObjectItemCaseSensitive(j,"text"),*request=cJSON_GetObjectItemCaseSensitive(j,"request_id");unsigned id=0;
        int result=cJSON_IsObject(j)&&cJSON_GetArraySize(j)==2&&cJSON_IsString(text)&&cJSON_IsString(request)?local_message_text(client_identity,0,0,text->valuestring,"NFC网页",request->valuestring,sntp_get_epoch(),access_now,&id):1;
        cJSON_Delete(j);if(result){http_reply(fd,result==3?503:result==4?409:400,"{\"error\":\"text_not_accepted\"}");return;}
        snprintf(body,sizeof(body),"{\"ok\":true,\"id\":%u,\"boot_id\":%u}",id,safety_boot_id());
    }
    else if(!strcmp(url,"/messages/reply")||!strcmp(url,"/messages/seen")) {
        cJSON *j=have_length?cJSON_ParseWithOpts(payload,NULL,1):NULL;
        cJSON *id=cJSON_GetObjectItemCaseSensitive(j,"id"),*code=cJSON_GetObjectItemCaseSensitive(j,"code"),*request=cJSON_GetObjectItemCaseSensitive(j,"request_id");
        int result=1;unsigned message_id=0;
        int valid_id=cJSON_IsNumber(id)&&id->valuedouble>=1&&id->valuedouble<=999999999&&id->valuedouble==id->valueint;
        if(valid_id && cJSON_IsObject(j)) {
            if(!strcmp(url,"/messages/seen")) {
                cJSON *direction=cJSON_GetObjectItemCaseSensitive(j,"direction");
                if(cJSON_GetArraySize(j)==1)result=local_message_seen(id->valueint,0);
                else if(cJSON_GetArraySize(j)==2&&cJSON_IsString(direction)&&(!strcmp(direction->valuestring,"board")||!strcmp(direction->valuestring,"family")))result=local_message_seen(id->valueint,!strcmp(direction->valuestring,"family"));
            }
            else if(!strcmp(url,"/messages/reply")&&cJSON_GetArraySize(j)==3&&cJSON_IsNumber(code)&&code->valuedouble==code->valueint&&code->valueint>=1&&code->valueint<=3&&cJSON_IsString(request))
                result=local_message_reply(client_identity,code->valueint,id->valueint,request->valuestring,sntp_get_epoch(),access_now,&message_id);
        }
        cJSON_Delete(j);
        if(result){http_reply(fd,result==2?404:result==3?503:result==4?409:400,"{\"error\":\"message_rejected\"}");return;}
        snprintf(body,sizeof(body),"{\"ok\":true,\"id\":%u,\"boot_id\":%u}",message_id,safety_boot_id());
    }
    else    if (!strcmp(url, "/living") || !strcmp(url, "/medicine") || !strcmp(url, "/light"))
    {
        unsigned int id;
        int code = !strcmp(url, "/living") ? SAFETY_CMD_MEDICINE_PLAN :
                   !strcmp(url, "/medicine") ? SAFETY_CMD_MEDICINE_STATE : SAFETY_CMD_LIGHT;
        if (strcmp(method, "POST")) {
            http_reply(fd, 405, "{\"ok\":false,\"error\":\"post_required\"}"); return;
        }
        if (!local_control) {
            http_reply(fd, 403, "{\"ok\":false,\"error\":\"local_header_required\"}"); return;
        }
        if (!have_length || living_validate(code, payload)) {
            http_reply(fd, 400, "{\"ok\":false,\"error\":\"invalid_living_parameters\"}"); return;
        }
        if (safety_command_submit_payload(code, "http", payload, &id)) {
            http_reply(fd, 503, "{\"ok\":false,\"error\":\"command_queue_full\"}"); return;
        }
        snprintf(body, sizeof(body), "{\"ok\":true,\"command_id\":%u,\"boot_id\":%u,\"state\":\"queued\"}", id, safety_boot_id());
        status = 202;
    }
    else if (!strcmp(url,"/care-pause")) {
        cJSON *j=have_length?cJSON_ParseWithOpts(payload,NULL,1):NULL,*m=cJSON_GetObjectItemCaseSensitive(j,"minutes");
        int valid=cJSON_IsObject(j)&&cJSON_GetArraySize(j)==1&&cJSON_IsNumber(m)&&m->valuedouble>=0&&m->valuedouble<=1440&&m->valuedouble==m->valueint;
        if(valid)care_policy_pause(m->valueint,access_now);cJSON_Delete(j);
        if(!valid){http_reply(fd,400,"{\"error\":\"invalid_pause\"}");return;}
        rc=care_policy_json(body,sizeof(body),sntp_get_epoch(),access_now);
    }
    else if (!strcmp(url, "/care-policy")) {
        if (!strcmp(method, "POST")) {
            if (!local_control) {http_reply(fd,403,"{\"error\":\"local_header_required\"}");return;}
            int result = have_length ? care_policy_apply(payload) : 1;
            if (result) {http_reply(fd,result==4?409:result==2?500:400,"{\"error\":\"care_policy_rejected\"}");return;}
        }
        rc = care_policy_json(body,sizeof(body),sntp_get_epoch(),access_now);
    }
    else if (!strcmp(url, "/status"))
        rc = safety_status_json(body, sizeof(body));
    else if (!strcmp(url, "/events"))
        rc = build_events_json(body, sizeof(body));
    else if (!strcmp(url, "/info"))
        rc = build_info_json(body, sizeof(body));
    else if (!strcmp(url, "/diagnostics"))
        rc = safety_status_json(body, sizeof(body));
    else if (!strcmp(url, "/stats"))
        rc = safety_stats_json(body, sizeof(body));
    else if (!strcmp(url, "/config"))
    {
        if (!strcmp(method, "GET"))
        {rc = safety_config_json(body, sizeof(body));
            if(!rc){cJSON *config=cJSON_Parse(body);cJSON *threshold=cJSON_GetObjectItemCaseSensitive(config,"noact_ms");if(cJSON_IsNumber(threshold)){cJSON_SetNumberValue(threshold,care_policy_timeout((unsigned)threshold->valueint));if(!cJSON_PrintPreallocated(config,body,sizeof(body),0))rc=-1;}cJSON_Delete(config);}
        }
        else
        {
            cJSON *requested=cJSON_Parse(payload);int has_threshold=cJSON_GetObjectItemCaseSensitive(requested,"noact_ms")!=NULL;cJSON_Delete(requested);
            if(has_threshold&&care_policy_timeout(0)){http_reply(fd,409,"{\"error\":\"use_care_policy_for_threshold\"}");return;}
            rc = safety_config_apply_json(payload);
            if (rc)
            {
                status = rc == -2 ? 500 : 400;
                snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"config_rejected\"}");
                rc = 0;
            }
            else
                snprintf(body, sizeof(body), "{\"ok\":true,\"network_requires_reboot\":true}");
        }
    }
    else if (!strcmp(url, "/ack") || !strcmp(url, "/mute"))
    {
        unsigned int id;
        int code = !strcmp(url, "/ack") ? SAFETY_CMD_ACK : SAFETY_CMD_MUTE;
        rc = safety_command_submit(code, "http", &id);
        if (rc)
        {
            status = 503;
            snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"command_queue_full\"}");
            rc = 0;
        }
        else
            snprintf(body, sizeof(body),
                     "{\"ok\":true,\"cmd\":\"%s\",\"command_id\":%u,\"boot_id\":%u,\"state\":"
                     "\"queued\"}",
                     code == SAFETY_CMD_ACK ? "ack" : "mute", id, safety_boot_id());
    }
    else if (!strcmp(url, "/command"))
    {
        safety_command_t c;
        unsigned int id = 0;
        char *stop = NULL;
        if (query && strncmp(query, "id=", 3) == 0)
            id = (unsigned int)strtoul(query + 3, &stop, 10);
        if (!id || !stop || *stop)
        {
            status = 400;
            strcpy(body, "{\"ok\":false,\"error\":\"invalid_id\"}");
        }
        else if (!safety_command_get(id, &c))
        {
            status = 404;
            strcpy(body, "{\"ok\":false,\"error\":\"command_not_found\"}");
        }
        else
            snprintf(
                body, sizeof(body),
                "{\"ok\":true,\"command_id\":%u,\"boot_id\":%u,\"state\":\"%s\",\"result\":%d}", id,
                safety_boot_id(), c.result < 0 ? "queued" : "executed", c.result);
    }
    else
    {
        status = 404;
        strcpy(body, "{\"ok\":false,\"error\":\"unknown_endpoint\"}");
    }
    if (rc)
    {
        status = 500;
        strcpy(body, "{\"ok\":false,\"error\":\"serialization_failed\"}");
    }
    http_reply(fd, status, body);
}
static void net_diag_task(void *arg)
{
    while (1)
    {
        char ip[16];
        int rssi = 0;
        int linked = 0;
        WifiLinkedInfo li;

        memset(&li, 0, sizeof(li));
        wifi_sta_info(ip, sizeof(ip), &rssi, &linked);
        GetLinkedInfo(&li);
        printf("[net-diag] ap=%d sta=%d link=%d ch=%d ip=%s rssi=%d\r\n",
               (IsHotspotActive() == WIFI_HOTSPOT_ACTIVE) ? 1 : 0,
               (IsWifiActive() == WIFI_STATE_AVALIABLE) ? 1 : 0, linked, li.frequency, ip, rssi);
        LOS_Msleep(10000);
    }
}

/***************************************************************
 * 函数名称: net_http_server
 * 说    明: TCP/HTTP 服务器线程
 ***************************************************************/
static void net_http_server(void *arg)
{
    int server_fd;
    int ret;

    while (1)
    {
        server_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (server_fd < 0)
        {
            printf("[net] create socket fail\r\n");
            LOS_Msleep(2000);
            continue;
        }

        int flag = 1;
        setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(int));

        struct sockaddr_in serv_addr = {0};
        serv_addr.sin_family = AF_INET;
        serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
        serv_addr.sin_port = htons(NET_HTTP_PORT);

        ret = bind(server_fd, (struct sockaddr *)&serv_addr, sizeof(serv_addr));
        if (ret < 0)
        {
            printf("[net] bind fail\r\n");
            lwip_close(server_fd);
            LOS_Msleep(2000);
            continue;
        }

        ret = listen(server_fd, 8);
        if (ret != 0)
        {
            printf("[net] listen fail\r\n");
            lwip_close(server_fd);
            LOS_Msleep(2000);
            continue;
        }

        printf("[net] http server listening on %d\r\n", NET_HTTP_PORT);

        while (1)
        {
            struct sockaddr_in client_addr = {0};
            socklen_t len = sizeof(client_addr);
            int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &len);
            if (client_fd < 0)
            {
                LOS_Msleep(500);
                continue;
            }

            http_handle_client(client_fd);
            lwip_close(client_fd);
        }
    }
}

/***************************************************************
 * 函数名称: net_wifi_task
 * 说    明: 启动 AP 并拉起 HTTP 服务线程
 ***************************************************************/
static void net_wifi_task(void *arg)
{
    unsigned int tid;
    TSK_INIT_PARAM_S task = {0};

    /* HTTP 服务在任何模式都启动（绑定 INADDR_ANY：AP 模式走 192.168.2.1，STA 模式走 STA IP） */
    task.pfnTaskEntry = (TSK_ENTRY_FUNC)net_http_server;
    task.uwStackSize = 8192;
    task.pcName = "net http server";
    task.usTaskPrio = 25;
    if (LOS_TaskCreate(&tid, &task) != LOS_OK)
    {
        printf("[net] create http server task fail\r\n");
    }

    task.pfnTaskEntry = (TSK_ENTRY_FUNC)net_diag_task;
    task.uwStackSize = 4096;
    task.pcName = "net wifi diag";
    task.usTaskPrio = 26;
    if (LOS_TaskCreate(&tid, &task) != LOS_OK)
    {
        printf("[net] create diag task fail\r\n");
    }

    if (net_get_mode() == NET_MODE_AP)
    {
        printf("[net] mode=AP, starting Wi-Fi AP ...\r\n");

        FlashInit();
        /* 不自定义 MAC：与官方 b10 AP 例程一致，用默认 MAC */
        VendorSet(VENDOR_ID_WIFI_SSID, (unsigned char *)NET_AP_SSID, sizeof(NET_AP_SSID));
        VendorSet(VENDOR_ID_WIFI_PASSWD, (unsigned char *)NET_AP_PASS, sizeof(NET_AP_PASS));
        VendorSet(VENDOR_ID_NET_IP, (unsigned char *)NET_AP_IP, sizeof(NET_AP_IP));
        VendorSet(VENDOR_ID_NET_GW, (unsigned char *)NET_AP_GW, sizeof(NET_AP_GW));
        VendorSet(VENDOR_ID_NET_MASK, (unsigned char *)NET_AP_MASK, sizeof(NET_AP_MASK));

        SetApModeOn();

        LOS_Msleep(2000);
        printf("[net] AP SSID=%s  IP=192.168.2.1  http://192.168.2.1:%d/status\r\n", NET_AP_SSID,
               NET_HTTP_PORT);
    }
    else
    {
        printf("[net] mode=STA: AP 关闭；HTTP 监听在所有网卡（用板子 STA IP 访问）\r\n");
    }
}

static volatile int s_net_mode = NET_MODE_AP;

void net_set_mode(int mode)
{
    s_net_mode = mode;
}

int net_get_mode(void)
{
    return s_net_mode;
}

void net_server_start(void)
{
    unsigned int tid;
    TSK_INIT_PARAM_S task = {0};

    task.pfnTaskEntry = (TSK_ENTRY_FUNC)net_wifi_task;
    task.uwStackSize = 8192;
    task.pcName = "net wifi task";
    task.usTaskPrio = 24;
    if (LOS_TaskCreate(&tid, &task) != LOS_OK)
    {
        printf("[net] create wifi task fail\r\n");
    }
}
