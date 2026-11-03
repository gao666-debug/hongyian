/* SPDX-License-Identifier: Apache-2.0 */
#include "safety_stats.h"
#include "cJSON.h"
#include "kv_store.h"
#include "los_tick.h"
#include "safety_runtime.h"
#include "sntp_client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct
{
    unsigned int day, activity, alarms, types[5], hours[24];
} day_t;
static day_t s_days[7], s_unsynced;
static unsigned int s_activity, s_alarms, s_hours[24], s_types[5];
static uint64_t s_saved;
static unsigned int s_dirty_slots;
static unsigned int day_now(void)
{
    unsigned int e = sntp_get_epoch();
    return e ? (e + 28800) / 86400 : 0;
}
static day_t *today(unsigned int day)
{
    day_t *d;
    if (!day)
        return &s_unsynced;
    d = &s_days[day % 7];
    if (d->day != day)
    {
        memset(d, 0, sizeof(*d));
        d->day = day;
        s_dirty_slots |= 1U << (day % 7);
    }
    return d;
}
void safety_stats_init(void)
{
    unsigned int i;
    char k[16], v[128];
    memset(s_days, 0, sizeof(s_days));
    safety_storage_lock();
    for (i = 0; i < 7; i++)
    {
        day_t *d = &s_days[i];
        snprintf(k, sizeof(k), "st%u", i);
        if (UtilsGetValue(k, v, sizeof(v)) < 0 ||
            sscanf(v, "%u %u %u %u %u %u %u %u", &d->day, &d->activity, &d->alarms, &d->types[0],
                   &d->types[1], &d->types[2], &d->types[3], &d->types[4]) != 8)
        {
            memset(d, 0, sizeof(*d));
            continue;
        }
        unsigned int h;
        snprintf(k, sizeof(k), "sh%u", i);
        if (UtilsGetValue(k, v, sizeof(v)) >= 0 && strlen(v) == 104)
        {
            char stamp[9];
            memcpy(stamp, v, 8);
            stamp[8] = 0;
            if ((unsigned int)strtoul(stamp, NULL, 16) == d->day)
                for (h = 0; h < 24; h++)
                {
                    char hex[5];
                    memcpy(hex, v + 8 + h * 4, 4);
                    hex[4] = 0;
                    d->hours[h] = (unsigned int)strtoul(hex, NULL, 16);
                }
        }
    }
    safety_storage_unlock();
    s_saved = LOS_TickCountGet();
}
void safety_stats_activity(void)
{
    unsigned int e = sntp_get_epoch(), day = e ? (e + 28800) / 86400 : 0,
                 h = e ? ((e / 3600 + 8) % 24) : (unsigned int)(LOS_TickCountGet() / 3600000) % 24;
    safety_lock();
    day_t *d = today(day);
    s_activity++;
    s_hours[h]++;
    d->activity++;
    d->hours[h]++;
    if (day)
        s_dirty_slots |= 1U << (day % 7);
    safety_unlock();
}
void safety_stats_alarm(int code)
{
    safety_lock();
    day_t *d = today(day_now());
    s_alarms++;
    d->alarms++;
    if (code >= 1 && code <= 5)
    {
        s_types[code - 1]++;
        d->types[code - 1]++;
    }
    if (d != &s_unsynced)
        s_dirty_slots |= 1U << (d->day % 7);
    safety_unlock();
}
void safety_stats_tick(void)
{
    day_t days[7];
    uint64_t now = LOS_TickCountGet();
    unsigned int i;
    unsigned int dirty;
    if (now - s_saved < 60000)
        return;
    safety_lock();
    if (!s_dirty_slots)
    {
        safety_unlock();
        return;
    }
    memcpy(days, s_days, sizeof(days));
    dirty = s_dirty_slots;
    s_dirty_slots = 0;
    safety_unlock();
    bool fail = false;
    safety_storage_lock();
    for (i = 0; i < 7; i++)
        if (days[i].day && (dirty & (1U << i)))
        {
            char k[16], v[128];
            day_t *d = &days[i];
            snprintf(v, 9, "%08x", d->day);
            for (unsigned int h = 0; h < 24; h++)
                snprintf(v + 8 + h * 4, 5, "%04x", d->hours[h] > 65535 ? 65535 : d->hours[h]);
            snprintf(k, sizeof(k), "sh%u", i);
            if (UtilsSetValue(k, v) < 0)
                fail = true;

            snprintf(v, sizeof(v), "%u %u %u %u %u %u %u %u", d->day, d->activity, d->alarms,
                     d->types[0], d->types[1], d->types[2], d->types[3], d->types[4]);
            snprintf(k, sizeof(k), "st%u", i);
            if (UtilsSetValue(k, v) < 0)
                fail = true;
        }
    safety_storage_unlock();
    s_saved = now;
    if (fail)
    {
        safety_lock();
        s_dirty_slots |= dirty;
        safety_unlock();
        printf("[stats] persist failed\r\n");
    }
}
static cJSON *hours_json(const unsigned int *h)
{
    unsigned int i;
    cJSON *a = cJSON_CreateArray();
    for (i = 0; i < 24; i++)
        cJSON_AddItemToArray(a, cJSON_CreateNumber(h[i]));
    return a;
}
static cJSON *types_json(const unsigned int *t)
{
    const char *names[] = {"help", "fall", "gas", "noact", "env"};
    unsigned int i;
    cJSON *o = cJSON_CreateObject();
    for (i = 0; i < 5; i++)
        cJSON_AddNumberToObject(o, names[i], t[i]);
    return o;
}
static void date_text(unsigned int day, char *out, int n)
{
    long z = (long)day + 719468, era = z / 146097;
    unsigned long doe = z - era * 146097,
                  yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long y = (long)yoe + era * 400;
    unsigned long doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    int d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9;
    y += m <= 2;
    snprintf(out, n, "%04ld-%02d-%02d", y, m, d);
}
int safety_stats_json(char *out, int n)
{
    day_t ds[7], td, us;
    unsigned int h[24], t[5], act, alarms, day = day_now(), i;
    safety_state_t st;
    cJSON *o = cJSON_CreateObject(), *a = cJSON_CreateArray();
    int ok;
    if (!o || !a)
    {
        cJSON_Delete(o);
        cJSON_Delete(a);
        return -1;
    }
    safety_lock();
    td = *today(day);
    us = s_unsynced;
    memcpy(ds, s_days, sizeof(ds));
    memcpy(h, s_hours, sizeof(h));
    memcpy(t, s_types, sizeof(t));
    act = s_activity;
    alarms = s_alarms;
    safety_unlock();
    safety_state_get(&st);
    cJSON_AddNumberToObject(o, "uptime", LOS_TickCountGet() / 1000);
    cJSON_AddNumberToObject(o, "boot_id", safety_boot_id());
    cJSON_AddNumberToObject(o, "activity_total", act);
    cJSON_AddNumberToObject(o, "body", st.body);
    cJSON_AddNumberToObject(o, "inactive", st.inactive);
    cJSON_AddItemToObject(o, "activity_by_hour", hours_json(h));
    cJSON_AddNumberToObject(o, "alarm_total", alarms);
    cJSON_AddItemToObject(o, "alarm_by_type", types_json(t));
    cJSON_AddBoolToObject(o, "time_synced", day != 0);
    cJSON_AddNumberToObject(o, "unsynced_activity", us.activity);
    cJSON_AddNumberToObject(o, "today_activity", day ? td.activity : 0);
    cJSON_AddItemToObject(o, "today_by_hour", hours_json(day ? td.hours : (unsigned int[24]){0}));
    for (i = 0; i < 7; i++)
    {
        unsigned int want = day >= i ? day - i : 0;
        day_t *d = &ds[want % 7];
        if (!want || d->day != want)
            continue;
        char date[16];
        cJSON *x = cJSON_CreateObject();
        date_text(d->day, date, sizeof(date));
        cJSON_AddStringToObject(x, "date", date);
        cJSON_AddNumberToObject(x, "activity_total", d->activity);
        cJSON_AddNumberToObject(x, "alarm_total", d->alarms);
        cJSON_AddItemToObject(x, "activity_by_hour", hours_json(d->hours));
        cJSON_AddItemToObject(x, "alarm_by_type", types_json(d->types));
        cJSON_AddItemToArray(a, x);
    }
    cJSON_AddItemToObject(o, "days", a);
    cJSON_AddStringToObject(o, "coverage", "observed_only");
    ok = cJSON_PrintPreallocated(o, out, n, 0);
    cJSON_Delete(o);
    return ok ? 0 : -1;
}
