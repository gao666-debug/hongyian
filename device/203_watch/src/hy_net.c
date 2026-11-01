/*
 * hy_net.c —— WiFi STA 联网 + 极简 SNTP
 *
 * 联网流程照抄 SDK 的 sta_sample（定向扫描 -> 连接 -> DHCP），但去掉了它那套
 * 状态机打印，改成带 deadline 的阻塞流程，方便被云端任务直接调用。
 */
#include "hy_net.h"

#include <string.h>
#include <stdio.h>
#include <errno.h>

#include "lwip/netifapi.h"
#include "lwip/netif.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/dhcp.h"
#include "lwip/pbuf.h"
#include "lwip/udp.h"
#include "lwip/tcp.h"
#include "lwip/mem.h"
#include "lwip/api.h"
#include "lwip/sys.h"
#include "lwip/tcpip.h"

#include "wifi_hotspot.h"
#include "wifi_hotspot_config.h"
#include "wifi_device.h"

#include "soc_osal.h"
#include "tcxo.h"

#define HY_NET_LOG            "[hy-net]"
#define HY_WIFI_IFNAME        "wlan0"
#define HY_WIFI_MAX_SSID_LEN  33
#define HY_WIFI_MAC_LEN       6
#define HY_SCAN_AP_LIMIT      32

/* SNTP 服务器，按顺序尝试；最后一个用 IP，防止 DNS 抽风 */
#define HY_NTP_PORT           123
#define HY_NTP_UNIX_OFFSET    2208988800UL   /* 1900-01-01 到 1970-01-01 的秒数 */

static const char *g_ntp_hosts[] = {
    "ntp.aliyun.com",
    "ntp1.aliyun.com",
    "203.107.6.88",
};

static volatile int g_sta_conn = 0;    /* 1 = 关联成功 */
static volatile int g_ip_ok = 0;       /* 1 = 已拿到 IP，上层只认这个 */
static volatile int g_scan_done = 0;
static int g_wifi_init_done = 0;       /* wifi_init 只许调一次 */
static int g_probed = 0;               /* 探针只打一次 */

static void hy_wifi_conn_changed(int state, const wifi_linked_info_stru *info, int reason_code)
{
    (void)info;
    if (state == 0) {
        printf("%s 连接断开，reason=%d\r\n", HY_NET_LOG, reason_code);
        g_sta_conn = 0;
        g_ip_ok = 0;
    } else {
        g_sta_conn = 1;
    }
}

static void hy_wifi_scan_changed(int state, int size)
{
    (void)state;
    (void)size;
    g_scan_done = 1;
}

static wifi_event_stru g_wifi_evt = {
    .wifi_event_connection_changed = hy_wifi_conn_changed,
    .wifi_event_scan_state_changed = hy_wifi_scan_changed,
};

static uint64_t hy_up_ms(void)
{
    return (uint64_t)uapi_tcxo_get_ms();
}

static int hy_wait_flag(volatile int *flag, uint64_t deadline, int step_ms)
{
    while (*flag == 0) {
        if (hy_up_ms() > deadline) {
            return -1;
        }
        osal_msleep((uint32_t)step_ms);
    }
    return 0;
}

/* 定向扫描目标 SSID，成功填好 bss 返回 0 */
static int hy_wifi_find_ap(const char *ssid, wifi_sta_config_stru *bss, uint64_t deadline)
{
    wifi_scan_params_stru scan;
    wifi_scan_info_stru *res = NULL;
    uint32_t num = HY_SCAN_AP_LIMIT;
    uint32_t len = (uint32_t)sizeof(wifi_scan_info_stru) * HY_SCAN_AP_LIMIT;
    size_t ssid_len = strlen(ssid);
    uint32_t i;
    int ret = -1;

    if (ssid_len > HY_WIFI_MAX_SSID_LEN - 1) {
        return -1;
    }

    memset(&scan, 0, sizeof(scan));
    (void)memcpy(scan.ssid, ssid, ssid_len);
    scan.ssid[ssid_len] = '\0';
    scan.ssid_len = (uint8_t)ssid_len;
    scan.scan_type = WIFI_SSID_SCAN;

    g_scan_done = 0;
    if (wifi_sta_scan_advance(&scan) != 0) {
        printf("%s 发起扫描失败\r\n", HY_NET_LOG);
        return -1;
    }
    if (hy_wait_flag(&g_scan_done, deadline, 100) != 0) {
        printf("%s 扫描超时（热点开着吗？手机热点要设成 2.4GHz）\r\n", HY_NET_LOG);
        return -1;
    }

    res = (wifi_scan_info_stru *)osal_kmalloc(len, OSAL_GFP_ATOMIC);
    if (res == NULL) {
        return -1;
    }
    memset(res, 0, len);

    if (wifi_sta_get_scan_info(res, &num) != 0) {
        osal_kfree(res);
        return -1;
    }

    for (i = 0; i < num; i++) {
        size_t n = strlen(res[i].ssid);

        if (n == ssid_len && memcmp(res[i].ssid, ssid, ssid_len) == 0) {
            (void)memcpy(bss->ssid, ssid, ssid_len);
            bss->ssid[ssid_len] = '\0';
            (void)memcpy(bss->bssid, res[i].bssid, HY_WIFI_MAC_LEN);
            bss->security_type = res[i].security_type;
            ret = 0;
            break;
        }
    }

    if (ret != 0) {
        printf("%s 扫不到 SSID「%s」，共扫到 %u 个热点\r\n", HY_NET_LOG, ssid, (unsigned)num);
        for (i = 0; i < num && i < 8; i++) {
            printf("%s   - %s\r\n", HY_NET_LOG, res[i].ssid);
        }
    }

    osal_kfree(res);
    return ret;
}

/* 把网卡状态打出来，DHCP 失败时靠这行定位 */
static void hy_netif_dump(struct netif *netif_p)
{
    printf("%s netif=%c%c%u mtu=%u up=%d link=%d hwlen=%u flags=0x%04x\r\n",
           HY_NET_LOG, netif_p->name[0], netif_p->name[1], (unsigned)netif_p->num,
           (unsigned)netif_p->mtu, (netif_is_up(netif_p) != 0) ? 1 : 0,
           (netif_is_link_up(netif_p) != 0) ? 1 : 0, (unsigned)netif_p->hwaddr_len,
           (unsigned)netif_p->flags);
    printf("%s mac=%02x:%02x:%02x:%02x:%02x:%02x\r\n", HY_NET_LOG,
           netif_p->hwaddr[0], netif_p->hwaddr[1], netif_p->hwaddr[2],
           netif_p->hwaddr[3], netif_p->hwaddr[4], netif_p->hwaddr[5]);
}

/*
 * 探针：dhcp_start 一直返回 -1(ERR_MEM)，socket 也开不出来(ENOBUFS)。
 * 逐个试 lwip 的各条分配路径，看是堆（malloc）坏了还是内存池（memp）坏了。
 * 返回成功项数，3 以上说明 lwip 是好的。
 */
static int hy_net_probe_run(const char *tag)
{
    struct pbuf *p;
    void *q;
    int fd;
    int ok = 0;

    printf("%s probe[%s] sys_now=%u\r\n", HY_NET_LOG, tag, (unsigned)sys_now());

    /* 1. lwip 堆（MEM_LIBC_MALLOC=1 时最终走 malloc） */
    q = mem_malloc(256);
    printf("%s probe[%s] mem_malloc=%p\r\n", HY_NET_LOG, tag, q);
    if (q != NULL) {
        mem_free(q);
        ok++;
    }

    /* 2. PBUF_RAM 走堆，PBUF_POOL 走 memp */
    p = pbuf_alloc(PBUF_RAW, 64, PBUF_RAM);
    printf("%s probe[%s] pbuf_ram=%p\r\n", HY_NET_LOG, tag, (void *)p);
    if (p != NULL) {
        (void)pbuf_free(p);
        ok++;
    }
    p = pbuf_alloc(PBUF_RAW, 64, PBUF_POOL);
    printf("%s probe[%s] pbuf_pool=%p\r\n", HY_NET_LOG, tag, (void *)p);
    if (p != NULL) {
        (void)pbuf_free(p);
        ok++;
    }

    /* 3. UDP / TCP PCB，dhcp 就是卡在 udp_new 这一步 */
    q = (void *)udp_new();
    printf("%s probe[%s] udp_new=%p\r\n", HY_NET_LOG, tag, q);
    if (q != NULL) {
        udp_remove((struct udp_pcb *)q);
        ok++;
    }
    q = (void *)tcp_new();
    printf("%s probe[%s] tcp_new=%p\r\n", HY_NET_LOG, tag, q);
    if (q != NULL) {
        (void)tcp_close((struct tcp_pcb *)q);
        ok++;
    }

    /* 4. netconn / socket，MQTT 要走这条 */
    q = (void *)netconn_new(NETCONN_UDP);
    printf("%s probe[%s] netconn=%p\r\n", HY_NET_LOG, tag, q);
    if (q != NULL) {
        (void)netconn_delete((struct netconn *)q);
        ok++;
    }

    fd = (int)socket(AF_INET, SOCK_DGRAM, 0);
    printf("%s probe[%s] socket fd=%d errno=%d\r\n", HY_NET_LOG, tag, fd, errno);
    if (fd >= 0) {
        (void)closesocket(fd);
        ok++;
    }

    return ok;
}

/*
 * lwip 的初始化（tcpip_init -> lwip_init -> memp_init）在 SDK 里只有一条路径：
 * wifi_host_task 里 uapi_wifi_init 成功后才会调。这一步一旦没走成，memp 内存池
 * 就全是空的——现象正好是 mem_malloc 正常、pbuf_pool/udp_new/netconn 全 NULL。
 * tcpip_init 自身带完成标志，重复调用是空操作，所以这里可以安全地补一次。
 */
static void hy_net_ensure_lwip(void)
{
    int ok = hy_net_probe_run("A");

    if (ok >= 3) {
        printf("%s lwip 内存池正常（%d/7）\r\n", HY_NET_LOG, ok);
        return;
    }

    printf("%s lwip 内存池不可用（%d/7），补一次 tcpip_init\r\n", HY_NET_LOG, ok);
    tcpip_init(NULL, NULL);
    osal_msleep(1000);

    ok = hy_net_probe_run("B");
    printf("%s 补初始化后 %d/7\r\n", HY_NET_LOG, ok);
}

/* 等网卡 up、启 DHCP、等 IP。关联已经建立但 IP 没拿到时也可以单独调 */
static int hy_wifi_wait_ip(uint64_t deadline)
{
    struct netif *netif_p;
    uint32_t i;
    int ok = 0;

    netif_p = netifapi_netif_find(HY_WIFI_IFNAME);
    if (netif_p == NULL) {
        printf("%s 找不到网卡 %s\r\n", HY_NET_LOG, HY_WIFI_IFNAME);
        return -1;
    }

    /*
     * 关联成功到网卡 link up 之间还有几十到几百毫秒，这时候直接
     * netifapi_dhcp_start 会失败，必须先等 link up。
     */
    for (i = 0; i < 20; i++) {
        if (netif_is_link_up(netif_p) != 0) {
            break;
        }
        osal_msleep(100);
    }

    hy_netif_dump(netif_p);

    if (g_probed == 0) {
        g_probed = 1;
        hy_net_ensure_lwip();
    }

    /*
     * 关键：网卡的 MTU 由底层驱动设置，实测 WS63 这颗网卡一直是 0，而
     * lwip 的 dhcp_start() 一进门就检查 netif->mtu < 576，不满足直接
     * return ERR_MEM(-1)——这就是一直 dhcp_start ret=-1 的原因。
     */
    if (netif_p->mtu < 576) {
        u16_t old_mtu = netif_p->mtu;
        err_t e = netifapi_netif_set_mtu(netif_p, 1500);

        printf("%s mtu %u -> %u, ret=%d\r\n", HY_NET_LOG, (unsigned)old_mtu,
               (unsigned)netif_p->mtu, (int)e);
    }

    if (netif_is_up(netif_p) == 0) {
        printf("%s 网卡没 up，手动拉起\r\n", HY_NET_LOG);
        (void)netifapi_netif_set_up(netif_p);
    }

    /* 公网报文要靠默认网卡选路，不设的话连华为云会 ERR_RTE */
    (void)netifapi_netif_set_default(netif_p);

    /* DHCP 可能在驱动内部已经跑起来了，失败就重试几次 */
    for (i = 0; i < 5; i++) {
        err_t e = netifapi_dhcp_start(netif_p);

        printf("%s dhcp_start ret=%d\r\n", HY_NET_LOG, (int)e);
        if (e == ERR_OK || e == ERR_USE) {   /* ERR_USE = 已经在跑了，不算失败 */
            ok = 1;
            break;
        }
        osal_msleep(800);
    }

    if (ok == 0) {
        /* 再兜一次底：有的版本 mtu 要等 link up 之后才生效，重设后重启 DHCP */
        (void)netifapi_netif_set_mtu(netif_p, 1500);
        ok = (netifapi_dhcp_start(netif_p) == ERR_OK) ? 1 : 0;
        printf("%s dhcp 兜底重试 ok=%d\r\n", HY_NET_LOG, ok);
    }

    while (ip_addr_isany(&netif_p->ip_addr) != 0) {
        if (hy_up_ms() > deadline) {
            printf("%s DHCP 超时（关联上了但没分到 IP）\r\n", HY_NET_LOG);
            g_ip_ok = 0;
            return -1;
        }
        osal_msleep(100);
    }

    printf("%s 拿到 IP: %s gw=%s\r\n", HY_NET_LOG, ipaddr_ntoa(&netif_p->ip_addr),
           ipaddr_ntoa(&netif_p->gw));
    g_ip_ok = 1;
    return 0;
}

int hy_net_wifi_connect(const char *ssid, const char *pwd, int timeout_ms)
{
    wifi_sta_config_stru bss;
    uint64_t deadline = hy_up_ms() + (uint64_t)timeout_ms;
    size_t pwd_len;
    int ret;

    if (ssid == NULL || pwd == NULL || ssid[0] == '\0') {
        printf("%s SSID 未配置\r\n", HY_NET_LOG);
        return -1;
    }

    pwd_len = strlen(pwd);
    if (pwd_len > HY_WIFI_MAX_SSID_LEN - 1) {
        return -1;
    }

    /* 上一轮已经关联上了、只是没拿到 IP：别再扫一遍，直接补 DHCP */
    if (g_sta_conn != 0) {
        printf("%s 已关联到 AP，直接补 DHCP\r\n", HY_NET_LOG);
        return hy_wifi_wait_ip(deadline);
    }

    printf("%s 连接 WiFi「%s」...\r\n", HY_NET_LOG, ssid);

    if (wifi_register_event_cb(&g_wifi_evt) != 0) {
        printf("%s 事件回调注册失败（可能已注册过，继续）\r\n", HY_NET_LOG);
    }

    /*
     * 注意：SDK 正常启动流程里并不会初始化 WiFi 协议栈（wifi_init 只在低功耗
     * 唤醒的恢复流程里被调用），所以必须由应用自己拉起来，否则
     * wifi_is_wifi_inited() 会一直是 0。
     */
    if (g_wifi_init_done == 0) {
        if (wifi_is_wifi_inited() == 0) {
            printf("%s 初始化 WiFi 协议栈...\r\n", HY_NET_LOG);
            if (wifi_init() != 0) {
                printf("%s wifi_init 失败（内存不足？）\r\n", HY_NET_LOG);
                return -1;
            }
        }
        g_wifi_init_done = 1;
    }

    while (wifi_is_wifi_inited() == 0) {
        if (hy_up_ms() > deadline) {
            printf("%s WiFi 协议栈初始化超时\r\n", HY_NET_LOG);
            return -1;
        }
        osal_msleep(100);
    }
    printf("%s WiFi 协议栈就绪\r\n", HY_NET_LOG);

    if (wifi_sta_enable() != 0) {
        printf("%s STA 使能返回失败（可能已使能，继续试）\r\n", HY_NET_LOG);
    }

    memset(&bss, 0, sizeof(bss));
    if (hy_wifi_find_ap(ssid, &bss, deadline) != 0) {
        return -1;
    }
    printf("%s 已扫到目标 AP，security_type=%d，开始连接\r\n", HY_NET_LOG, (int)bss.security_type);

    (void)memcpy(bss.pre_shared_key, pwd, pwd_len);
    bss.pre_shared_key[pwd_len] = '\0';
    bss.ip_type = 1;    /* DHCP */

    g_sta_conn = 0;
    ret = wifi_sta_connect(&bss);
    if (ret != 0) {
        printf("%s 连接请求失败 ret=%d\r\n", HY_NET_LOG, ret);
        return -1;
    }
    if (hy_wait_flag(&g_sta_conn, deadline, 100) != 0) {
        printf("%s 关联超时（密码对不对？）\r\n", HY_NET_LOG);
        return -1;
    }
    printf("%s 关联成功，开始 DHCP\r\n", HY_NET_LOG);

    return hy_wifi_wait_ip(deadline);
}

int hy_net_is_up(void)
{
    /* 只认"关联 + 有 IP"，光关联上但没 IP 上层什么也干不了 */
    return g_ip_ok;
}

static uint32_t hy_ntohl32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static int hy_sntp_query(const char *host, uint32_t *utc_sec, int timeout_ms)
{
    struct sockaddr_in addr;
    struct timeval tv;
    uint8_t pkt[48];
    int fd;
    int n;
    int ret = -1;

    fd = (int)socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        return -1;
    }

    tv.tv_sec = (long)(timeout_ms / 1000);
    tv.tv_usec = (long)((timeout_ms % 1000) * 1000);
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    memset(&addr, 0, sizeof(addr));

    /* 域名和 IP 都支持，统一走 getaddrinfo */
    {
        struct addrinfo hints;
        struct addrinfo *ai = NULL;

        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;

        if (getaddrinfo(host, "123", &hints, &ai) != 0 || ai == NULL) {
            (void)closesocket(fd);
            return -1;
        }
        memcpy(&addr, ai->ai_addr, sizeof(addr));
        freeaddrinfo(ai);
    }

    memset(pkt, 0, sizeof(pkt));
    pkt[0] = 0x1B;    /* LI=0, VN=3, Mode=3(client) */

    if (sendto(fd, pkt, sizeof(pkt), 0, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        (void)closesocket(fd);
        return -1;
    }

    n = (int)recvfrom(fd, pkt, sizeof(pkt), 0, NULL, NULL);
    (void)closesocket(fd);
    if (n < 48) {
        return -1;
    }

    {
        uint32_t sec = hy_ntohl32(&pkt[40]);
        if (sec < HY_NTP_UNIX_OFFSET) {
            return -1;
        }
        *utc_sec = sec - HY_NTP_UNIX_OFFSET;
        ret = 0;
    }

    return ret;
}

int hy_net_sntp(uint32_t *utc_sec, int timeout_ms)
{
    uint32_t i;

    if (utc_sec == NULL) {
        return -1;
    }

    for (i = 0; i < sizeof(g_ntp_hosts) / sizeof(g_ntp_hosts[0]); i++) {
        if (hy_sntp_query(g_ntp_hosts[i], utc_sec, timeout_ms) == 0) {
            return 0;
        }
    }

    return -1;
}

void hy_net_utc_stamp(uint32_t utc_sec, char *buf)
{
    static const uint8_t day_tab[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    uint32_t days = utc_sec / 86400U;
    uint32_t rem = utc_sec % 86400U;
    int year = 1970;
    int mon = 0;
    int day;

    /* 逐年扣，1970~2100 最多 130 次，无所谓 */
    for (;;) {
        uint32_t ydays = 365U;
        if ((year % 4 == 0 && year % 100 != 0) || (year % 400 == 0)) {
            ydays = 366U;
        }
        if (days < ydays) {
            break;
        }
        days -= ydays;
        year++;
    }

    for (mon = 0; mon < 12; mon++) {
        uint32_t mdays = day_tab[mon];
        if (mon == 1 && ((year % 4 == 0 && year % 100 != 0) || (year % 400 == 0))) {
            mdays = 29;
        }
        if (days < mdays) {
            break;
        }
        days -= mdays;
    }

    day = (int)days + 1;
    (void)snprintf(buf, 11, "%04d%02d%02d%02d", year, mon + 1, day, (int)(rem / 3600U));
}
