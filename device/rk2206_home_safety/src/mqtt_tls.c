/* SPDX-License-Identifier: Apache-2.0 */
#include "mqtt_tls.h"
#include "iotda_ca.h"
#include "sntp_client.h"
#include "mbedtls/ssl.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/platform_util.h"
#include "lz_hardware/rand.h"
#include "los_tick.h"
#include "cmsis_os2.h"
#include "los_task.h"
#ifndef SAFETY_HOST_TEST
#include "rk2206.h"
#endif
#include <time.h>
#include "../../../../../../third_party/musl/include/errno.h"
static mbedtls_ssl_context ssl;
static mbedtls_ssl_config config;
static mbedtls_ctr_drbg_context tls_random;
static mbedtls_x509_crt ca;
static int initialized;
static unsigned int tls_rx_bytes, tls_tx_bytes;
static int tls_read_wait_ms = 2000;
static uint64_t now_ms(void) { return LOS_TickCountGet() * 1000ULL / osKernelGetTickFreq(); }
static int entropy_chunk(unsigned char *out, size_t len)
{
#ifdef SAFETY_HOST_TEST
    return GetRandData(out, len);
#else
    /* Same TRNG configuration as the SDK HAL, with explicit clock setup and timeout. */
    uint32_t gates7 = CRU_CLKGATE_CON07_ACLK_CRYPTO_EN_MASK | CRU_CLKGATE_CON07_HCLK_CRYPTO_EN_MASK;
    uint32_t gates8 = CRU_CLKGATE_CON08_CLK_CRYPTO_DIV_EN_MASK | CRU_CLKGATE_CON08_CLK_CRYPTO_NP5_DIV_EN_MASK | CRU_CLKGATE_CON08_CLK_CRYPTO_EN_MASK;
    CRU->CRU_CLKGATE_CON[7] = gates7 << 16;
    CRU->CRU_CLKGATE_CON[8] = gates8 << 16;
    CRYPTO->RNG_SAMPLE_CNT = 100;
    CRYPTO->RNG_CTL = 0xFFFF0037U;
    uint64_t deadline = now_ms() + 200;
    while (CRYPTO->RNG_CTL & CRYPTO_RNG_CTL_RNG_START_MASK) {
        if (now_ms() >= deadline) {
            printf("[iotda] RNG timeout control=0x%lx\r\n", (unsigned long)CRYPTO->RNG_CTL);
            CRYPTO->RNG_CTL = (CRYPTO_RNG_CTL_RNG_START_MASK | CRYPTO_RNG_CTL_RNG_ENABLE_MASK) << 16;
            return 1;
        }
        LOS_Msleep(1);
    }
    uint32_t data[8], nonzero = 0;
    for (int i = 0; i < 8; ++i) { data[i] = CRYPTO->RNG_DOUT[i]; nonzero |= data[i]; }
    CRYPTO->RNG_CTL = (CRYPTO_RNG_CTL_RNG_START_MASK | CRYPTO_RNG_CTL_RNG_ENABLE_MASK) << 16;
    if (!nonzero) { mbedtls_platform_zeroize(data, sizeof(data)); return 2; }
    memcpy(out, data, len);
    mbedtls_platform_zeroize(data, sizeof(data));
    return 0;
#endif
}
static int entropy(void *arg, unsigned char *out, size_t len)
{
    (void)arg;
    /* RK2206 HAL_CRYPTO_Trng rejects single requests above 32 bytes. */
    uint32_t block[8];
    size_t offset = 0;
    while (offset < len) {
        size_t chunk = len - offset;
        if (chunk > sizeof(block)) chunk = sizeof(block);
        unsigned int rc = entropy_chunk((unsigned char *)block, chunk);
        if (rc != 0) {
            mbedtls_platform_zeroize(block, sizeof(block));
            mbedtls_platform_zeroize(out, offset);
            printf("[iotda] RNG failed rc=%u chunk=%u\r\n", rc, (unsigned int)chunk);
            return MBEDTLS_ERR_CTR_DRBG_ENTROPY_SOURCE_FAILED;
        }
        memcpy(out + offset, block, chunk);
        offset += chunk;
    }
    mbedtls_platform_zeroize(block, sizeof(block));
    return 0;
}
static int bio_send(void *ctx, const unsigned char *buf, size_t len)
{
    Network *n = ctx;
    struct timeval tv = {2, 0};
    setsockopt(n->my_socket, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    int rc = send(n->my_socket, buf, len, 0);
    if (rc >= 0) { tls_tx_bytes += rc; return rc; }
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return MBEDTLS_ERR_SSL_WANT_WRITE;
    return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
}
static int bio_recv(void *ctx, unsigned char *buf, size_t len)
{
    Network *n = ctx;
    fd_set readable;
    FD_ZERO(&readable); FD_SET(n->my_socket, &readable);
    struct timeval tv = {tls_read_wait_ms / 1000, (tls_read_wait_ms % 1000) * 1000};
    int ready = select(n->my_socket + 1, &readable, NULL, NULL, &tv);
    if (ready == 0) return MBEDTLS_ERR_SSL_WANT_READ;
    if (ready < 0) return errno == EINTR ? MBEDTLS_ERR_SSL_WANT_READ : MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    int rc = recv(n->my_socket, buf, len, 0);
    if (rc > 0) { tls_rx_bytes += rc; return rc; }
    if (rc == 0) {
        printf("[iotda] TLS peer closed state=%d rx=%u tx=%u\r\n", ssl.state, tls_rx_bytes, tls_tx_bytes);
        return 0;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return MBEDTLS_ERR_SSL_WANT_READ;
    return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
}
static uint64_t date_key(int year, int month, int day, int hour, int minute, int second)
{
    return (((((uint64_t)year * 13 + month) * 32 + day) * 24 + hour) * 60 + minute) * 60 + second;
}
/* LiteOS mbedTLS config omits HAVE_TIME_DATE: explicitly validate all chain dates. */
static int verify_dates(void *arg, mbedtls_x509_crt *crt, int depth, uint32_t *flags)
{
    (void)arg; (void)depth;
    time_t epoch = sntp_get_epoch();
    struct tm utc;
    if (!epoch || !gmtime_r(&epoch, &utc)) { *flags |= MBEDTLS_X509_BADCERT_OTHER; return 0; }
    uint64_t current = date_key(utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec);
    mbedtls_x509_time *from = &crt->valid_from, *to = &crt->valid_to;
    if (current < date_key(from->year, from->mon, from->day, from->hour, from->min, from->sec))
        *flags |= MBEDTLS_X509_BADCERT_FUTURE;
    if (current > date_key(to->year, to->mon, to->day, to->hour, to->min, to->sec))
        *flags |= MBEDTLS_X509_BADCERT_EXPIRED;
    return 0;
}
static int tls_read(Network *n, unsigned char *buf, int len, int timeout)
{
    uint64_t end = now_ms() + (timeout > 0 ? timeout : 1);
    int done = 0;
    while (done < len) {
        uint64_t now = now_ms();
        if (now >= end) return done ? -1 : 0;
        int left = end - now;
        tls_read_wait_ms = left;
        struct timeval tv = {left / 1000, (left % 1000) * 1000};
        setsockopt(n->my_socket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        int rc = mbedtls_ssl_read(&ssl, buf + done, len - done);
        if (rc > 0) { done += rc; continue; }
        if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        return -1;
    }
    return done;
}
static int tls_write(Network *n, unsigned char *buf, int len, int timeout)
{
    (void)n;
    uint64_t end = now_ms() + (timeout > 0 ? timeout : 1);
    int rc;
    do {
        rc = mbedtls_ssl_write(&ssl, buf, len);
        if (rc != MBEDTLS_ERR_SSL_WANT_WRITE && rc != MBEDTLS_ERR_SSL_WANT_READ) return rc;
    } while (now_ms() < end);
    return -1;
}
void safety_tls_close(void)
{
    if (!initialized) return;
    mbedtls_ssl_free(&ssl);
    mbedtls_ssl_config_free(&config);
    mbedtls_x509_crt_free(&ca);
    mbedtls_ctr_drbg_free(&tls_random);
    initialized = 0;
}
int safety_tls_connect(Network *n, const char *host)
{
    safety_tls_close();
    mbedtls_ssl_init(&ssl); mbedtls_ssl_config_init(&config);
    mbedtls_x509_crt_init(&ca); mbedtls_ctr_drbg_init(&tls_random);
    initialized = 1;
    tls_rx_bytes = tls_tx_bytes = 0;
    tls_read_wait_ms = 2000;
    const char *stage = "rng";
    printf("[iotda] TLS stage=rng\r\n");
    int rc = mbedtls_ctr_drbg_seed(&tls_random, entropy, NULL, (const unsigned char *)"home-safety", 11);
    if (rc) goto failed;
    stage = "ca-parse";
    printf("[iotda] TLS stage=ca-parse\r\n");
    rc = mbedtls_x509_crt_parse(&ca, iotda_ca, sizeof(iotda_ca));
    if (rc) goto failed;
    stage = "config";
    printf("[iotda] TLS stage=config\r\n");
    rc = mbedtls_ssl_config_defaults(&config, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    if (rc) goto failed;
    static const mbedtls_ecp_group_id curves[] = {MBEDTLS_ECP_DP_SECP256R1, MBEDTLS_ECP_DP_NONE};
    mbedtls_ssl_conf_curves(&config, curves);
#if defined(MBEDTLS_ECP_RESTARTABLE)
    /* Library-wide ECC slice budget; the MQTT task owns this TLS context. */
    mbedtls_ecp_set_max_ops(128);
#endif
    mbedtls_ssl_conf_authmode(&config, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&config, &ca, NULL);
    mbedtls_ssl_conf_rng(&config, mbedtls_ctr_drbg_random, &tls_random);
    mbedtls_ssl_conf_verify(&config, verify_dates, NULL);
    mbedtls_ssl_conf_min_version(&config, MBEDTLS_SSL_MAJOR_VERSION_3, MBEDTLS_SSL_MINOR_VERSION_3);
    stage = "setup";
    printf("[iotda] TLS stage=setup\r\n");
    rc = mbedtls_ssl_setup(&ssl, &config);
    if (rc) goto failed;
    stage = "hostname";
    printf("[iotda] TLS stage=hostname\r\n");
    rc = mbedtls_ssl_set_hostname(&ssl, host);
    if (rc) goto failed;
    mbedtls_ssl_set_bio(&ssl, n, bio_send, bio_recv, NULL);
    struct timeval tv = {2, 0};
    setsockopt(n->my_socket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    stage = "handshake";
    printf("[iotda] TLS stage=handshake\r\n");
    /* Real RK2206 logs show steady ECC progress exceeding 30 seconds.
     * Keep a bounded deadline while allowing the slow MCU to finish. */
    uint64_t start = now_ms(), end = start + 60000;
    int previous_state = -1;
    unsigned int crypto_yields = 0;
    uint64_t next_progress = start + 5000;
    while (ssl.state != MBEDTLS_SSL_HANDSHAKE_OVER) {
        if (now_ms() >= end) { rc = MBEDTLS_ERR_SSL_TIMEOUT; goto failed; }
        if (previous_state != ssl.state) {
            printf("[iotda] TLS handshake state=%d elapsed=%u ms rx=%u tx=%u\r\n",
                   ssl.state, (unsigned int)(now_ms() - start), tls_rx_bytes, tls_tx_bytes);
            previous_state = ssl.state;
        }
        rc = mbedtls_ssl_handshake_step(&ssl);
        if (rc != 0 && rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE
#if defined(MBEDTLS_ECP_RESTARTABLE)
            && rc != MBEDTLS_ERR_SSL_CRYPTO_IN_PROGRESS
#endif
        ) goto failed;
#if defined(MBEDTLS_ECP_RESTARTABLE)
        if (rc == MBEDTLS_ERR_SSL_CRYPTO_IN_PROGRESS) ++crypto_yields;
#endif
        if (now_ms() >= next_progress) {
            printf("[iotda] TLS progress state=%d elapsed=%u ms crypto_yields=%u rx=%u tx=%u\r\n",
                   ssl.state, (unsigned int)(now_ms() - start), crypto_yields, tls_rx_bytes, tls_tx_bytes);
            next_progress = now_ms() + 5000;
        }
        LOS_Msleep(1);
    }
    if (mbedtls_ssl_get_verify_result(&ssl) != 0) { rc = -1; goto failed; }
    n->mqttread = tls_read; n->mqttwrite = tls_write;
    printf("[iotda] TLS verified certificate/hostname/date crypto_yields=%u\r\n", crypto_yields);
    return 0;
failed:
    printf("[iotda] TLS failed stage=%s rc=%d verify=0x%lx\r\n", stage, rc, (unsigned long)mbedtls_ssl_get_verify_result(&ssl));
    safety_tls_close();
    return -1;
}
