/*
 * hy_mqtt.h —— 最小 MQTT 3.1.1 客户端（QoS 0）
 *
 * SDK 里只有一个 paho.mqtt.c，那是给 Linux/POSIX 用的，线程模型和内存模型
 * 都不适合 LiteOS，编进来也跑不起来。这里按华为云 IoTDA 的实际需要裁一版：
 *   CONNECT / CONNACK / PUBLISH / PINGREQ / PINGRESP / DISCONNECT
 * 只做 QoS 0，属性上报够用了。
 */
#ifndef HY_MQTT_H
#define HY_MQTT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 返回码 */
#define HY_MQTT_OK              0
#define HY_MQTT_ERR_PARAM     (-1)
#define HY_MQTT_ERR_DNS       (-2)
#define HY_MQTT_ERR_SOCKET    (-3)
#define HY_MQTT_ERR_CONNECT   (-4)
#define HY_MQTT_ERR_SEND      (-5)
#define HY_MQTT_ERR_RECV      (-6)
#define HY_MQTT_ERR_AUTH      (-7)   /* CONNACK 里平台拒绝，多半是三元组不对 */
#define HY_MQTT_ERR_BUF       (-8)

typedef struct {
    int      fd;
    int      connected;
    uint16_t keepalive_s;
    uint64_t last_tx_ms;
    int      last_connack;   /* CONNACK 返回码：0 成功，4 用户名/密码错，5 未授权 */
    char     peer[64];
} hy_mqtt_t;

/*
 * 建立 TCP 连接并完成 MQTT CONNECT 握手。
 * 成功返回 HY_MQTT_OK；失败返回负值，m->last_connack 里有平台给的原始码。
 */
int hy_mqtt_connect(hy_mqtt_t *m, const char *host, uint16_t port,
                    const char *client_id, const char *user, const char *pass,
                    uint16_t keepalive_s);

/* 发布一条 QoS 0 消息 */
int hy_mqtt_publish(hy_mqtt_t *m, const char *topic, const char *payload, int len);

/* 发心跳并等 PINGRESP */
int hy_mqtt_ping(hy_mqtt_t *m);

/* 非阻塞探活：0 = 还活着，非 0 = 已断开 */
int hy_mqtt_probe(hy_mqtt_t *m);

void hy_mqtt_close(hy_mqtt_t *m);

#ifdef __cplusplus
}
#endif

#endif /* HY_MQTT_H */
