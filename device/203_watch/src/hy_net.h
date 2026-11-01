/*
 * hy_net.h —— WiFi STA 联网 + SNTP 取时
 *
 * 华为云 IoTDA 用明文 1883 接入时，平台会强制校验 ClientId 里的时间戳
 * （UTC 的 YYYYMMDDHH），所以板子必须能拿到可信的 UTC 时间。WS63 没有 RTC
 * 备份电池，每次上电都得重新同步一次，这里就用最省事的 SNTP。
 */
#ifndef HY_NET_H
#define HY_NET_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 连接 WiFi 并拿到 IP，成功返回 0。timeout_ms 覆盖整个流程 */
int hy_net_wifi_connect(const char *ssid, const char *pwd, int timeout_ms);

/* 当前是否已关联且有 IP */
int hy_net_is_up(void);

/* 发一次 SNTP 请求，成功返回 0，*utc_sec 为 Unix 时间戳（秒） */
int hy_net_sntp(uint32_t *utc_sec, int timeout_ms);

/* Unix 秒 -> "YYYYMMDDHH"（UTC），buf 至少 11 字节 */
void hy_net_utc_stamp(uint32_t utc_sec, char *buf);

#ifdef __cplusplus
}
#endif

#endif /* HY_NET_H */
