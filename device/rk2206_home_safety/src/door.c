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

#include "door.h"

#include "lz_hardware/pinctrl.h"

/* KY-025: + -> 3.3V, G -> GND, DO -> B6; AO unused.
 * B4/B5 belong to the RGB LED and must not be used here. */
#include <stdio.h>
#ifndef DOOR_GPIO
#define DOOR_GPIO GPIO0_PB6
#endif
#ifndef DOOR_OPEN_LEVEL
/* Confirmed on the connected KY-025: magnet near = high = closed. */
#define DOOR_OPEN_LEVEL 0
#endif
#define DOOR_DEBOUNCE_MS 100U
static bool ready, valid, candidate_set, stable_open;
static int candidate;
static unsigned long long candidate_since;
static bool read_failed;

void door_dev_init(void)
{
    ready = valid = candidate_set = false;
    stable_open = false;
    read_failed = false;
    /* IoTGpioInit forces DRIVE_LEVEL0. Use the board API here to keep
     * existing electrical settings for this input-only comparator signal. */
    unsigned int rc = LzGpioInit(DOOR_GPIO);
    printf("[door] stage=gpio rc=%u\n", rc);
    if (rc == 0) {
        rc = PinctrlSet(DOOR_GPIO, MUX_GPIO, PULL_KEEP, DRIVE_KEEP);
        printf("[door] stage=mux rc=%u\n", rc);
    }
    if (rc == 0) {
        rc = LzGpioSetDir(DOOR_GPIO, LZGPIO_DIR_IN);
        printf("[door] stage=input rc=%u\n", rc);
    }
    ready = rc == 0;
    printf("[door] B6 input init rc=%u open_level=%d debounce=%u ms\n",
           rc, DOOR_OPEN_LEVEL, DOOR_DEBOUNCE_MS);
}

bool door_read(unsigned long long now_ms, bool *open)
{
    LzGpioValue v = LZGPIO_LEVEL_LOW;
    if (!open || !ready) return false;
    unsigned int rc = LzGpioGetVal(DOOR_GPIO, &v);
    if (rc != 0) {
        if (!read_failed) printf("[door] stage=read rc=%u\n", rc);
        read_failed = true;
        valid = candidate_set = false;
        return false;
    }
    read_failed = false;
    int raw = v != LZGPIO_LEVEL_LOW;
    if (!candidate_set || candidate != raw) {
        candidate = raw;
        candidate_since = now_ms;
        candidate_set = true;
    }
    if (now_ms - candidate_since >= DOOR_DEBOUNCE_MS) {
        bool next = candidate == DOOR_OPEN_LEVEL;
        if (!valid || next != stable_open)
            printf("[door] raw=%d state=%s\n", candidate, next ? "OPEN" : "CLOSED");
        stable_open = next;
        valid = true;
    }
    if (valid) *open = stable_open;
    return valid;
}

bool door_is_open(void)
{
    return valid && stable_open;
}
