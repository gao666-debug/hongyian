/* SPDX-License-Identifier: Apache-2.0 */
#ifndef MONITOR_LOGIC_H
#define MONITOR_LOGIC_H
#include <stdbool.h>
#include <stdint.h>

/* Engineering stability windows, not a claim about sensor detection accuracy. */
#define PIR_SETTLE_MS 2000U
#define PIR_ACTIVE_DEBOUNCE_MS 0U /* Respond on the next successful active sample. */
#define PIR_IDLE_DEBOUNCE_MS 800U
#define ENV_LAST_GOOD_MS 6000U
#define CARE_RESPONSE_MS 30000U

typedef struct {
    bool started, candidate_set, valid, active;
    int candidate;
    uint64_t started_ms, candidate_ms;
} monitor_pir_t;
void monitor_pir_step(monitor_pir_t *s, int raw, uint64_t now);

typedef struct {
    bool seen, last_read_ok;
    double temp, humi;
    uint64_t good_ms;
} monitor_env_t;
void monitor_env_observe(monitor_env_t *s, bool ok, double temp, double humi, uint64_t now);
bool monitor_env_available(const monitor_env_t *s, uint64_t now);

typedef struct {
    bool armed, alarm;
    uint64_t last_activity_ms;
} monitor_care_t;
typedef struct {
    bool warning, alarm;
    unsigned int inactive_ms;
} monitor_care_result_t;
monitor_care_result_t monitor_care_step(monitor_care_t *s, uint64_t now,
    bool pir_valid, bool active, bool local_response, bool door_changed,
    unsigned int timeout_ms);

typedef struct {
    bool beep, alarm_light;
    unsigned char r, g, b;
    int owner; /* Existing living protocol: 0 off, 1 night, 2 medicine, 3 alarm. */
} monitor_outputs_t;
monitor_outputs_t monitor_outputs(bool alarm, bool help, bool red_alarm,
    bool care_warning, bool alarm_beep, bool care_beep,
    bool medicine, bool medicine_beep, bool night);
#endif
