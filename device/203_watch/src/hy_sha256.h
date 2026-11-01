/*
 * hy_sha256.h —— 最小 SHA-256 / HMAC-SHA256
 *
 * 华为云 IoTDA 的设备鉴权是：
 *     password = hex( HMAC-SHA256(key = timestamp(YYYYMMDDHH), msg = device_secret) )
 * 只需要这一个能力，没必要把 mbedtls 整个拖进来（会多占几十 KB flash，
 * 而且 mbedtls 在 LiteOS 上还要配 entropy / time 回调，收益为零）。
 */
#ifndef HY_SHA256_H
#define HY_SHA256_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 单次 SHA-256，out 固定 32 字节 */
void hy_sha256(const uint8_t *data, uint32_t len, uint8_t out[32]);

/* HMAC-SHA256，out 固定 32 字节 */
void hy_hmac_sha256(const uint8_t *key, uint32_t key_len,
                    const uint8_t *msg, uint32_t msg_len,
                    uint8_t out[32]);

/* 给小写十六进制字符串，out 需要 2 * len + 1 字节 */
void hy_hex_encode(const uint8_t *in, uint32_t len, char *out);

#ifdef __cplusplus
}
#endif

#endif /* HY_SHA256_H */
