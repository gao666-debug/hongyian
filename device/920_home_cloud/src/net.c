/**
 * 联网与授时实现
 *
 * 为什么自己写 SNTP：本 SDK 没有 SNTP，也没有 settimeofday，
 * uapi_rtc 只是定时器不是墙钟。而华为云 clientId 需要 "YYYYMMDDHH"，
 * 且用的是 **UTC**（实测：本地 11:32 生成的 clientId 尾号是 03 点）。
 */

#include "cfg.h"
#include "net.h"

#include "lwip/netifapi.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "wifi_hotspot.h"
#include "wifi_hotspot_config.h"
#include "cmsis_os2.h"
#include "common_def.h"
#include "soc_osal.h"

#define NET_LOG                 "[NET] "
#define WIFI_IFNAME_MAX_SIZE    16
#define WIFI_MAX_SSID_LEN       33
#define WIFI_SCAN_AP_LIMIT      64
#define WIFI_MAC_LEN            6
#define WIFI_GET_IP_MAX_COUNT   300

#define NTP_PORT                123
#define NTP_PKT_LEN             48
#define NTP_TIMEOUT_SEC         3
#define NTP_FALLBACK_IP         "203.107.6.88"      /* ntp.aliyun.com 兜底 */
#define NTP_EPOCH_DELTA         2208988800UL        /* 1900 -> 1970 */
#define TICKS_PER_SEC           100                 /* LOSCFG_BASE_CORE_TICK_PER_SECOND=100 */

enum {
    NET_ST_INIT = 0,
    NET_ST_SCANING,
    NET_ST_SCAN_DONE,
    NET_ST_FOUND_TARGET,
    NET_ST_CONNECTING,
    NET_ST_CONNECT_DONE,
    NET_ST_GET_IP,
};

static td_u8   g_wifi_state;
static td_bool g_net_ready = TD_FALSE;
static td_u32  g_epoch_anchor = 0;      /* 校时时刻的 UTC 秒 */
static td_u32  g_tick_anchor = 0;       /* 校时时刻的系统 tick */

static td_void net_scan_state_changed(td_s32 state, td_s32 size)
{
    unused(state);
    unused(size);
    g_wifi_state = NET_ST_SCAN_DONE;
}

static td_void net_connection_changed(td_s32 state, const wifi_linked_info_stru *info, td_s32 reason_code)
{
    unused(info);
    unused(reason_code);
    if (state == 0) {
        osal_printk(NET_LOG "connect fail, retry\r\n");
        g_wifi_state = NET_ST_INIT;
    } else {
        osal_printk(NET_LOG "connect succ\r\n");
        g_wifi_state = NET_ST_CONNECT_DONE;
    }
}

static wifi_event_stru g_wifi_event_cb = {
    .wifi_event_connection_changed = net_connection_changed,
    .wifi_event_scan_state_changed = net_scan_state_changed,
};

static td_s32 net_get_match_network(wifi_sta_config_stru *expected_bss)
{
    td_u32 num = WIFI_SCAN_AP_LIMIT;
    td_char expected_ssid[] = CFG_WIFI_SSID;
    td_char key[] = CFG_WIFI_PASS;
    td_bool find_ap = TD_FALSE;
    td_u8 bss_index;
    td_u32 scan_len = sizeof(wifi_scan_info_stru) * WIFI_SCAN_AP_LIMIT;
    wifi_scan_info_stru *result = osal_kmalloc(scan_len, OSAL_GFP_ATOMIC);

    if (result == TD_NULL) {
        return -1;
    }
    memset_s(result, scan_len, 0, scan_len);

    if (wifi_sta_get_scan_info(result, &num) != 0) {
        osal_kfree(result);
        return -1;
    }

    for (bss_index = 0; bss_index < num; bss_index++) {
        if (strlen(expected_ssid) == strlen(result[bss_index].ssid) &&
            memcmp(expected_ssid, result[bss_index].ssid, strlen(expected_ssid)) == 0) {
            find_ap = TD_TRUE;
            break;
        }
    }
    if (find_ap == TD_FALSE) {
        osal_kfree(result);
        return -1;
    }

    if (memcpy_s(expected_bss->ssid, WIFI_MAX_SSID_LEN, expected_ssid, strlen(expected_ssid)) != 0 ||
        memcpy_s(expected_bss->bssid, WIFI_MAC_LEN, result[bss_index].bssid, WIFI_MAC_LEN) != 0 ||
        memcpy_s(expected_bss->pre_shared_key, WIFI_MAX_SSID_LEN, key, strlen(key)) != 0) {
        osal_kfree(result);
        return -1;
    }
    expected_bss->security_type = result[bss_index].security_type;
    expected_bss->ip_type = 1;      /* 动态 DHCP */
    osal_kfree(result);
    return 0;
}

static td_u32 net_sta_scan(td_void)
{
    wifi_scan_params_stru scan_params = {0};

    if (memcpy_s(scan_params.ssid, sizeof(scan_params.ssid), CFG_WIFI_SSID, strlen(CFG_WIFI_SSID) + 1) != EOK) {
        return ERRCODE_FAIL;
    }
    scan_params.scan_type = WIFI_SSID_SCAN;
    scan_params.ssid_len = strlen(CFG_WIFI_SSID);
    scan_params.ssid[scan_params.ssid_len] = '\0';
    return wifi_sta_scan_advance(&scan_params);
}

/* 极简 SNTP：发 48 字节请求，取响应第 40~43 字节（transmit timestamp，大端） */
static td_s32 sntp_get_epoch(td_u32 *out_epoch)
{
    struct sockaddr_in addr = {0};
    struct timeval tv;
    td_u8 pkt[NTP_PKT_LEN] = {0};
    td_s32 fd;
    td_s32 ret;
    struct addrinfo hints = {0};
    struct addrinfo *res = NULL;
    td_u32 secs;

    addr.sin_family = AF_INET;
    addr.sin_port = htons(NTP_PORT);

    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo("ntp.aliyun.com", "123", &hints, &res) == 0 && res != NULL) {
        addr.sin_addr.s_addr = ((struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr;
        freeaddrinfo(res);
    } else {
        addr.sin_addr.s_addr = inet_addr(NTP_FALLBACK_IP);
        osal_printk(NET_LOG "ntp dns fail, use fallback\r\n");
    }

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        osal_printk(NET_LOG "socket fail\r\n");
        return -1;
    }

    tv.tv_sec = NTP_TIMEOUT_SEC;
    tv.tv_usec = 0;
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    pkt[0] = 0x1B;      /* LI=0, VN=3, Mode=3(客户端) */
    if (sendto(fd, pkt, NTP_PKT_LEN, 0, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        lwip_close(fd);
        return -1;
    }

    ret = recv(fd, pkt, NTP_PKT_LEN, 0);
    lwip_close(fd);
    if (ret < NTP_PKT_LEN) {
        osal_printk(NET_LOG "ntp recv fail ret=%d\r\n", ret);
        return -1;
    }

    secs = ((td_u32)pkt[40] << 24) | ((td_u32)pkt[41] << 16) |
           ((td_u32)pkt[42] << 8) | (td_u32)pkt[43];
    if (secs < NTP_EPOCH_DELTA) {
        osal_printk(NET_LOG "bad ntp ts\r\n");
        return -1;
    }
    *out_epoch = secs - NTP_EPOCH_DELTA;
    return 0;
}

/* 把 Unix 时间换算成 UTC 的年月日时（不依赖 libc 的 gmtime） */
td_void net_epoch_to_utc(td_u32 epoch, td_s32 *year, td_s32 *mon, td_s32 *day, td_s32 *hour)
{
    td_u32 days = epoch / 86400;
    td_u32 rem = epoch % 86400;
    td_s64 z, era, y;
    td_u32 doe, yoe, doy, mp, d, m;

    *hour = (td_s32)(rem / 3600);
    z = (td_s64)days + 719468;
    era = (z >= 0 ? z : z - 146096) / 146097;
    doe = (td_u32)(z - era * 146097);
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = (td_s64)yoe + era * 400;
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp + (mp < 10 ? 3 : -9);

    *year = (td_s32)(y + (m <= 2 ? 1 : 0));
    *mon = (td_s32)m;
    *day = (td_s32)d;
}

td_s32 net_connect(td_void)
{
    td_char ifname[WIFI_IFNAME_MAX_SIZE + 1] = "wlan0";
    wifi_sta_config_stru expected_bss = {0};
    struct netif *netif_p = TD_NULL;
    td_u32 wait_count = 0;
    td_u32 epoch = 0;

    if (wifi_register_event_cb(&g_wifi_event_cb) != 0) {
        osal_printk(NET_LOG "register cb fail\r\n");
        return -1;
    }
    while (wifi_is_wifi_inited() == 0) {
        (void)osDelay(10);
    }
    osal_printk(NET_LOG "wifi inited, ssid=%s\r\n", CFG_WIFI_SSID);

    if (wifi_sta_enable() != 0) {
        osal_printk(NET_LOG "sta enable fail\r\n");
        return -1;
    }
    g_wifi_state = NET_ST_INIT;

    while (g_net_ready == TD_FALSE) {
        (void)osDelay(10);
        if (g_wifi_state == NET_ST_INIT) {
            g_wifi_state = NET_ST_SCANING;
            if (net_sta_scan() != 0) {
                g_wifi_state = NET_ST_INIT;
                (void)osDelay(100);
            }
        } else if (g_wifi_state == NET_ST_SCAN_DONE) {
            if (net_get_match_network(&expected_bss) != 0) {
                osal_printk(NET_LOG "ap not found, rescan\r\n");
                g_wifi_state = NET_ST_INIT;
                (void)osDelay(100);
                continue;
            }
            g_wifi_state = NET_ST_FOUND_TARGET;
        } else if (g_wifi_state == NET_ST_FOUND_TARGET) {
            g_wifi_state = NET_ST_CONNECTING;
            if (wifi_sta_connect(&expected_bss) != 0) {
                g_wifi_state = NET_ST_INIT;
                (void)osDelay(100);
            }
        } else if (g_wifi_state == NET_ST_CONNECT_DONE) {
            g_wifi_state = NET_ST_GET_IP;
            netif_p = netifapi_netif_find(ifname);
            if (netif_p == TD_NULL || netifapi_dhcp_start(netif_p) != 0) {
                osal_printk(NET_LOG "dhcp start fail\r\n");
                g_wifi_state = NET_ST_INIT;
                (void)osDelay(100);
            }
        } else if (g_wifi_state == NET_ST_GET_IP) {
            if (ip_addr_isany(&(netif_p->ip_addr)) == 0) {
                g_net_ready = TD_TRUE;
            } else if (wait_count++ > WIFI_GET_IP_MAX_COUNT) {
                g_wifi_state = NET_ST_INIT;
                wait_count = 0;
            }
            (void)osDelay(50);
        }
    }

    osal_printk(NET_LOG "ip=%s\r\n", ipaddr_ntoa(&(netif_p->ip_addr)));

    for (td_s32 i = 0; i < 5; i++) {
        if (sntp_get_epoch(&epoch) == 0) {
            td_s32 y, m, d, h;
            g_epoch_anchor = epoch;
            g_tick_anchor = osKernelGetTickCount();
            net_epoch_to_utc(epoch, &y, &m, &d, &h);
            osal_printk(NET_LOG "ntp ok, utc %04d-%02d-%02d %02d:xx (epoch=%u)\r\n",
                        y, m, d, h, epoch);
            return 0;
        }
        osal_printk(NET_LOG "ntp retry %d\r\n", i + 1);
        (void)osDelay(200);
    }
    osal_printk(NET_LOG "ntp FAILED（MQTT 仍可连，但时间字段不可信）\r\n");
    return 0;
}

td_u32 net_epoch_now(td_void)
{
    td_u32 ticks;

    if (g_epoch_anchor == 0) {
        return 0;
    }
    ticks = osKernelGetTickCount() - g_tick_anchor;
    return g_epoch_anchor + ticks / TICKS_PER_SEC;
}

td_u32 net_uptime_sec(td_void)
{
    return osKernelGetTickCount() / TICKS_PER_SEC;
}
