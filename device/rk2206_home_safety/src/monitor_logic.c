/* SPDX-License-Identifier: Apache-2.0 */
#include "monitor_logic.h"
#include <limits.h>
#include <math.h>

void monitor_pir_step(monitor_pir_t *s, int raw, uint64_t now)
{
    if (!s->started) { s->started = true; s->started_ms = now; }
    if (raw != 0 && raw != 1) {
        s->valid = false;
        s->candidate_set = false;
        return;
    }
    if (!s->candidate_set || raw != s->candidate) {
        s->candidate_set = true;
        s->candidate = raw;
        s->candidate_ms = now;
    }
    if (now < s->started_ms || now - s->started_ms < PIR_SETTLE_MS) {
        s->valid = false;
        return;
    }
    unsigned int wait = raw == 0 ? PIR_ACTIVE_DEBOUNCE_MS : PIR_IDLE_DEBOUNCE_MS;
    if (now >= s->candidate_ms && now - s->candidate_ms >= wait) {
        s->active = raw == 0; /* User-confirmed polarity on this board. */
        s->valid = true;
    }
}

void monitor_env_observe(monitor_env_t *s, bool ok, double temp, double humi, uint64_t now)
{
    s->last_read_ok = ok && isfinite(temp) && isfinite(humi) &&
        temp >= -40 && temp <= 125 && humi >= 0 && humi <= 100;
    if (s->last_read_ok) {
        s->temp = temp; s->humi = humi; s->good_ms = now; s->seen = true;
    }
}

bool monitor_env_available(const monitor_env_t *s, uint64_t now)
{
    return s->seen && now >= s->good_ms && now - s->good_ms <= ENV_LAST_GOOD_MS;
}

monitor_care_result_t monitor_care_step(monitor_care_t *s, uint64_t now,
    bool pir_valid, bool active, bool local_response, bool door_changed,
    unsigned int timeout_ms)
{
    if (local_response || (pir_valid && active)) {
        s->armed = true; s->last_activity_ms = now; s->alarm = false;
    } else if (!pir_valid) {
        /* Faults cannot manufacture inactivity or end an existing alert. */
        s->armed = false;
    } else if (door_changed && s->armed) {
        s->last_activity_ms = now; s->alarm = false;
    }
    uint64_t age = now >= s->last_activity_ms ? now - s->last_activity_ms : 0;
    bool warning = s->armed && pir_valid && age >= timeout_ms;
    if (warning && age - timeout_ms >= CARE_RESPONSE_MS) s->alarm = true;
    monitor_care_result_t result = {
        .warning = warning || s->alarm,
        .alarm = s->alarm,
        .inactive_ms = age > UINT_MAX ? UINT_MAX : (unsigned int)age
    };
    return result;
}

monitor_outputs_t monitor_outputs(bool alarm, bool help, bool red_alarm,
    bool care_warning, bool alarm_beep, bool care_beep,
    bool medicine, bool medicine_beep, bool night)
{
    monitor_outputs_t o = {0};
    if (alarm || care_warning) {
        o.owner = 3;
        o.alarm_light = true;
        o.beep = alarm ? alarm_beep : care_beep;
        if (help) { o.b = 255; }
        else if (red_alarm) { o.r = 255; }
        else { o.r = 255; o.g = 160; }
    } else if (medicine) {
        o.owner = 2; o.r = 16; o.g = 8; o.beep = medicine_beep;
    } else if (night) {
        o.owner = 1; o.r = o.g = o.b = 8;
    }
    return o;
}
