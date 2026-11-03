/* SPDX-License-Identifier: Apache-2.0 */
#ifndef SAFETY_RUNTIME_H
#define SAFETY_RUNTIME_H
#include <stdbool.h>
#include <stdint.h>
#define SAFETY_FW "v3.5.44"
#define SAFETY_CMD_ACK 1
#define SAFETY_CMD_MUTE 2
#define SAFETY_CMD_LIVING 3
#define SAFETY_CMD_MEDICINE 4
#define SAFETY_CMD_MEDICINE_PLAN 5
#define SAFETY_CMD_MEDICINE_STATE 6
#define SAFETY_EVENT_COUNT 16
/* Locks are created before any application task starts. */
int safety_runtime_init(void);
void safety_i2c_lock(void);
int safety_i2c_try_lock(unsigned int ticks);
unsigned int safety_i2c_lock_timeouts(void);
void safety_i2c_unlock(void);
/* Shared EI2C0_M2 bus. Caller must hold safety_i2c_lock; success is cached. */
int safety_i2c_prepare_locked(void);
/* Call after a returned hardware transaction while holding the bus lock. */
void safety_i2c_result_locked(bool ok);
void safety_lock(void);
void safety_unlock(void);
void safety_storage_lock(void);
void safety_storage_unlock(void);
typedef struct
{
    char wifi_ssid[33], wifi_pass[65], broker[96], client_id[96];
    unsigned int port, report_ms, noact_ms, mute_ms;
    double temp_low, temp_high, humi_low, humi_high, gas_limit;
    bool demo;
} safety_config_t;
void safety_config_get(safety_config_t *out);
/* All changes validated together before any KV write. Network changes need reboot. */
int safety_config_set(const safety_config_t *cfg);
int safety_config_json(char *out, int n);
int safety_config_apply_json(const char *json);
typedef struct
{
    unsigned int id;
    int code, result;
    char source[8];
    char payload[384];
} safety_command_t;
int safety_command_submit(int code, const char *source, unsigned int *id);
int safety_command_submit_payload(int code, const char *source, const char *payload, unsigned int *id);
int safety_command_take(safety_command_t *cmd);
void safety_command_done(const safety_command_t *cmd, int result);
int safety_command_get(unsigned int id, safety_command_t *cmd);
unsigned int safety_boot_id(void);
typedef struct
{
    double temp, humi, gas, light;
    int body, inactive, alarm, status, help, fall, gas_alarm, door, smoke;
    int env_valid, imu_valid, mute;
    int door_valid;
    int env_cached, env_age_s;
    int body_valid; /* Local validity; invalid PIR is omitted from cloud properties. */
    unsigned int sample_up;
    int sample_received;
} safety_state_t;
typedef struct {
    int sample_age_s; /* -1 means no sample, or an inconsistent clock. */
    int stale, env_available, fall_detection_available;
    const char *data_state; /* waiting / stale / ready */
} safety_quality_t;
void safety_state_quality(const safety_state_t *state, unsigned int uptime, safety_quality_t *out);
void safety_state_set(const safety_state_t *state);
void safety_state_get(safety_state_t *state);
int safety_status_json(char *out, int n);
void safety_health_set(int env, int imu, int mute);
void safety_mqtt_connected(int connected);
int safety_mqtt_is_connected(void);
typedef struct {
    int connected, report_received;
    unsigned int report_up, report_epoch;
} safety_mqtt_diag_t;
void safety_mqtt_report_ok(unsigned int uptime, unsigned int epoch);
void safety_mqtt_diag_get(safety_mqtt_diag_t *out);
/* Ordered fall detector: reset on invalid samples, confirm only after impact. */
typedef struct
{
    uint64_t free_ms, impact_ms;
    bool free_seen, pending, active;
    float peak;
} safety_fall_t;
void safety_fall_sample(safety_fall_t *f, float g, uint64_t now, bool valid);
void safety_alarm_edges(const bool alarms[5], bool previous[5], unsigned int *rising,
                        unsigned int *falling);
/* Returns alarm code 1..5 only for a new highest-priority active alarm. */
int safety_voice_select(const bool alarms[5], unsigned int rising, bool suppressed);
void safety_fall_confirm(safety_fall_t *f, uint64_t now, uint64_t last_activity, bool body);
#endif
