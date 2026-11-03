/*
 * Copyright (c) 2024 iSoftStone Education Co., Ltd.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "event_log.h"
#include "kv_store.h"
#include "los_tick.h"
#include "safety_runtime.h"
#include "sntp_client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define EVT_SLOTS 16
static safety_event_t s_events[EVT_SLOTS];
static unsigned int s_head, s_seq;
static void persist(unsigned int slot, const safety_event_t *e)
{
    char k[16], v[128];
    snprintf(k, sizeof(k), "evt_%02u", slot);
    snprintf(v, sizeof(v), "up=%us %s %d e=%u d=%d i=%u b=%u", e->up, e->tag, e->val, e->epoch,
             e->dur, e->id, e->boot);
    if (UtilsSetValue(k, v) < 0)
        printf("[evt] storage write failed\r\n");
}
void event_log_init(void)
{
    unsigned int i;
    char k[16], v[128];
    safety_storage_lock();
    if (UtilsGetValue("evt_head", v, sizeof(v)) >= 0)
        s_head = (unsigned int)strtoul(v, NULL, 10) % EVT_SLOTS;
    if (UtilsGetValue("evt_seq", v, sizeof(v)) >= 0)
        s_seq = (unsigned int)strtoul(v, NULL, 10);
    for (i = 0; i < EVT_SLOTS; i++)
    {
        safety_event_t *e = &s_events[i];
        snprintf(k, sizeof(k), "evt_%02u", i);
        if (UtilsGetValue(k, v, sizeof(v)) >= 0)
        {
            int fields = sscanf(v, "up=%us %15s %d e=%u d=%d i=%u b=%u", &e->up, e->tag, &e->val,
                                &e->epoch, &e->dur, &e->id, &e->boot);
            if (fields < 3)
                memset(e, 0, sizeof(*e));
            else
            {
                if (e->id > s_seq)
                    s_seq = e->id;
            }
        }
    }
    /* Migrate old records with stable IDs; boot=0 means legacy/unknown session. */
    for (i = 0; i < EVT_SLOTS; i++)
    {
        unsigned int slot = (s_head + i) % EVT_SLOTS;
        if (s_events[slot].tag[0] && !s_events[slot].id)
        {
            s_events[slot].id = ++s_seq;
            persist(slot, &s_events[slot]);
        }
    }
    snprintf(v, sizeof(v), "%u", s_seq);
    UtilsSetValue("evt_seq", v);
    safety_storage_unlock();
}
int event_log_add(const char *tag, int value)
{
    safety_event_t e = {0};
    char v[24];
    unsigned int slot;
    e.up = (unsigned int)(LOS_TickCountGet() / 1000);
    e.epoch = sntp_get_epoch();
    e.boot = safety_boot_id();
    e.val = value;
    snprintf(e.tag, sizeof(e.tag), "%s", tag);
    safety_storage_lock();
    safety_lock();
    e.id = ++s_seq;
    slot = s_head;
    s_events[slot] = e;
    s_head = (s_head + 1) % EVT_SLOTS;
    safety_unlock();
    persist(slot, &e);
    snprintf(v, sizeof(v), "%u", s_seq);
    UtilsSetValue("evt_seq", v);
    snprintf(v, sizeof(v), "%u", s_head);
    UtilsSetValue("evt_head", v);
    safety_storage_unlock();
    printf("[evt] id=%u %s value=%d\r\n", e.id, e.tag, e.val);
    return (int)e.id;
}
void event_log_set_duration(int id, int dur)
{
    unsigned int i;
    safety_event_t e;
    bool found = false;
    if (id <= 0)
        return;
    safety_storage_lock();
    safety_lock();
    for (i = 0; i < EVT_SLOTS; i++)
        if (s_events[i].id == (unsigned int)id)
        {
            s_events[i].dur = dur;
            e = s_events[i];
            found = true;
            break;
        }
    safety_unlock();
    if (found)
        persist(i, &e);
    safety_storage_unlock();
}
void event_log_clear(void)
{
    unsigned int i;
    char k[16];
    safety_storage_lock();
    safety_lock();
    memset(s_events, 0, sizeof(s_events));
    s_head = 0;
    safety_unlock();
    for (i = 0; i < EVT_SLOTS; i++)
    {
        snprintf(k, sizeof(k), "evt_%02u", i);
        UtilsDeleteValue(k);
    }
    UtilsSetValue("evt_head", "0");
    safety_storage_unlock();
}
int event_log_slots(void)
{
    return EVT_SLOTS;
}
int event_log_entry(int idx, safety_event_t *e)
{
    if (idx < 0 || idx >= EVT_SLOTS || !e)
        return 0;
    safety_lock();
    *e = s_events[(s_head + EVT_SLOTS - 1 - (unsigned int)idx) % EVT_SLOTS];
    safety_unlock();
    return e->tag[0] != 0;
}
unsigned int event_log_last_id(void)
{
    unsigned int id;
    safety_lock();
    id = s_seq;
    safety_unlock();
    return id;
}
int event_log_get(int idx, char *out, int n)
{
    safety_event_t e;
    if (!out || n <= 0 || !event_log_entry(idx, &e))
        return 0;
    snprintf(out, n, "up=%us %s %d e=%u d=%d", e.up, e.tag, e.val, e.epoch, e.dur);
    return 1;
}
const char *event_log_tag_cn(const char *tag)
{
    if(!strcmp(tag,"MED_DUE"))return "服药提醒";
    if(!strcmp(tag,"MED_TAKEN"))return "服药确认";
    if (tag == NULL)
    {
        return "";
    }
    if (strcmp(tag, "HELP") == 0)
    {
        return "紧急求助";
    }
    if (strcmp(tag, "NOACT") == 0)
    {
        return "无活动";
    }
    if (strcmp(tag, "GAS") == 0)
    {
        return "燃气泄漏";
    }
    if (strcmp(tag, "FALL") == 0)
    {
        return "疑似跌倒";
    }
    if (strcmp(tag, "ENV") == 0)
    {
        return "环境异常";
    }
    if (strcmp(tag, "DOOR") == 0)
    {
        return "房门开";
    }
    if (strcmp(tag, "DOOR_CLOSE") == 0) return "房门关";
    /* 未知 tag 原样返回 */
    return tag;
}

int event_log_snapshot(safety_event_t entries[16])
{
    int count = 0;
    safety_lock();
    for (int i = 0; i < 16; i++)
    {
        safety_event_t e = s_events[(s_head + 16 - 1 - (unsigned int)i) % 16];
        if (!e.tag[0])
            break;
        entries[count++] = e;
    }
    safety_unlock();
    return count;
}
