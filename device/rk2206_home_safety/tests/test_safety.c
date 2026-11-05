#include "home_safety.h"
#include "safety_runtime.h"
#include "safety_stats.h"
#include "event_log.h"
#include "mqtt_client.h"
#include "sntp_client.h"
#include "sht30.h"
#include "mpu6050.h"
#include "bh1750.h"
#include "su_03t.h"
#include "living.h"
#include "iot_uart.h"
#include "iot_i2c.h"
#include "lcd.h"
#include "ui.h"
#include "cmsis_os2.h"
#include "MQTTClient.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include "lwip/sockets.h"
#include "../src/net_server.c"
#include "../src/mqtt_client.c"
#ifndef SAFETY_REAL_TLS_TEST
int safety_tls_connect(Network *network, const char *hostname) { (void)network; (void)hostname; return -1; }
void safety_tls_close(void) {}
#endif
float key_last_voltage(void) { return 2.191f; }
void body_induction_diagnostics(body_diagnostics_t *out) {
    memset(out, 0, sizeof(*out));
}
static uint64_t tick = 1000;
static unsigned int epoch = 0;
static int realtime = 0, fail_after = -1;
static unsigned char voice_bytes[256];
static unsigned int voice_len;
static char lcd_network_line[64], lcd_cloud_line[64], lcd_temp_cell[48], lcd_body_cell[48], lcd_night_cell[48];
static unsigned lcd_fills,lcd_clears,lcd_texts;
unsigned int lcd_init(void) { return 0; }
void lcd_fill(uint16_t x,uint16_t y,uint16_t xe,uint16_t ye,uint16_t color)
{ assert(x<=xe && y<=ye && xe<=LCD_W && ye<=LCD_H);lcd_fills++;if(x==0&&y==0&&xe>=LCD_W-1&&ye>=LCD_H-1)lcd_clears++; }
void lcd_draw_line(uint16_t x,uint16_t y,uint16_t xe,uint16_t ye,uint16_t color) {}
void lcd_draw_circle(uint16_t x,uint16_t y,uint8_t r,uint16_t color) {assert(x>=r&&y>=r&&x+r<LCD_W&&y+r<LCD_H);}
void lcd_draw_rectangle(uint16_t x,uint16_t y,uint16_t xe,uint16_t ye,uint16_t color) {assert(x<=xe&&y<=ye&&xe<LCD_W&&ye<LCD_H);}
void lcd_show_chinese(uint16_t x,uint16_t y,uint8_t *text,uint16_t fg,uint16_t bg,uint8_t size,uint8_t mode)
{
    lcd_texts++;
    assert(y+size<=LCD_H);
    unsigned width=0;const unsigned char *cursor=text;
    while(*cursor){if(*cursor>=0xE0 && cursor[1] && cursor[2]){width+=size;cursor+=3;}else{width+=size/2;cursor++;}}
    assert(x+width<=LCD_W);
    if(y==108 && x==18)snprintf(lcd_body_cell,sizeof(lcd_body_cell),"%s",text);
    if(y==168 && x==18)snprintf(lcd_night_cell,sizeof(lcd_night_cell),"%s",text);
    if(y==75 && x==121)snprintf(lcd_temp_cell,sizeof(lcd_temp_cell),"%s",text);
    if(y==28 && x==32)snprintf(lcd_network_line,sizeof(lcd_network_line),"%s",text);
    if(y==28 && x==160)snprintf(lcd_cloud_line,sizeof(lcd_cloud_line),"%s",text);
}
void lcd_show_string(uint16_t x,uint16_t y,const uint8_t *text,uint16_t fg,uint16_t bg,uint8_t size,uint8_t mode)
{
    lcd_texts++;
    assert(y + size <= LCD_H);
    if(y==48 && x==4+52)snprintf(lcd_temp_cell,sizeof(lcd_temp_cell),"%s",text);
    assert(!strstr((const char*)text,"Last report") && !strstr((const char*)text,"USB"));
}
unsigned int IoTUartInit(unsigned int id, const IotUartAttribute *param) { return 0; }
unsigned int IoTUartDeinit(unsigned int id) { return 0; }
int IoTUartRead(unsigned int id, unsigned char *data, unsigned int n) { return 0; }
int IoTUartWrite(unsigned int id, const unsigned char *data, unsigned int n)
{
    assert(voice_len + n <= sizeof(voice_bytes));
    memcpy(voice_bytes + voice_len, data, n); voice_len += n; return n;
}
static char keys[50][32], values[50][128];
static int count;
static unsigned int kv_writes;
struct netif *netif_default = NULL;
static int sta = 0;
static int wifi_rssi = -42;
static int wifi_attempts, wifi_success_at;
static char wifi_selected[33];
uint64_t LOS_TickCountGet(void)
{
    if (realtime)
    {
        struct timespec t;
        clock_gettime(CLOCK_MONOTONIC, &t);
        return (uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
    }
    return tick;
}
unsigned int osKernelGetTickFreq(void)
{
    return 1000;
}
void LOS_Msleep(unsigned int ms)
{
    usleep(ms * 1000);
}
int LOS_TaskCreate(unsigned int *id, TSK_INIT_PARAM_S *t)
{
    (void)id;
    (void)t;
    return -1;
}
osMutexId_t osMutexNew(void *a)
{
    pthread_mutex_t *m = malloc(sizeof(*m));
    assert(m);
    pthread_mutex_init(m, NULL);
    return m;
}
int osMutexAcquire(osMutexId_t m, unsigned int wait)
{
    if(wait==osWaitForever)return pthread_mutex_lock(m);
    struct timespec deadline;clock_gettime(CLOCK_REALTIME,&deadline);
    deadline.tv_sec+=wait/1000;
    deadline.tv_nsec+=(long)(wait%1000)*1000000L;
    if(deadline.tv_nsec>=1000000000L){deadline.tv_sec++;deadline.tv_nsec-=1000000000L;}
    return pthread_mutex_timedlock(m,&deadline);
}
int osMutexRelease(osMutexId_t m)
{
    return pthread_mutex_unlock(m);
}
osThreadId_t osThreadNew(osThreadFunc_t f, void *p, const osThreadAttr_t *a)
{
    (void)f;
    (void)p;
    (void)a;
    return NULL;
}
int osThreadGetPriority(osThreadId_t t)
{
    (void)t;
    return 0;
}
osThreadId_t osThreadGetId(void)
{
    return NULL;
}
unsigned int sntp_get_epoch(void)
{
    return epoch;
}
int UtilsGetValue(const char *k, char *out, unsigned int n)
{
    for (int i = 0; i < count; i++)
        if (!strcmp(k, keys[i]))
        {
            if (strlen(values[i]) + 1 > n)
                return -1;
            strcpy(out, values[i]);
            return strlen(out);
        }
    return -1;
}
int UtilsSetValue(const char *k, const char *v)
{
    kv_writes++;
    if (fail_after == 0)
        return -1;
    if (fail_after > 0)
        fail_after--;
    assert(strlen(k) < 32 && strlen(v) < 128 && strlen(v) > 0);
    for (int i = 0; i < count; i++)
        if (!strcmp(k, keys[i]))
        {
            strcpy(values[i], v);
            return 0;
        }
    assert(count < 50);
    strcpy(keys[count], k);
    strcpy(values[count++], v);
    return 0;
}
int UtilsDeleteValue(const char *k)
{
    for (int i = 0; i < count; i++)
        if (!strcmp(k, keys[i]))
        {
            count--;
            strcpy(keys[i], keys[count]);
            strcpy(values[i], values[count]);
            return 0;
        }
    return -1;
}
int GetLinkedInfo(WifiLinkedInfo *i)
{
    memset(i, 0, sizeof(*i));
    if (sta)
    {
        i->connState = WIFI_CONNECTED;
        i->ipAddress = inet_addr("192.168.1.23");
        i->rssi = wifi_rssi;
    }
    return 0;
}
int IsHotspotActive(void)
{
    return !sta;
}
int IsWifiActive(void)
{
    return sta;
}
int SetApModeOn(void)
{
    return 0;
}
int SetWifiModeOff(void) { sta = 0; return 0; }
int SetWifiModeOn(void)
{
    ++wifi_attempts;
    sta = wifi_success_at && wifi_attempts == wifi_success_at;
    return sta ? 0 : -1;
}
int WifiDisconnect(int x)
{
    (void)x;
    return 0;
}
int WifiConnect(unsigned char *s, unsigned char *p)
{
    (void)s;
    (void)p;
    return 0;
}
void FlashInit(void)
{
}
int VendorSet(int a, unsigned char *b, unsigned int c)
{
    if (a == VENDOR_ID_WIFI_ROUTE_SSID) snprintf(wifi_selected, sizeof(wifi_selected), "%s", b);
    (void)c;
    return 0;
}
static void *serve(void *arg)
{
    int fd = *(int *)arg;
    http_handle_client(fd);
    close(fd);
    return NULL;
}
static cJSON *request(const char *req, int expect)
{
    int fds[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    pthread_t t;
    assert(!pthread_create(&t, NULL, serve, &fds[1]));
    /* Fragment the request header to exercise stream parsing. */
    size_t half = strlen(req) / 2;
    assert(write(fds[0], req, half) == (ssize_t)half);
    usleep(1000);
    assert(write(fds[0], req + half, strlen(req) - half) == (ssize_t)(strlen(req) - half));
    char response[10000];
    int off = 0, r;
    while ((r = read(fds[0], response + off, sizeof(response) - 1 - off)) > 0)
        off += r;
    response[off] = 0;
    if (r < 0)
        perror("http read");
    close(fds[0]);
    pthread_join(t, NULL);
    int status = 0;
    if (sscanf(response, "HTTP/1.1 %d", &status) != 1 || status != expect)
    {
        fprintf(stderr, "REQUEST: %s\nRESPONSE: %s\nEXPECTED: %d\n", req, response, expect);
        abort();
    }
    char *body = strstr(response, "\r\n\r\n");
    assert(body);
    body += 4;
    char *len = strstr(response, "Content-Length:");
    assert(len && atoi(len + 15) == (int)strlen(body));
    if (strstr(response,"Content-Type: text/html")) {
        assert(strstr(body,"鸿颐安") && strstr(body,"/status") && strstr(body,"/events"));
        assert(strstr(body,"command('ack')") && strstr(body,"command('mute')"));
        assert(strstr(body,"/config") && strstr(body,"setTimeout(refresh,2000)"));
        return cJSON_CreateObject();
    }
    cJSON *j = cJSON_Parse(body);
    assert(j);
    return j;
}
static void test_fall(void)
{
    safety_fall_t f = {0};
    safety_fall_sample(&f, 3, 100, true);
    safety_fall_sample(&f, .1, 110, true);
    assert(!f.pending);
    safety_fall_sample(&f, 3, 200, true);
    assert(f.pending);
    safety_fall_confirm(&f, 1000, 0, false);
    assert(!f.active);
    safety_fall_confirm(&f, 5200, 0, false);
    assert(f.active);
    safety_fall_sample(&f, 0, 5300, false);
    assert(f.active);
    safety_fall_confirm(&f, 5400, 5400, true);
    assert(!f.active && !f.pending);
    memset(&f, 0, sizeof(f));
    safety_fall_sample(&f, .1, 100, true);
    safety_fall_sample(&f, 3, 1800, true);
    assert(!f.pending);
    puts("PASS ordered fall, full post-impact wait, invalid samples, recovery");
}
static int bus_fail = 0, crc_bad = 0;
static int sht_reads, sht_bad_once, sht_read_fail_once;
static int bus_init_calls, bus_init_success, bus_writes, bus_deinits;
unsigned int IoTI2cDeinit(unsigned int port) {assert(port==EI2C0_M2);bus_deinits++;bus_init_success=0;return 0;}
static unsigned char mpu_regs[256], last_reg;
unsigned int IoTI2cInit(unsigned int port, unsigned int speed)
{
    (void)port;
    (void)speed;
    ++bus_init_calls;
    if (bus_fail || bus_init_success) return 1; /* SDK rejects duplicate init. */
    bus_init_success = 1;
    return 0;
}
unsigned int IoTI2cWrite(unsigned int port, unsigned int address, const uint8_t *data,
                         unsigned int n)
{
    (void)port;
    ++bus_writes;
    if (bus_fail)
        return 1;
    if (address == 0x68)
    {
        last_reg = data[0];
        if (n == 2)
            mpu_regs[last_reg] = data[1];
    }
    return 0;
}
static uint8_t sensor_crc(uint8_t *data)
{
    uint8_t crc = 0xff;
    for (int i = 0; i < 2; i++)
    {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
            crc = crc & 0x80 ? (crc << 1) ^ 0x31 : crc << 1;
    }
    return crc;
}
unsigned int IoTI2cRead(unsigned int port, unsigned int address, uint8_t *data, unsigned int n)
{
    (void)port;
    if (address == 0x44) {
        sht_reads++;
        if (sht_read_fail_once) { sht_read_fail_once--; return 1; }
    }
    if (bus_fail)
        return 1;
    if (address == 0x68)
    {
        memcpy(data, mpu_regs + last_reg, n);
        return 0;
    }
    memset(data, 0, n);
    if (address == 0x23) { assert(n == 2); data[1] = 12; return 0; }
    data[0] = 0x66;
    data[1] = 0x66;
    data[2] = sensor_crc(data);
    data[3] = 0x80;
    data[4] = 0;
    data[5] = sensor_crc(data + 3);
    if (crc_bad || sht_bad_once) {
        data[5] ^= 1;
        if (sht_bad_once) sht_bad_once--;
    }
    return 0;
}
static void test_sensors(void)
{
    double locked_env[2]={123,456}, locked_light=789;
    short locked_imu[3]={11,22,33};
    unsigned writes_before=bus_writes;
    safety_i2c_lock();
    assert(sht30_read_data(locked_env)==-1);
    assert(locked_env[0]==123 && locked_env[1]==456);
    assert(bh1750_read_checked(&locked_light)==-1 && locked_light==789);
    assert(mpu6050_read_data(locked_imu)==-1 && locked_imu[0]==11);
    assert(bus_writes==writes_before);
    assert(!safety_i2c_try_lock(1)); /* Failed readers must not unlock another owner. */
    safety_i2c_unlock();
    assert(safety_i2c_try_lock(1));safety_i2c_unlock();
    puts("PASS shared bus contention: sensor reads time out, preserve outputs and lock ownership");
    bus_fail = 1;
    sht30_init();
    assert(bus_init_calls == 1 && bus_writes == 0);
    bus_fail = 0;
    sht30_init();
    assert(bus_init_calls == 2 && bus_init_success);
    bh1750_init();
    double light = -1;
    bh1750_read_data(&light);
    assert(light == 10);
    double env[2] = {123, 456};
    crc_bad = 1;
    int previous_reads = sht_reads;
    assert(sht30_read_data(env) == -1 && env[0] == 123 && env[1] == 456);
    assert(sht_reads == previous_reads + 2);
    crc_bad = 0;
    assert(!sht30_read_data(env) && env[0] > 20 && env[0] < 30);
    sht_bad_once = 1; previous_reads = sht_reads;
    assert(!sht30_read_data(env) && sht_reads == previous_reads + 2);
    sht_read_fail_once = 1; previous_reads = sht_reads;
    assert(!sht30_read_data(env) && sht_reads == previous_reads + 2);
    assert(sht30_read_data(NULL) == -1);
    puts("PASS SHT30 bounded retry: transient CRC/read error recovers, persistent CRC failure preserves both outputs");
    short acc[3] = {11, 22, 33};
    bus_fail = 1;
    assert(mpu6050_read_data(acc) == -1 && acc[0] == 11 && acc[1] == 22);
    assert(mpu6050_init() == -1);
    bus_fail = 0;
    mpu_regs[MPU6050_RA_WHO_AM_I] = 0;
    assert(mpu6050_init() == -1);
    mpu_regs[MPU6050_RA_WHO_AM_I] = 0x68;
    assert(!mpu6050_init());
    mpu_regs[MPU6050_ACC_OUT + 4] = 8;
    assert(!mpu6050_read_data(acc) && acc[2] == 2048);
    sht30_init(); bh1750_init();
    assert(bus_init_calls == 2); /* All three devices and retries share one bus. */
    puts("PASS shared I2C: failed init prevents writes, retry succeeds, duplicate init avoided across 3 devices");
    puts("PASS sensor bus errors, CRC rejection, ID/range check, preserved outputs");
    unsigned old_deinits=bus_deinits;
    bus_fail=1;
    assert(sht30_read_data(env)==-1); /* Two returned transfer failures. */
    assert(bh1750_read_checked(&light)==-1); /* Third failure resets controller. */
    assert(bus_deinits==old_deinits+1 && !bus_init_success);
    bus_fail=0;sht30_init();assert(!sht30_read_data(env));
    assert(bus_init_success && env[0]>20 && env[0]<30);
    bus_fail=1;assert(sht30_read_data(env)==-1);assert(bh1750_read_checked(&light)==-1);
    assert(bus_deinits==old_deinits+1); /* Cooldown prevents reset storms. */
    bus_fail=0;assert(!sht30_read_data(env));
    puts("PASS I2C recovery: 3 failed transfers reset once, next init/read recovers, cooldown prevents storms");
}
static void test_overlapping_alarms(void)
{
    bool previous[5] = {0}, alarms[5] = {0};
    unsigned int rising, falling;
    alarms[3] = true;
    safety_alarm_edges(alarms, previous, &rising, &falling);
    assert(rising == (1U << 3) && !falling);
    alarms[0] = true;
    safety_alarm_edges(alarms, previous, &rising, &falling);
    assert(rising == 1 && !falling);
    alarms[3] = false;
    safety_alarm_edges(alarms, previous, &rising, &falling);
    assert(!rising && falling == (1U << 3));
    alarms[0] = false;
    safety_alarm_edges(alarms, previous, &rising, &falling);
    assert(!rising && falling == 1);
    puts("PASS independent alarm starts and endings during overlapping alarms");
}
static void test_commands(void)
{
    unsigned int ids[8], id;
    safety_command_t cmd;
    for (int i = 0; i < 8; i++)
        assert(!safety_command_submit(i % 2 + 1, "test", &ids[i]));
    assert(safety_command_submit(1, "test", &id) == -2);
    for (int i = 0; i < 8; i++)
    {
        assert(safety_command_take(&cmd) && cmd.id == ids[i]);
        assert(safety_command_get(ids[i], &cmd) && cmd.result == -1);
        safety_command_done(&cmd, 0);
        assert(safety_command_get(ids[i], &cmd) && cmd.result == 0);
    }
    assert(!safety_command_take(&cmd));
    puts("PASS FIFO commands, overflow rejection, execution receipts");
}
static void test_http_events(void)
{
    unsigned int first = 0;
    for (int i = 0; i < 16; i++)
    {
        tick += 1000;
        int id = event_log_add("HELP", i);
        if (!i)
            first = id;
        event_log_set_duration(id, i + 1);
    }
    cJSON *j = request("GET /events HTTP/1.1\r\nHost: board\r\n\r\n", 200);
    cJSON *a = cJSON_GetObjectItem(j, "events");
    assert(cJSON_GetArraySize(a) == 16);
    assert(cJSON_GetObjectItem(cJSON_GetArrayItem(a, 15), "id")->valueint == (int)first);
    cJSON_Delete(j);
    int newer = event_log_add("GAS", 900);
    event_log_set_duration(first, 999);
    safety_event_t e;
    assert(event_log_entry(0, &e) && e.id == (unsigned int)newer && e.dur == 0);
    event_log_init();
    assert(event_log_entry(0, &e) && e.id == (unsigned int)newer);
    j = request("GET /status HTTP/1.1\r\nReferer: /ack\r\n\r\n", 200);
    assert(cJSON_GetObjectItem(j, "help"));
    cJSON_Delete(j);
    safety_command_t cmd;
    assert(!safety_command_take(&cmd));
    j = request("GET /acknowledge HTTP/1.1\r\n\r\n", 404);
    cJSON_Delete(j);
    j = request("POST /ack HTTP/1.1\r\nContent-Length: 0\r\n\r\n", 200);
    cJSON_Delete(j);
    assert(safety_command_take(&cmd) && cmd.code==SAFETY_CMD_ACK);
    safety_command_done(&cmd,0);
    j=request("POST /mute HTTP/1.1\r\nContent-Length: 2\r\n\r\n{}",200);
    cJSON_Delete(j);
    assert(safety_command_take(&cmd) && cmd.code==SAFETY_CMD_MUTE);
    safety_command_done(&cmd,0);
    sta = 1;
    net_set_mode(NET_MODE_STA);
    j = request("GET /info HTTP/1.1\r\n\r\n", 200);
    assert(!strcmp(cJSON_GetObjectItem(j, "ip")->valuestring, "192.168.1.23"));
    cJSON_Delete(j);
    j = request("GET /ack HTTP/1.1\r\n\r\n", 200);
    unsigned int id = cJSON_GetObjectItem(j, "command_id")->valueint;
    cJSON_Delete(j);
    assert(safety_command_take(&cmd) && cmd.id == id);
    safety_command_done(&cmd, 0);
    char req[120];
    snprintf(req, sizeof(req), "GET /command?id=%u HTTP/1.1\r\n\r\n", id);
    j = request(req, 200);
    assert(!strcmp(cJSON_GetObjectItem(j, "state")->valuestring, "executed"));
    cJSON_Delete(j);
    puts("PASS full 16-event HTTP, lengths, event IDs, exact routes, STA info, command receipt");
}
static void test_config(void)
{
    safety_config_t original, c;
    safety_config_get(&original);
    assert(safety_config_apply_json("{\"temp_low\":99,\"temp_high\":1}") == -1);
    assert(safety_config_apply_json("{\"report_ms\":1.5}") == -1);
    assert(safety_config_apply_json("{\"typo\":1}") == -1);
    assert(safety_config_apply_json("{} trailing") == -1);
    assert(safety_config_apply_json("{\"port\":1,\"port\":2}") == -1);
    const char *body = "{\"demo\":false,\"report_ms\":30000,\"wifi_pass\":\"secret-for-test\"}";
    char req[512];
    snprintf(req, sizeof(req), "POST /config HTTP/1.1\r\nContent-Length: %u\r\n\r\n%s",
             (unsigned int)strlen(body), body);
    cJSON *j = request(req, 200);
    cJSON_Delete(j);
    safety_config_get(&c);
    assert(c.noact_ms == 1800000 && c.report_ms == 30000 && !c.demo);
    j = request("GET /config HTTP/1.1\r\n\r\n", 200);
    assert(!cJSON_GetObjectItem(j, "wifi_pass"));
    cJSON_Delete(j);
    c.report_ms = 15000;
    fail_after = 2;
    assert(safety_config_set(&c) == -2);
    fail_after = -1;
    safety_config_get(&c);
    assert(c.report_ms == 30000);
    assert(safety_config_set(&original) == 0);
    puts("PASS config validation, profiles, secret redaction, failed save keeps active config");
}
static void test_stats(void)
{
    epoch = 1791800000;
    for (int i = 0; i < 7; i++)
    {
        epoch = 1791800000 + i * 86400;
        safety_stats_activity();
        safety_stats_alarm(i % 5 + 1);
        tick += 61000;
        unsigned int before=kv_writes;
        safety_stats_tick();
        assert(kv_writes-before==2);
    }
    char out[8192];
    assert(!safety_stats_json(out, sizeof(out)));
    cJSON *j = cJSON_Parse(out);
    assert(j);
    assert(cJSON_GetArraySize(cJSON_GetObjectItem(j, "days")) == 7);
    assert(cJSON_GetObjectItem(j, "today_activity")->valueint == 1);
    cJSON_Delete(j);
    safety_stats_init();
    assert(!safety_stats_json(out, sizeof(out)));
    j = cJSON_Parse(out);
    assert(cJSON_GetArraySize(cJSON_GetObjectItem(j, "days")) == 7);
    assert(cJSON_GetObjectItem(j, "today_activity")->valueint == 1);
    cJSON_Delete(j);
    assert(count <= 43);
    puts("PASS UTC+8 daily rollover, seven-day persistence/reload, KV capacity");
}
static void read_exact(int fd, unsigned char *out, int n)
{
    int off = 0;
    while (off < n)
    {
        int r = read(fd, out + off, n - off);
        assert(r > 0);
        off += r;
    }
}
static void *ack_publish(void *arg)
{
    int fd = *(int *)arg;
    unsigned char h[2];
    read_exact(fd, h, 2);
    assert((h[0] >> 4) == 3);
    int len = h[1] & 127, multiplier = 128;
    unsigned char byte = h[1];
    while (byte & 128)
    {
        read_exact(fd, &byte, 1);
        len += (byte & 127) * multiplier;
        multiplier *= 128;
        assert(multiplier <= 268435456);
    }
    assert(len < 2048);
    unsigned char packet[2048];
    read_exact(fd, packet, len);
    int topic_len = (packet[0] << 8) | packet[1];
    unsigned char ack[4] = {0x40, 2, packet[topic_len + 2], packet[topic_len + 3]};
    assert(write(fd, ack, 4) == 4);
    return NULL;
}
static void *ack_two_publishes(void *arg) { ack_publish(arg); ack_publish(arg); return NULL; }
static void test_wifi_backup(void)
{
    sta = 0; wifi_attempts = 0; wifi_success_at = 2;
    assert(mqtt_connect_wifi() == 0 && wifi_attempts == 2);
    assert(!strcmp(wifi_selected, "CONFIGURE_LOCALLY"));
    assert(mqtt_connect_wifi() == 0 && wifi_attempts == 2);
    sta = 0; wifi_attempts = 0; wifi_success_at = 1;
    assert(mqtt_connect_wifi() == 0 && wifi_attempts == 1);
    sta = 0; wifi_attempts = 0; wifi_success_at = 0;
    assert(mqtt_connect_wifi() == -1 && wifi_attempts == 3);
    sta = 0; wifi_attempts = 0; wifi_success_at = 3;
    assert(mqtt_connect_wifi() == 0 && wifi_attempts == 3);
    assert(!strcmp(wifi_selected, "CONFIGURE_LOCALLY"));
    assert(mqtt_connect_wifi() == 0 && wifi_attempts == 3);
    sta = 1;
    wifi_current = 2; wifi_recoveries = cloud_failures = 0; wifi_recovery_started = false;
    wifi_cloud_failed(1000); wifi_cloud_failed(2000); wifi_cloud_failed(3000);
    assert(sta == 1);
    wifi_cloud_failed(4000); assert(wifi_first == 2 && wifi_recoveries == 1);
    sta = 0; wifi_attempts = 0; wifi_success_at = 1;
    assert(mqtt_connect_wifi() == 0 && !strcmp(wifi_selected, "CONFIGURE_LOCALLY"));
    wifi_cloud_failed(5000); wifi_cloud_failed(6000); wifi_cloud_failed(7000); wifi_cloud_failed(8000);
    assert(wifi_recoveries == 1); /* no rapid reconnect storm */
    wifi_cloud_failed(124000); assert(wifi_first == 0 && wifi_recoveries == 2);
    sta = 0; wifi_attempts = 0; wifi_success_at = 1;
    assert(mqtt_connect_wifi() == 0 && !strcmp(wifi_selected, s_network_cfg.wifi_ssid));
    wifi_cloud_ok(); assert(cloud_failures == 0 && wifi_recoveries == 0);
    wifi_first = 0; sta = 1;
    puts("PASS cloud failure recovery: threshold, cooldown, current refresh, alternate rotation");
    puts("PASS three hotspots including CONFIGURE_LOCALLY, existing link preservation, all unavailable");
}
static void test_cloud_commands(void)
{
    char topic[256], response[256]; unsigned int slot;
    const char *request = "$oc/devices/test_device/sys/commands/request_id=abc-1";
    const char *body = "{\"service_id\":\"HomeSafety\",\"command_name\":\"ack\",\"paras\":{}}";
    cloud_command_accept(request, body, strlen(body));
    assert(cloud_response_next(topic, sizeof(topic), response, sizeof(response), &slot) == 0);
    cloud_command_accept(request, body, strlen(body));
    safety_command_t cmd;
    assert(safety_command_take(&cmd) && cmd.code == SAFETY_CMD_ACK && !strcmp(cmd.source, "cloud"));
    safety_command_t other;
    assert(!safety_command_take(&other));
    assert(cloud_response_next(topic, sizeof(topic), response, sizeof(response), &slot) == 0);
    safety_command_done(&cmd, 0);
    assert(cloud_response_next(topic, sizeof(topic), response, sizeof(response), &slot) == 1);
    assert(strstr(topic, "/response/request_id=abc-1") && strstr(response, "\"result_code\":0"));
    cloud_response_sent(slot);
    assert(cloud_response_next(topic, sizeof(topic), response, sizeof(response), &slot) == 0);
    cloud_command_accept(request, body, strlen(body));
    assert(!safety_command_take(&other));
    assert(cloud_response_next(topic, sizeof(topic), response, sizeof(response), &slot) == 1);
    cloud_response_sent(slot);
    const char *invalid = "{\"service_id\":\"Other\",\"command_name\":\"ack\",\"paras\":{}}";
    cloud_command_accept("$oc/devices/test_device/sys/commands/request_id=abc-2", invalid, strlen(invalid));
    assert(!safety_command_take(&other));
    assert(cloud_response_next(topic, sizeof(topic), response, sizeof(response), &slot) == 1);
    assert(strstr(response, "\"result_code\":1")); cloud_response_sent(slot);
    const char *mute = "{\"service_id\":\"HomeSafety\",\"command_name\":\"mute\",\"paras\":{}}";
    cloud_command_accept("$oc/devices/test_device/sys/commands/request_id=abc-3", mute, strlen(mute));
    assert(safety_command_take(&cmd) && cmd.code == SAFETY_CMD_MUTE); safety_command_done(&cmd, 0);
    assert(cloud_response_next(topic, sizeof(topic), response, sizeof(response), &slot) == 1); cloud_response_sent(slot);
    const char *sos = "{\"service_id\":\"HomeSafety\",\"command_name\":\"sos\",\"paras\":{}}";
    cloud_command_accept("$oc/devices/test_device/sys/commands/request_id=sos-one", sos, strlen(sos));
    assert(safety_command_take(&cmd) && cmd.code == SAFETY_CMD_SOS);
    safety_command_done(&cmd,0);
    assert(cloud_response_next(topic,sizeof(topic),response,sizeof(response),&slot)==1 && strstr(response,"\"result_code\":0"));cloud_response_sent(slot);
    cloud_command_accept("$oc/devices/test_device/sys/commands/request_id=sos-one", sos, strlen(sos));
    assert(!safety_command_take(&other));
    assert(cloud_response_next(topic,sizeof(topic),response,sizeof(response),&slot)==1);cloud_response_sent(slot);
    const char *bad_sos = "{\"service_id\":\"Other\",\"command_name\":\"sos\",\"paras\":{}}";
    cloud_command_accept("$oc/devices/test_device/sys/commands/request_id=sos-bad", bad_sos, strlen(bad_sos));
    assert(!safety_command_take(&other));
    assert(cloud_response_next(topic,sizeof(topic),response,sizeof(response),&slot)==1 && strstr(response,"\"result_code\":1"));cloud_response_sent(slot);
    const char *bad_sos_paras = "{\"service_id\":\"HomeSafety\",\"command_name\":\"sos\",\"paras\":{\"on\":1}}";
    cloud_command_accept("$oc/devices/test_device/sys/commands/request_id=sos-bad-paras", bad_sos_paras, strlen(bad_sos_paras));
    assert(!safety_command_take(&other));
    assert(cloud_response_next(topic,sizeof(topic),response,sizeof(response),&slot)==1 && strstr(response,"\"result_code\":1"));cloud_response_sent(slot);
    puts("PASS remote SOS: empty parameters, service validation, executed receipt and duplicate request suppression");
    const char *living="{\"service_id\":\"HomeSafety\",\"command_name\":\"set_living\",\"paras\":{\"night_auto\":true,\"medicine_enabled\":true,\"medicine_minute\":480,\"expected_revision\":1}}";
    cloud_command_accept("$oc/devices/test_device/sys/commands/request_id=life-1",living,strlen(living));
    assert(safety_command_take(&cmd) && cmd.code==SAFETY_CMD_LIVING);
    assert(!living_execute(cmd.code,cmd.payload,0));safety_command_done(&cmd,0);
    assert(cloud_response_next(topic,sizeof(topic),response,sizeof(response),&slot)==1 && strstr(response,"\"result_code\":0"));cloud_response_sent(slot);
    cloud_command_accept("$oc/devices/test_device/sys/commands/request_id=life-1",living,strlen(living));
    assert(!safety_command_take(&other));
    assert(cloud_response_next(topic,sizeof(topic),response,sizeof(response),&slot)==1);cloud_response_sent(slot);
    cloud_command_accept("$oc/devices/test_device/sys/commands/request_id=life-2",living,strlen(living));
    assert(safety_command_take(&cmd));assert(living_execute(cmd.code,cmd.payload,0)==4);safety_command_done(&cmd,4);
    assert(cloud_response_next(topic,sizeof(topic),response,sizeof(response),&slot)==1 && strstr(response,"\"result_code\":4"));cloud_response_sent(slot);
    const char *badlife="{\"service_id\":\"Other\",\"command_name\":\"confirm_medicine\",\"paras\":{\"dose_day\":21000}}";
    cloud_command_accept("$oc/devices/test_device/sys/commands/request_id=life-3",badlife,strlen(badlife));
    assert(!safety_command_take(&other));
    assert(cloud_response_next(topic,sizeof(topic),response,sizeof(response),&slot)==1 && strstr(response,"\"result_code\":1"));cloud_response_sent(slot);
    const char *plan="{\"service_id\":\"HomeSafety\",\"command_name\":\"set_medicine_plan\",\"paras\":{\"night_auto\":true,\"medicine_enabled\":true,\"medicine_minute\":0,\"medicine_enabled_2\":true,\"medicine_minute_2\":720,\"medicine_enabled_3\":true,\"medicine_minute_3\":1200,\"expected_revision\":2}}";
    cloud_command_accept("$oc/devices/test_device/sys/commands/request_id=plan-three",plan,strlen(plan));
    assert(safety_command_take(&cmd));assert(cmd.code==SAFETY_CMD_MEDICINE_PLAN);assert(strstr(cmd.payload,"medicine_minute_3"));
    assert(living_execute(cmd.code,cmd.payload,0)==0);safety_command_done(&cmd,0);
    puts("PASS cloud commands: ack/mute, living parameters, executed/conflict receipts, retry deduplication and service validation");
}
static void test_iotda_properties(void)
{
    safety_state_t state = {0};
    state.temp = 25.75; state.humi = 62.5; state.env_valid = 1; state.body_valid = 1;
    state.door_valid = 1; state.door = 1;
    state.help = 1; state.alarm = 1; state.status = 1;
    char payload[1024];
    assert(properties_json(&state, payload, sizeof(payload)) == 0);
    cJSON *root = cJSON_Parse(payload);
    cJSON *services = cJSON_GetObjectItem(root, "services");
    assert(cJSON_GetArraySize(services) == 1);
    cJSON *service = cJSON_GetArrayItem(services, 0);
    assert(!strcmp(cJSON_GetObjectItem(service, "service_id")->valuestring, "HomeSafety"));
    cJSON *props = cJSON_GetObjectItem(service, "properties");
    assert(cJSON_GetArraySize(props) == 13);
    assert(cJSON_GetObjectItem(props, "temp")->valuedouble == 25.75);
    const char *names[] = {"body", "inactive", "alarm", "status", "help", "fall", "gas_alarm", "smoke", "door"};
    for (unsigned int i = 0; i < sizeof(names)/sizeof(names[0]); ++i)
        assert(cJSON_IsNumber(cJSON_GetObjectItem(props, names[i])));
    assert(!strstr(payload, "password") && !strstr(payload, "secret"));
    cJSON_Delete(root);
    state.env_valid = 0;
    assert(properties_json(&state, payload, sizeof(payload)) == 0);
    root = cJSON_Parse(payload);
    props = cJSON_GetObjectItem(cJSON_GetArrayItem(cJSON_GetObjectItem(root, "services"), 0), "properties");
    assert(cJSON_GetArraySize(props) == 11 && !cJSON_GetObjectItem(props, "temp"));
    cJSON_Delete(root);
    state.door_valid = 0;
    assert(properties_json(&state, payload, sizeof(payload)) == 0);
    root = cJSON_Parse(payload);
    props = cJSON_GetObjectItem(cJSON_GetArrayItem(cJSON_GetObjectItem(root, "services"), 0), "properties");
    assert(!cJSON_GetObjectItem(props, "door"));
    cJSON_Delete(root);
    state.body_valid = 0;
    assert(properties_json(&state, payload, sizeof(payload)) == 0);
    root = cJSON_Parse(payload);
    props = cJSON_GetObjectItem(cJSON_GetArrayItem(cJSON_GetObjectItem(root, "services"), 0), "properties");
    assert(!cJSON_GetObjectItem(props, "body"));
    cJSON_Delete(root);
    assert(properties_json(&state, payload, 8) == -1);
    assert(safety_config_apply_json("{\"broker\":\"other.example\"}") == -1);
    assert(safety_config_apply_json("{\"client_id\":\"other_device\"}") == -1);
    char port_json[48];
    snprintf(port_json, sizeof(port_json), "{\"port\":%d}", IOTDA_PORT);
    assert(safety_config_apply_json(port_json) >= 0);
    snprintf(port_json, sizeof(port_json), "{\"port\":%d}", IOTDA_PORT == 1883 ? 8883 : 1883);
    assert(safety_config_apply_json(port_json) == -1);
    puts("PASS IoTDA model, numeric mapping, invalid sensor omission, fixed credential destination");
}
static void test_mqtt_queue(void)
{
    for (int i = 0; i < 17; i++)
        mqtt_alarm(i % 5 + 1, i);
    unsigned int pending, dropped;
    mqtt_queue_info(&pending, &dropped);
    assert(pending == 16 && dropped == 1);
    alarm_item_t first;
    assert(peek(&first));
    pop(first.seq + 1);
    mqtt_queue_info(&pending, &dropped);
    assert(pending == 16);
    realtime = 1;
    int fds[2];
    assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, fds));
    Network net;
    NetworkInit(&net);
    net.my_socket = fds[0];
    MQTTClient c = {0};
    unsigned char sendbuf[1024], readbuf[1024];
    MQTTClientInit(&c, &net, 100, sendbuf, sizeof(sendbuf), readbuf, sizeof(readbuf));
    c.isconnected = 1;
    char payload[512];
    assert(publish_pending(&c, payload, sizeof(payload)) == FAILURE);
    mqtt_queue_info(&pending, &dropped);
    assert(pending == 16);
    close(fds[1]);
    NetworkDisconnect(&net);
    assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, fds));
    net.my_socket = fds[0];
    c.isconnected = 1;
    pthread_t t;
    assert(!pthread_create(&t, NULL, ack_two_publishes, &fds[1]));
    assert(publish_pending(&c, payload, sizeof(payload)) == SUCCESS);
    pthread_join(t, NULL);
    mqtt_queue_info(&pending, &dropped);
    assert(pending == 15);
    assert(strstr(payload, "\"name\":\"alarm_event\""));
    close(fds[1]);
    NetworkDisconnect(&net);
    realtime = 0;
    puts("PASS alarm queue bounds, failure retention, successful QoS1 retry dequeues");
}
static void test_mqtt(void)
{
    realtime = 1;
    Timer timer;
    TimerInit(&timer);
    TimerCountdownMS(&timer, 35);
    assert(!TimerIsExpired(&timer));
    usleep(45000);
    assert(TimerIsExpired(&timer));
    int fds[2];
    assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, fds));
    Network net;
    NetworkInit(&net);
    net.my_socket = fds[0];
    unsigned char sendbuf[1024], readbuf[1024];
    MQTTClient c = {0};
    MQTTClientInit(&c, &net, 500, sendbuf, sizeof(sendbuf), readbuf, sizeof(readbuf));
    c.isconnected = 1;
    uint64_t before = LOS_TickCountGet();
    assert(MQTTYield(&c, 30) == 0);
    assert(LOS_TickCountGet() - before < 250);
    pthread_t t;
    assert(!pthread_create(&t, NULL, ack_publish, &fds[1]));
    MQTTMessage msg = {0};
    msg.qos = QOS1;
    msg.payload = "{}";
    msg.payloadlen = 2;
    assert(MQTTPublish(&c, "alarm", &msg) == SUCCESS);
    pthread_join(t, NULL);
    close(fds[1]);
    unsigned char b;
    assert(linux_read(&net, &b, 1, 20) == -1);
    NetworkDisconnect(&net);
    assert(net.my_socket == -1);
    realtime = 0;
    puts("PASS monotonic MQTT timer, idle receive bounds, QoS1 PUBACK, EOF");
}
static void test_data_quality(void)
{
    safety_state_t state = {0};
    safety_quality_t q;
    safety_state_quality(&state, 0, &q);
    assert(!strcmp(q.data_state, "waiting") && q.sample_age_s == -1 && q.stale);
    state.sample_received = 1; state.sample_up = 0; state.env_valid = 1;
    safety_state_quality(&state, 0, &q);
    assert(!q.stale && q.env_available && !q.fall_detection_available);
    state.sample_up = 10; state.imu_valid = 1;
    safety_state_quality(&state, 20, &q);
    assert(q.sample_age_s == 10 && !q.stale && !q.fall_detection_available);
    safety_state_quality(&state, 21, &q);
    assert(q.stale && !q.env_available && !q.fall_detection_available);
    safety_state_quality(&state, 9, &q);
    assert(q.stale && q.sample_age_s == -1);
    safety_state_set(&state);
    tick = 21000;
    char payload[2048];
    assert(safety_status_json(payload, sizeof(payload)) == 0);
    cJSON *j = cJSON_Parse(payload);
    assert(cJSON_IsTrue(cJSON_GetObjectItem(j, "data_stale")));
    assert(cJSON_GetObjectItem(j, "sample_age_s")->valueint == 11);
    cJSON_Delete(j);
    assert(health_message_json(payload, sizeof(payload)) == SUCCESS);
    j = cJSON_Parse(payload);
    cJSON *health = cJSON_GetObjectItem(j, "content");
    assert(cJSON_GetObjectItem(cJSON_GetObjectItem(health,"living"),"config_revision")->valueint==1);
    assert(cJSON_IsTrue(cJSON_GetObjectItem(health, "data_stale")));
    assert(cJSON_IsFalse(cJSON_GetObjectItem(health, "fall_detection_available")));
    assert(cJSON_IsFalse(cJSON_GetObjectItem(health, "tls")));
    cJSON_Delete(j);
    assert(health_message_json(payload, 8) == FAILURE);
    tick = 1000;
    puts("PASS data quality: startup, zero-time sample, stale boundary, invalid clock, HTTP/cloud metadata");
}
static void test_voice(void)
{
    bool alarms[5] = {false,false,false,true,false};
    assert(safety_voice_select(alarms,1U << 3,false) == 4);
    assert(safety_voice_select(alarms,0,false) == 0);
    alarms[0] = true;
    assert(safety_voice_select(alarms,1U << 0,false) == 1);
    assert(safety_voice_select(alarms,1U << 4,false) == 0);
    assert(safety_voice_select(alarms,1U << 0,true) == 0);
    memset(alarms,1,sizeof(alarms));
    assert(safety_voice_select(alarms,31,false) == 1);
    tick = 1000; voice_len = 0;
    safety_state_t state = {0};
    state.sample_up = 1;
    safety_state_set(&state);
    const uint8_t temp_query[] = {3,1};
    su03t_receive_bytes(temp_query, sizeof(temp_query));
    assert(voice_len == 0); /* Invalid defaults must not be spoken as zero. */
    state.env_valid = 1; state.temp = 25.5; state.humi = 60;
    safety_state_set(&state);
    su03t_receive_bytes(temp_query, 1);
    assert(voice_len == 0);
    su03t_receive_bytes(temp_query + 1, 1);
    assert(voice_len == 13 && voice_bytes[0] == 0xaa && voice_bytes[2] == 1);
    assert(voice_bytes[11] == 0x55 && voice_bytes[12] == 0xaa);
    double value; memcpy(&value, voice_bytes + 3, sizeof(value)); assert(value == 25.5);
    voice_len = 0;
    const uint8_t queries[] = {0xff,3,2,3,1,3,3};
    su03t_receive_bytes(queries, sizeof(queries));
    assert(voice_len == 26 && voice_bytes[2] == 2 && voice_bytes[15] == 1);
    voice_len = 0; tick = 12000;
    su03t_receive_bytes(temp_query, 2); assert(voice_len == 0);
    tick = 1000; state.temp = 0; safety_state_set(&state);
    su03t_receive_bytes(temp_query, 2);
    assert(voice_len == 13); memcpy(&value,voice_bytes + 3,8); assert(value == 0);
    voice_len=0;state.mute=1;safety_state_set(&state);
    su03t_receive_bytes(temp_query,2);assert(voice_len==0);
    state.mute=0;state.alarm=1;safety_state_set(&state);
    su03t_receive_bytes(temp_query,2);assert(voice_len==0);
    state.alarm=0;safety_state_set(&state);
    su03t_receive_bytes(temp_query,1);tick+=600;
    su03t_receive_bytes(temp_query+1,1);assert(voice_len==0);
    tick=1000;
    const uint8_t light_on[]={1,1},light_off[]={1,2},automatic[]={0,1};
    su03t_receive_bytes(light_on,1);su03t_receive_bytes(light_on+1,1);
    living_tick(0,1000,500,1,0);living_status_t lamp;living_get(&lamp);assert(lamp.night_on);
    su03t_receive_bytes(light_off,2);living_tick(0,1001,0,1,0);living_get(&lamp);assert(!lamp.night_on);
    su03t_receive_bytes(light_on,2);living_tick(0,1002,0,1,1);living_get(&lamp);assert(!lamp.night_on&&lamp.light_owner==3);
    su03t_receive_bytes(automatic,2);living_tick(0,1003,500,1,0);living_get(&lamp);assert(!lamp.night_on);
    assert(living_validate(SAFETY_CMD_LIGHT,"{\"mode\":true}")==1);
    assert(living_validate(SAFETY_CMD_LIGHT,"{\"mode\":2}")==1);
    assert(living_execute(SAFETY_CMD_LIGHT,"{\"mode\":1}",0)==0);
    living_tick(0,1100,500,1,0);living_get(&lamp);assert(lamp.night_on && lamp.light_mode==1);
    living_tick(0,1101,500,1,1);living_get(&lamp);assert(lamp.light_owner==3);
    assert(living_execute(SAFETY_CMD_LIGHT,"{\"mode\":0}",0)==0);
    living_tick(0,1102,0,1,0);living_get(&lamp);assert(!lamp.night_on);
    assert(living_execute(SAFETY_CMD_LIGHT,"{\"mode\":-1}",0)==0);
    printf("PASS factory voice lamp: split ON, OFF, AUTO, alarm priority\n");
    voice_len = 0;
    su03t_send_u8_msg(VOICE_IDX_NO_ACTIVITY,0);
#if VOICE_DEDICATED_IDX
    assert(voice_len == 6 && voice_bytes[2] == 4 && voice_bytes[3] == 0);
    assert(voice_bytes[4] == 0x55 && voice_bytes[5] == 0xaa);
#else
    assert(voice_len == 0); /* Factory profile must not receive numeric alarm zero. */
#endif
    voice_len=0;
    bool active[5]={true,false,false,false,false};
    su03t_process_voices(active,1,1,0,100000);
#if VOICE_DEDICATED_IDX
    assert(voice_len==6 && voice_bytes[2]==8);
#else
    assert(voice_len==0);
#endif
    voice_len=0;
    su03t_process_voices(active,1,1,0,110000);assert(!voice_len);
    active[0]=false;
    su03t_process_voices(active,1,1,0,116000);
#if VOICE_DEDICATED_IDX && SU03T_MEDICINE_PROFILE_ENABLED
    assert(voice_len==6 && voice_bytes[2]==9);
#else
    assert(voice_len==0);
#endif
    voice_len=0;
    su03t_process_voices(active,1,1,0,200000);assert(!voice_len);
    su03t_process_voices(active,0,0,0,201000);
    active[0]=true;
    su03t_process_voices(active,0,0,1,202000);assert(!voice_len);
    puts("PASS voice scheduler UART: SOS mapping, held alarm once, deferred medicine, mute and vocabulary gates");
    puts("PASS voice: invalid/stale silence, split/combined queries, exact frame, valid zero, alarm profile guard");
}
static void test_cloud_display(void)
{
    safety_mqtt_connected(0);
    sta = 0;net_set_mode(NET_MODE_STA);tick=1000;
    ui_show_main();
    assert(!strcmp(lcd_network_line,"网络未连接"));
    assert(!strcmp(lcd_cloud_line,"云端未连接"));
    sta = 1;
    wifi_rssi = 0;
    ui_show_main();
    assert(!strcmp(lcd_network_line,"网络已连接"));
    assert(!strcmp(lcd_cloud_line,"云端未连接"));
    wifi_rssi = -42;ui_show_main();
    assert(!strcmp(lcd_network_line,"网络已连接"));
    safety_mqtt_connected(1);
    safety_mqtt_report_ok(1,0);tick=6000;
    ui_show_main();
    assert(!strcmp(lcd_cloud_line,"云端已连接"));
    char payload[2048];assert(safety_status_json(payload,sizeof(payload))==0);
    cJSON *j=cJSON_Parse(payload);
    assert(cJSON_GetObjectItem(j,"mqtt")->valueint==1);
    assert(cJSON_GetObjectItem(j,"mqtt_report_age_s")->valueint==5);
    assert(cJSON_IsTrue(cJSON_GetObjectItem(j,"mqtt_report_received")));
    cJSON_Delete(j);
    safety_mqtt_connected(0);tick=11000;
    ui_show_main();
    assert(!strcmp(lcd_cloud_line,"云端未连接"));
    net_set_mode(NET_MODE_AP);ui_show_main();
    assert(!strcmp(lcd_network_line,"本地热点"));
    assert(!strcmp(lcd_cloud_line,"云端未启用"));
    ui_show_diagnostics();
    tick=1000;sta=0;net_set_mode(NET_MODE_STA);
    puts("PASS LCD/cloud diagnostics: WiFi separate from MQTT, unknown RSSI, PUBACK age, disconnect history, AP");
}
static void test_page_refresh(void)
{
    ui_show_main();ui_show_diagnostics();
    unsigned f=lcd_fills,t=lcd_texts,c=lcd_clears;
    for(int i=0;i<20;i++)ui_show_diagnostics();
    assert(lcd_fills==f && lcd_texts==t && lcd_clears==c);
    safety_mqtt_connected(!safety_mqtt_is_connected());ui_show_diagnostics();
    assert(lcd_fills==f+1 && lcd_texts==t+1 && lcd_clears==c);
    ui_show_living(NULL,-1,-1);f=lcd_fills;t=lcd_texts;c=lcd_clears;
    for(int i=0;i<20;i++)ui_show_living(NULL,-1,-1);
    assert(lcd_fills==f && lcd_texts==t && lcd_clears==c);
    living_status_t status;living_get(&status);living_config_t draft=status.config;
    draft.night_auto=!draft.night_auto;ui_show_living(&draft,0,-1);
    assert(lcd_clears==c && lcd_fills>f && lcd_fills<=f+6 && lcd_texts>t && lcd_texts<=t+6);
    f=lcd_fills;t=lcd_texts;ui_show_living(&draft,1,-1);
    assert(lcd_clears==c && lcd_fills>f && lcd_fills<=f+6 && lcd_texts>t && lcd_texts<=t+6); /* Night marker, slot marker and editing hint. */
    f=lcd_fills;t=lcd_texts;
    for(int i=0;i<20;i++)ui_show_living(&draft,1,-1);
    assert(lcd_fills==f && lcd_texts==t);
    draft.medicine_minute=1439;ui_show_living(&draft,2,-1);
    draft.medicine_minute=0;ui_show_living(&draft,3,0);
    assert(lcd_clears==c); /* Shorter text is erased within its own row. */
    ui_show_events(0);c=lcd_clears;ui_show_living(NULL,-1,-1);assert(lcd_clears==c+1);
    c=lcd_clears;ui_show_diagnostics();assert(lcd_clears==c+1);
    ui_show_main();c=lcd_clears;ui_show_diagnostics();assert(lcd_clears==c+1);
    puts("PASS LCD row caching: unchanged pages write nothing, changed rows only, page re-entry resets cache");
}
static void test_event_page_states(void)
{
    event_log_clear();ui_show_events(0);
    unsigned fills=lcd_fills,texts=lcd_texts,clears=lcd_clears;
    for(int i=0;i<20;i++)ui_show_events(0);
    assert(lcd_fills==fills && lcd_texts==texts && lcd_clears==clears);
    safety_state_t state={0};state.help=1;state.alarm=1;
    safety_state_set(&state);
    int id=event_log_add("HELP",0);ui_show_events(0);
    assert(lcd_clears==clears && lcd_texts>texts);
    fills=lcd_fills;texts=lcd_texts;ui_show_events(0);
    assert(lcd_fills==fills && lcd_texts==texts);
    state.help=0;state.alarm=0;safety_state_set(&state);
    event_log_set_duration(id,10);ui_show_events(0);
    assert(lcd_clears==clears && lcd_texts>texts);
    for(int i=0;i<16;i++)event_log_add("MED_TAKEN",i%3+1);
    ui_show_events(1);clears=lcd_clears;fills=lcd_fills;texts=lcd_texts;
    for(int i=0;i<20;i++)ui_show_events(1);
    assert(lcd_clears==clears && lcd_fills==fills && lcd_texts==texts);
    event_log_clear();ui_show_events(1);assert(lcd_clears==clears);
    puts("PASS event pages: empty/history/end transitions repaint rows only; both pages cache and clip safely");
}
static void test_deferred_display(void)
{
    safety_state_t state={0};unsigned f=lcd_fills,t=lcd_texts;
    /* A burst of page requests never renders from the safety caller. */
    for(int i=0;i<100;i++)ui_present(i%5,&state,"正常",NULL,-1,-1,0);
    ui_present(4,&state,"正常",NULL,-1,-1,0);
    assert(lcd_fills==f && lcd_texts==t);
    ui_render_pending();assert(lcd_texts>t);
    f=lcd_fills;t=lcd_texts;ui_render_pending();assert(lcd_fills==f&&lcd_texts==t);
    ui_present(4,&state,"正常",NULL,-1,-1,0);ui_render_pending();
    assert(lcd_fills==f&&lcd_texts==t);
    ui_present(3,&state,"正常",NULL,-1,-1,0);assert(lcd_fills==f);
    ui_render_pending();assert(lcd_fills>f);
    puts("PASS deferred display: safety caller writes no LCD, request bursts coalesce, unchanged snapshots do not redraw");
}
static void test_monitor_display_snapshot(void)
{
    safety_state_t frame = {0};
    frame.sample_received=1;frame.sample_up=(unsigned int)(tick/1000);
    frame.temp=25.5; frame.humi=60; frame.env_valid=1; frame.env_cached=1;
    frame.body=1; frame.body_valid=1; frame.door_valid=1;
    safety_state_set(&frame);
    living_tick(0,900000,5,1,1);
    ui_present(0,&frame,"无活动",NULL,-1,-1,0);
    /* The display worker must not mix a queued frame with newer invalid data. */
    safety_state_t newer={0}; safety_state_set(&newer);
    living_tick(0,903000,5,1,0);
    ui_render_pending();
    assert(!strcmp(lcd_temp_cell,"25.5"));
    assert(!strcmp(lcd_body_cell,"人体 有活动"));
    assert(!strcmp(lcd_night_cell,"夜灯 报警"));
    ui_present(0,&newer,"正常",NULL,-1,-1,0); ui_render_pending();
    assert(!strcmp(lcd_temp_cell,"--") && !strcmp(lcd_body_cell,"人体 待确认"));
    puts("PASS display: bounded cached data without decorative prefix, invalid PIR is unknown, queued temperature/validity/light owner share one snapshot");
}
int main(void)
{
    setbuf(stdout, NULL);
    assert(!safety_runtime_init());
    living_init();
    event_log_init();
    safety_stats_init();
    mqtt_config_init();
    test_data_quality();
    test_cloud_display();
    test_page_refresh();
    test_event_page_states();
    test_deferred_display();
    test_voice();
    test_fall();
    test_sensors();
    test_monitor_display_snapshot();
    test_overlapping_alarms();
    test_commands();
    cJSON *page_test=request("GET / HTTP/1.1\r\nHost: board\r\n\r\n",200);
    cJSON_Delete(page_test);
    puts("PASS local HTML: UTF-8 page, polling, events, ACK/mute and settings");
    test_http_events();
    test_config();
    test_stats();
    test_mqtt();
    char stats_payload[8192];
    assert(stats_message_json(stats_payload, sizeof(stats_payload)) == 0);
    cJSON *stats_message = cJSON_Parse(stats_payload);
    assert(stats_message && cJSON_GetObjectItem(stats_message, "content"));
    cJSON_Delete(stats_message);
    test_wifi_backup();
    test_cloud_commands();
    test_iotda_properties();
    test_mqtt_queue();
    puts("ALL TESTS PASSED");
    return 0;
}

void lcd_metric_slide(uint16_t x,uint16_t y,const char *old_text,const char *new_text,
                      uint8_t old_size,uint8_t new_size,uint8_t progress,uint16_t fg,uint16_t bg)
{
    (void)old_text;(void)old_size;(void)progress;
    lcd_show_chinese(x,y,(uint8_t*)new_text,fg,bg,new_size,0);
}
