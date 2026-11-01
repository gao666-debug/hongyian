/*
 * hy_mqtt.c —— 最小 MQTT 3.1.1 客户端实现（lwip socket）
 */
#include "hy_mqtt.h"

#include <string.h>
#include <stdio.h>
#include <errno.h>

#include "lwip/sockets.h"
#include "lwip/netdb.h"

#include "soc_osal.h"
#include "tcxo.h"

#define HY_MQTT_TX_BUF        1024
#define HY_MQTT_RX_BUF        256
#define HY_MQTT_CONN_TIMEOUT  8000    /* ms，等 CONNACK */
#define HY_MQTT_TX_TIMEOUT    5000
#define HY_MQTT_PRE           6       /* 固定头预留区：1 + 最多 4 字节剩余长度 */

/* 固定头类型 */
#define HY_MQTT_PKT_CONNECT    0x10
#define HY_MQTT_PKT_CONNACK    0x20
#define HY_MQTT_PKT_PUBLISH    0x30
#define HY_MQTT_PKT_PINGREQ    0xC0
#define HY_MQTT_PKT_PINGRESP   0xD0
#define HY_MQTT_PKT_DISCONNECT 0xE0

static uint8_t s_tx[HY_MQTT_TX_BUF];
static uint8_t s_rx[HY_MQTT_RX_BUF];

static uint64_t hy_now_ms(void)
{
    return (uint64_t)uapi_tcxo_get_ms();
}

/* 剩余长度编码，返回占用字节数 */
static int hy_mqtt_remlen(uint8_t *buf, int len)
{
    int i = 0;
    uint32_t v = (uint32_t)len;

    do {
        uint8_t d = (uint8_t)(v % 128U);
        v /= 128U;
        if (v > 0U) {
            d = (uint8_t)(d | 0x80U);
        }
        buf[i] = d;
        i++;
    } while (v > 0U && i < 4);

    return i;
}

static int hy_send_all(int fd, const uint8_t *buf, int len)
{
    int sent = 0;

    while (sent < len) {
        int n = (int)send(fd, &buf[sent], (size_t)(len - sent), 0);
        if (n <= 0) {
            return HY_MQTT_ERR_SEND;
        }
        sent += n;
    }

    return HY_MQTT_OK;
}

/* 读满 n 字节，超时返回 HY_MQTT_ERR_RECV */
static int hy_recv_exact(int fd, uint8_t *buf, int n, int timeout_ms)
{
    int got = 0;
    uint64_t t0 = hy_now_ms();

    while (got < n) {
        int r = (int)recv(fd, &buf[got], (size_t)(n - got), 0);
        if (r > 0) {
            got += r;
            continue;
        }
        if (r == 0) {
            return HY_MQTT_ERR_RECV;    /* 对端关闭 */
        }
        if (errno != EWOULDBLOCK && errno != EINTR) {   /* EAGAIN 与 EWOULDBLOCK 同值 */
            return HY_MQTT_ERR_RECV;
        }
        if ((hy_now_ms() - t0) > (uint64_t)timeout_ms) {
            return HY_MQTT_ERR_RECV;
        }
        osal_msleep(10);
    }

    return HY_MQTT_OK;
}

static void hy_sock_set_timeout(int fd, int ms)
{
    struct timeval tv;

    tv.tv_sec = (long)(ms / 1000);
    tv.tv_usec = (long)((ms % 1000) * 1000);
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

static int hy_tcp_open(const char *host, uint16_t port)
{
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    struct addrinfo *p = NULL;
    char port_str[8];
    int fd = -1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    (void)snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);

    if (getaddrinfo(host, port_str, &hints, &res) != 0 || res == NULL) {
        printf("[hy-mqtt] DNS 解析失败: %s\r\n", host);
        return HY_MQTT_ERR_DNS;
    }

    for (p = res; p != NULL; p = p->ai_next) {
        fd = (int)socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) {
            continue;
        }
        hy_sock_set_timeout(fd, HY_MQTT_TX_TIMEOUT);
        if (connect(fd, p->ai_addr, (socklen_t)p->ai_addrlen) == 0) {
            break;
        }
        (void)closesocket(fd);
        fd = -1;
    }

    freeaddrinfo(res);

    if (fd < 0) {
        printf("[hy-mqtt] TCP 连接失败: %s:%u\r\n", host, (unsigned)port);
        return HY_MQTT_ERR_CONNECT;
    }

    return fd;
}

/* 写一个 2 字节长度前缀的字符串，返回写入字节数 */
static int hy_put_str(uint8_t *buf, const char *s)
{
    size_t n = strlen(s);

    buf[0] = (uint8_t)((n >> 8) & 0xFF);
    buf[1] = (uint8_t)(n & 0xFF);
    memcpy(&buf[2], s, n);

    return (int)(n + 2);
}

int hy_mqtt_connect(hy_mqtt_t *m, const char *host, uint16_t port,
                    const char *client_id, const char *user, const char *pass,
                    uint16_t keepalive_s)
{
    int pos;
    int body_len;
    int hl;
    int fd;
    uint8_t flags = 0x02;    /* clean session */
    uint8_t ack[4];
    uint8_t tmp[4];

    if (m == NULL || host == NULL || client_id == NULL) {
        return HY_MQTT_ERR_PARAM;
    }

    memset(m, 0, sizeof(*m));
    m->fd = -1;

    fd = hy_tcp_open(host, port);
    if (fd < 0) {
        return fd;
    }

    m->fd = fd;
    m->keepalive_s = keepalive_s;
    (void)snprintf(m->peer, sizeof(m->peer), "%s:%u", host, (unsigned)port);
    hy_sock_set_timeout(fd, HY_MQTT_CONN_TIMEOUT);

    /* ---- 可变头（先写在预留区之后） ---- */
    pos = HY_MQTT_PRE;
    s_tx[pos++] = 0x00;
    s_tx[pos++] = 0x04;
    s_tx[pos++] = 'M';
    s_tx[pos++] = 'Q';
    s_tx[pos++] = 'T';
    s_tx[pos++] = 'T';
    s_tx[pos++] = 0x04;      /* protocol level 3.1.1 */
    s_tx[pos++] = 0x02;      /* 连接标志，下面会回填 */
    s_tx[pos++] = (uint8_t)((keepalive_s >> 8) & 0xFF);
    s_tx[pos++] = (uint8_t)(keepalive_s & 0xFF);

    if (user != NULL && user[0] != '\0') {
        flags = (uint8_t)(flags | 0x80);
        if (pass != NULL && pass[0] != '\0') {
            flags = (uint8_t)(flags | 0x40);
        }
    }
    s_tx[HY_MQTT_PRE + 7] = flags;

    /* ---- payload ---- */
    pos += hy_put_str(&s_tx[pos], client_id);
    if ((flags & 0x80) != 0) {
        pos += hy_put_str(&s_tx[pos], user);
        if ((flags & 0x40) != 0) {
            pos += hy_put_str(&s_tx[pos], pass);
        }
    }

    body_len = pos - HY_MQTT_PRE;
    if (body_len + 5 > HY_MQTT_TX_BUF) {
        hy_mqtt_close(m);
        return HY_MQTT_ERR_BUF;
    }

    /* ---- 前移，补上固定头 ---- */
    hl = hy_mqtt_remlen(tmp, body_len);
    memmove(&s_tx[1 + hl], &s_tx[HY_MQTT_PRE], (size_t)body_len);
    s_tx[0] = HY_MQTT_PKT_CONNECT;
    {
        int i;
        for (i = 0; i < hl; i++) {
            s_tx[1 + i] = tmp[i];
        }
    }

    m->last_tx_ms = hy_now_ms();
    if (hy_send_all(fd, s_tx, body_len + 1 + hl) != HY_MQTT_OK) {
        hy_mqtt_close(m);
        return HY_MQTT_ERR_SEND;
    }

    /* ---- CONNACK ---- */
    if (hy_recv_exact(fd, ack, 4, HY_MQTT_CONN_TIMEOUT) != HY_MQTT_OK) {
        printf("[hy-mqtt] 等 CONNACK 超时\r\n");
        hy_mqtt_close(m);
        return HY_MQTT_ERR_RECV;
    }

    if (ack[0] != HY_MQTT_PKT_CONNACK) {
        printf("[hy-mqtt] CONNACK 类型异常: 0x%02x\r\n", ack[0]);
        hy_mqtt_close(m);
        return HY_MQTT_ERR_RECV;
    }

    m->last_connack = ack[3];
    if (ack[3] != 0) {
        printf("[hy-mqtt] 平台拒绝连接，auth_code=%u\r\n", (unsigned)ack[3]);
        hy_mqtt_close(m);
        return HY_MQTT_ERR_AUTH;
    }

    m->connected = 1;
    hy_sock_set_timeout(fd, 1000);

    return HY_MQTT_OK;
}

int hy_mqtt_publish(hy_mqtt_t *m, const char *topic, const char *payload, int len)
{
    int tlen;
    int body;
    int hl;
    int i;
    uint8_t tmp[4];

    if (m == NULL || m->fd < 0 || topic == NULL || payload == NULL || len < 0) {
        return HY_MQTT_ERR_PARAM;
    }

    tlen = (int)strlen(topic);
    body = 2 + tlen + len;
    hl = hy_mqtt_remlen(tmp, body);

    if ((body + hl + 1) > HY_MQTT_TX_BUF) {
        return HY_MQTT_ERR_BUF;
    }

    s_tx[0] = HY_MQTT_PKT_PUBLISH;
    for (i = 0; i < hl; i++) {
        s_tx[1 + i] = tmp[i];
    }
    i = 1 + hl;

    s_tx[i] = (uint8_t)((tlen >> 8) & 0xFF);
    s_tx[i + 1] = (uint8_t)(tlen & 0xFF);
    memcpy(&s_tx[i + 2], topic, (size_t)tlen);
    memcpy(&s_tx[i + 2 + tlen], payload, (size_t)len);

    if (hy_send_all(m->fd, s_tx, i + 2 + tlen + len) != HY_MQTT_OK) {
        m->connected = 0;
        return HY_MQTT_ERR_SEND;
    }
    m->last_tx_ms = hy_now_ms();

    return HY_MQTT_OK;
}

int hy_mqtt_ping(hy_mqtt_t *m)
{
    uint8_t req[2];
    uint8_t resp[2];

    if (m == NULL || m->fd < 0) {
        return HY_MQTT_ERR_PARAM;
    }

    req[0] = HY_MQTT_PKT_PINGREQ;
    req[1] = 0x00;

    if (hy_send_all(m->fd, req, 2) != HY_MQTT_OK) {
        m->connected = 0;
        return HY_MQTT_ERR_SEND;
    }
    m->last_tx_ms = hy_now_ms();

    if (hy_recv_exact(m->fd, resp, 2, 5000) != HY_MQTT_OK) {
        m->connected = 0;
        return HY_MQTT_ERR_RECV;
    }

    if (resp[0] != HY_MQTT_PKT_PINGRESP) {
        m->connected = 0;
        return HY_MQTT_ERR_RECV;
    }

    return HY_MQTT_OK;
}

int hy_mqtt_probe(hy_mqtt_t *m)
{
    int r;
    int i;

    if (m == NULL || m->fd < 0 || m->connected == 0) {
        return -1;
    }

    r = (int)recv(m->fd, s_rx, sizeof(s_rx) - 1, 0);
    if (r == 0) {
        m->connected = 0;
        return -1;
    }
    if (r < 0) {
        if (errno == EWOULDBLOCK) {   /* EAGAIN 与 EWOULDBLOCK 同值 */
            return 0;    /* 没数据，正常 */
        }
        m->connected = 0;
        return -1;
    }

    /*
     * 真有下行数据时，多半是平台在报错（属性名对不上、服务 ID 不存在等）。
     * 把原始报文打出来比猜有用得多，不可打印字符统一换成点。
     */
    s_rx[r] = '\0';
    for (i = 0; i < r; i++) {
        if (s_rx[i] < 0x20 || s_rx[i] > 0x7E) {
            s_rx[i] = (uint8_t)'.';
        }
    }
    printf("[hy-mqtt] 平台下行 %d 字节: %s\r\n", r, (char *)s_rx);

    return 0;
}

void hy_mqtt_close(hy_mqtt_t *m)
{
    if (m == NULL) {
        return;
    }

    if (m->fd >= 0) {
        uint8_t bye[2];

        bye[0] = HY_MQTT_PKT_DISCONNECT;
        bye[1] = 0x00;
        (void)send(m->fd, bye, 2, 0);
        (void)closesocket(m->fd);
        m->fd = -1;
    }
    m->connected = 0;
}
