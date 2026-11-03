/* SPDX-License-Identifier: Apache-2.0 */
#define SAFETY_REAL_TLS_TEST
#define main other_tests_main
#include "test_safety.c"
#undef main
#include "../src/mqtt_tls.c"
#include <fcntl.h>
static int rng_calls, rng_fail_call;
unsigned int GetRandData(unsigned char *out, unsigned int n)
{
    assert(n <= 32);
    if (++rng_calls == rng_fail_call) return 1;
    int fd = open("/dev/urandom", O_RDONLY); if (fd < 0) return 1;
    int rc = read(fd, out, n); close(fd); return rc == (int)n ? 0 : 1;
}
int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    unsigned char entropy_buffer[65];
    const int lengths[] = {1,31,32,33,48,64,65};
    for (unsigned int i = 0; i < sizeof(lengths)/sizeof(lengths[0]); ++i) {
        rng_calls = 0;
        assert(entropy(NULL, entropy_buffer, lengths[i]) == 0);
        assert(rng_calls == (lengths[i]+31)/32);
    }
    rng_calls = 0; rng_fail_call = 2;
    memset(entropy_buffer, 0xA5, sizeof(entropy_buffer));
    assert(entropy(NULL, entropy_buffer, 48) == MBEDTLS_ERR_CTR_DRBG_ENTROPY_SOURCE_FAILED);
    for (int i = 0; i < 32; ++i) assert(entropy_buffer[i] == 0);
    rng_fail_call = 0;
    puts("PASS RK2206 32-byte RNG request limit, split entropy requests, failure propagation");
    epoch = 0;
    mbedtls_x509_crt crt; mbedtls_x509_crt_init(&crt);
    uint32_t flags = 0;
    verify_dates(NULL, &crt, 0, &flags); assert(flags & MBEDTLS_X509_BADCERT_OTHER);
    epoch = (unsigned int)time(NULL);
    crt.valid_from = (mbedtls_x509_time){2000,1,1,0,0,0};
    crt.valid_to = (mbedtls_x509_time){2001,1,1,0,0,0};
    flags = 0; verify_dates(NULL, &crt, 0, &flags); assert(flags & MBEDTLS_X509_BADCERT_EXPIRED);
    crt.valid_from = (mbedtls_x509_time){2090,1,1,0,0,0};
    crt.valid_to = (mbedtls_x509_time){2091,1,1,0,0,0};
    flags = 0; verify_dates(NULL, &crt, 0, &flags); assert(flags & MBEDTLS_X509_BADCERT_FUTURE);
    puts("PASS production TLS date checks: unsynced, expired, future");
    if (argc < 2) return 0;
    realtime = 1;
    Network net; NetworkInit(&net);
    assert(NetworkConnect(&net, argv[1], 8883) == 0);
    assert(safety_tls_connect(&net, argv[1]) == 0);
    safety_tls_close(); NetworkDisconnect(&net);
    puts("PASS production TLS handshake with official CA and matching hostname; no MQTT session or telemetry");
    NetworkInit(&net); assert(NetworkConnect(&net, argv[1], 8883) == 0);
    assert(safety_tls_connect(&net, "wrong.example.invalid") != 0);
    safety_tls_close(); NetworkDisconnect(&net);
    puts("PASS production TLS wrong hostname rejected");
    return 0;
}
