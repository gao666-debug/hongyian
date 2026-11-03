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

#include "button.h"
#include "lz_hardware/pinctrl.h"
#include <stdio.h>
#include "safety_runtime.h"
#ifndef SAFETY_HOST_TEST
#include "los_task.h"
#include "los_tick.h"
#endif

#define DEBOUNCE_MS 50U
#define ACK_HOLD_MS 1000U
typedef struct {
    GpioID gpio;
    bool ready, candidate, stable, armed, sampled, failed;
    unsigned long long since;
} wireless_input_t;
/* Only D0 -> B7 is connected. Never initialize floating spare channels. */
#define WIRELESS_INPUTS 1
static wireless_input_t inputs[WIRELESS_INPUTS] = {{.gpio=GPIO0_PB7}};
static const char *pin_names[WIRELESS_INPUTS] = {"B7"};
static const char *action_names[WIRELESS_INPUTS] = {"CONTEXT"};
static const unsigned int action_flags[WIRELESS_INPUTS] = {WIRELESS_SOS};
static unsigned int pending;
void button_scan(unsigned long long now_ms);
#ifndef SAFETY_HOST_TEST
static void wireless_thread(void *arg)
{
    (void)arg;
    while (1) { button_scan(LOS_TickCountGet()); LOS_Msleep(10); }
}
#endif

void button_dev_init(void)
{
    pending = 0;
    for (int i=0; i<WIRELESS_INPUTS; i++) {
        GpioID gpio = inputs[i].gpio;
        inputs[i] = (wireless_input_t){.gpio=gpio};
        unsigned int rc = LzGpioInit(gpio);
        if (!rc) rc = PinctrlSet(gpio, MUX_GPIO, PULL_DOWN, DRIVE_KEEP);
        if (!rc) rc = LzGpioSetDir(gpio, LZGPIO_DIR_IN);
        inputs[i].ready = rc == 0;
        printf("[wireless] %s input init rc=%u action=%s hold=%ums; waiting for release\n",
               pin_names[i],rc,action_names[i],i==1?ACK_HOLD_MS:DEBOUNCE_MS);
    }
#ifndef SAFETY_HOST_TEST
    unsigned int tid;
    TSK_INIT_PARAM_S task = {0};
    task.pfnTaskEntry = (TSK_ENTRY_FUNC)wireless_thread;
    task.uwStackSize = 2048;
    task.pcName = "wireless SOS";
    task.usTaskPrio = 23;
    unsigned int rc = LOS_TaskCreate(&tid, &task);
    if (rc != LOS_OK) {
        for (int i=0; i<WIRELESS_INPUTS; ++i) inputs[i].ready = false;
        printf("[wireless] scan task failed rc=%u\n",rc);
    }
#endif
}

void button_scan(unsigned long long now_ms)
{
    for (int i=0; i<WIRELESS_INPUTS; i++) {
        wireless_input_t *s = &inputs[i];
        LzGpioValue value = LZGPIO_LEVEL_LOW;
        if (!s->ready) continue;
        unsigned int rc=LzGpioGetVal(s->gpio, &value);
        if (rc) {
            if (!s->failed) printf("[wireless] %s read failed rc=%u\n",pin_names[i],rc);
            s->failed = true;
            s->sampled = s->armed = false;
            continue;
        }
        s->failed = false;
        bool raw = value != LZGPIO_LEVEL_LOW;
        if (!s->sampled || s->candidate != raw) {
            s->candidate = raw;
            s->since = now_ms;
            s->sampled = true;
        }
        unsigned int threshold=raw && i==1 ? ACK_HOLD_MS : DEBOUNCE_MS;
        if (now_ms - s->since < threshold) continue;
        if (!s->candidate) {
            s->stable = false;
            s->armed = true;
            continue;
        }
        if (s->stable || !s->armed) continue;
        s->stable = true;
        s->armed = false;
        printf("[wireless] %s pressed: %s\n",pin_names[i],action_names[i]);
        safety_lock();
        pending |= action_flags[i];
        safety_unlock();
    }
}

unsigned int button_take_events(void)
{
    safety_lock();
    unsigned int events = pending;
    pending = 0;
    safety_unlock();
    return events;
}
