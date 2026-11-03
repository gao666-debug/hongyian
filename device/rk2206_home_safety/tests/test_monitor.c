#include "monitor_logic.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>

static void test_pir(void)
{
    monitor_pir_t p = {0};
    monitor_pir_step(&p, -1, 0);
    monitor_pir_step(&p, 1, 100);
    monitor_pir_step(&p, 1, 1000); assert(!p.valid);
    monitor_pir_step(&p, 1, 2000); assert(p.valid && !p.active);
    monitor_pir_step(&p, 0, 2100); assert(p.valid && p.active);
    monitor_pir_step(&p, 1, 2300); assert(p.valid && p.active); /* Brief release does not flicker. */
    monitor_pir_step(&p, 0, 2400); assert(p.active);
    monitor_pir_step(&p, 0, 2799); assert(p.active);
    monitor_pir_step(&p, 0, 2800); assert(p.valid && p.active);
    monitor_pir_step(&p, 1, 2900);
    monitor_pir_step(&p, 1, 3699); assert(p.active);
    monitor_pir_step(&p, 1, 3700); assert(p.valid && !p.active);
    monitor_pir_step(&p, -1, 3800); assert(!p.valid);
    monitor_pir_step(&p, 0, 3900); assert(p.valid && p.active);
    monitor_pir_step(&p, 0, 4300); assert(p.valid && p.active);
    monitor_pir_step(&p, 1, 4400);
    monitor_pir_step(&p, -1, 4500); assert(!p.valid);
    monitor_pir_step(&p, 1, 4600);
    monitor_pir_step(&p, 1, 5400); assert(p.valid && !p.active);
    puts("PASS PIR: active-low, startup settling, first-sample activation, release debounce, invalid read and recovery");
}

static void test_env(void)
{
    monitor_env_t e = {0};
    assert(!monitor_env_available(&e, 0));
    monitor_env_observe(&e, false, 0, 0, 100); assert(!e.seen);
    monitor_env_observe(&e, true, 25.5, 60, 1000);
    assert(monitor_env_available(&e, 1000) && e.last_read_ok);
    monitor_env_observe(&e, false, 0, 0, 2000);
    assert(monitor_env_available(&e, 2000) && !e.last_read_ok && e.temp == 25.5);
    assert(monitor_env_available(&e, 7000) && !monitor_env_available(&e, 7001));
    monitor_env_observe(&e, true, NAN, 60, 7100); assert(!monitor_env_available(&e, 7100));
    monitor_env_observe(&e, true, 25, 101, 7200); assert(!monitor_env_available(&e, 7200));
    monitor_env_observe(&e, true, 0, 0, 8000); assert(monitor_env_available(&e, 8000));
    assert(e.temp == 0 && e.humi == 0 && !monitor_env_available(&e, 7999));
    puts("PASS environment: no invented zero, bounded last-good cache, expiry, range/NaN rejection, recovery and valid zero");
}

static void test_care(void)
{
    monitor_care_t s = {0};
    monitor_care_result_t r = monitor_care_step(&s, 100000, true, false, false, false, 60000);
    assert(!r.warning && !r.alarm); /* unattended boot is not observed inactivity */
    r = monitor_care_step(&s, 101000, true, true, false, false, 60000);
    assert(!r.warning && r.inactive_ms == 0);
    r = monitor_care_step(&s, 160999, true, false, false, false, 60000); assert(!r.warning);
    r = monitor_care_step(&s, 161000, true, false, false, false, 60000); assert(r.warning && !r.alarm);
    r = monitor_care_step(&s, 190999, true, false, false, false, 60000); assert(r.warning && !r.alarm);
    r = monitor_care_step(&s, 191000, true, false, false, false, 60000); assert(r.warning && r.alarm);
    r = monitor_care_step(&s, 192000, false, false, false, false, 60000); assert(r.alarm);
    r = monitor_care_step(&s, 300000, true, false, false, false, 60000); assert(r.alarm);
    r = monitor_care_step(&s, 301000, true, false, true, false, 60000); assert(!r.alarm && r.inactive_ms == 0);
    r = monitor_care_step(&s, 361000, true, false, false, false, 60000); assert(r.warning);
    r = monitor_care_step(&s, 362000, true, true, false, false, 60000); assert(!r.warning);
    r = monitor_care_step(&s, 363000, false, false, false, false, 60000); assert(!r.alarm);
    r = monitor_care_step(&s, 500000, true, false, false, false, 60000); assert(!r.alarm && !r.warning);
    r = monitor_care_step(&s, 501000, true, true, false, false, 60000); assert(!r.alarm);
    r = monitor_care_step(&s, 561000, true, false, false, true, 60000); assert(!r.warning);
    puts("PASS care: observation required, response window, local response, fault never creates/clears an alarm, rearm and door transition");
}

static void test_outputs(void)
{
    for (int warning = 0; warning < 2; ++warning)
    for (int medicine = 0; medicine < 2; ++medicine)
    for (int night = 0; night < 2; ++night) {
        monitor_outputs_t o = monitor_outputs(true,false,false,warning,false,true,medicine,true,night);
        assert(o.owner == 3 && o.r == 255 && o.g == 160 && !o.b);
        assert(o.alarm_light && !o.beep); /* muted alarm is never replaced by medicine beep */
        o = monitor_outputs(false,false,false,true,false,true,medicine,true,night);
        assert(o.owner == 3 && o.alarm_light && o.r == 255 && o.g == 160 && !o.b && o.beep);
        o = monitor_outputs(true,true,false,warning,true,false,medicine,true,night);
        assert(o.owner == 3 && o.b == 255 && o.r == 0 && o.beep);
        o = monitor_outputs(true,false,true,warning,true,false,medicine,true,night);
        assert(o.r == 255 && !o.g && !o.b);
    }
    monitor_outputs_t o = monitor_outputs(false,false,false,false,false,false,true,true,true);
    assert(o.owner == 2 && o.r == 16 && o.g == 8 && !o.b && o.beep && !o.alarm_light);
    o = monitor_outputs(false,false,false,false,false,false,false,false,true);
    assert(o.owner == 1 && o.r == 8 && o.g == 8 && o.b == 8 && !o.beep);
    o = monitor_outputs(false,false,false,false,false,false,false,false,false);
    assert(o.owner == 0 && !o.r && !o.g && !o.b && !o.beep);
    puts("PASS outputs: safety alarm > care response window > medicine > night, mute priority and exact RGB values");
}

int main(void) { test_pir(); test_env(); test_care(); test_outputs(); }
