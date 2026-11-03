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

#include "key.h"

#include <stdio.h>

#include "los_task.h"
#include "los_tick.h"
#include "iot_adc.h"
#include "iot_errno.h"

/* No external receiver connected:
 * idle 2.191V, K3 0.003V, K4 0.693V, K5 1.089V, K6 0.358V.
 * ADC conversion readings; not a physical supply-voltage measurement. */
#define KEY_ADC_CHANNEL     7
#define KEY_VREF            3.3f
#define KEY_ADC_FULL        1024.0f

#define KEY_DEBOUNCE_CNT    5       /* 连续 5 次一致才认可(≈250ms) */
#define KEY_SCAN_MS         50
#define KEY_LONG_PRESS_MS   2000    /* 按住≥2s 视为长按 */

static volatile key_id_t g_event = KEY_NONE;
static volatile key_id_t g_long_event = KEY_NONE;
static volatile float g_last_v = 0.0f;

static key_id_t decode(float v)
{
    if (v > 1.64f)
    {
        return KEY_NONE;     /* 空载（放宽，抗噪） */
    }
    else if (v > 0.89f)
    {
        return KEY_LEFT;     /* K5 ~1.089V */
    }
    else if (v > 0.525f)
    {
        return KEY_DOWN;     /* K4 ~0.693V */
    }
    else if (v > 0.18f)
    {
        return KEY_RIGHT;    /* K6 ~0.358V */
    }
    return KEY_UP;           /* K3 ~0V */
}

static float read_voltage(void)
{
    unsigned int data = 0;

    if (IoTAdcGetVal(KEY_ADC_CHANNEL, &data) != IOT_SUCCESS)
    {
        return -1.0f;
    }
    return data * KEY_VREF / KEY_ADC_FULL;
}

/***************************************************************
 * 函数名称: key_thread
 * 说    明: 按键扫描线程（50ms + 去抖）
 ***************************************************************/
static void key_thread(void *arg)
{
    key_id_t stable = KEY_NONE;
    key_id_t candidate = KEY_NONE;
    int cnt = 0;
    bool armed = true;     /* 只有松开(回到无键)后才允许再次触发，防连发 */
    key_id_t held = KEY_NONE;
    unsigned int held_since = 0;
    bool long_sent = false;

    while (1)
    {
        float v = read_voltage();
        key_id_t cur;

        if (v >= 0.0f)
        {
            g_last_v = v;
        }
        cur = (v >= 0.0f) ? decode(v) : KEY_NONE;

        if (cur == candidate)
        {
            if (cnt < KEY_DEBOUNCE_CNT) cnt++;
        }
        else
        {
            candidate = cur;
            cnt = 1;
        }

        if (stable == KEY_NONE)
        {
            armed = true;
        }

        if ((cnt >= KEY_DEBOUNCE_CNT) && (candidate != stable))
        {
            stable = candidate;
            if (stable != KEY_NONE)
            {
                if (armed)
                {
                    /* K6 short is emitted on release, so holding it to edit/save cannot
                     * first confirm a dose or advance the editing field. */
                    if(stable!=KEY_RIGHT)g_event = stable;
                    armed = false;
                    printf("[key] press id=%d v=%.2fV\r\n", (int)stable, g_last_v);
                }
                held = stable;
                held_since = LOS_TickCountGet();
                long_sent = false;
            }
            else
            {
                if(held==KEY_RIGHT && !long_sent)g_event=KEY_RIGHT;
                held = KEY_NONE;
                long_sent = false;
            }
        }

        /* 按住超过阈值 -> 产生一次长按事件 */
        if (held != KEY_NONE && !long_sent &&
            (LOS_TickCountGet() - held_since) >= KEY_LONG_PRESS_MS)
        {
            g_long_event = held;
            long_sent = true;
            printf("[key] long id=%d v=%.2fV\r\n", (int)held, g_last_v);
        }

        LOS_Msleep(KEY_SCAN_MS);
    }
}

void key_dev_init(void)
{
    unsigned int thread_id;
    TSK_INIT_PARAM_S task = {0};
    unsigned int ret = LOS_OK;

    IoTAdcInit(KEY_ADC_CHANNEL);

    task.pfnTaskEntry = (TSK_ENTRY_FUNC)key_thread;
    task.uwStackSize = 2048;
    task.pcName = "key thread";
    task.usTaskPrio = 23;
    ret = LOS_TaskCreate(&thread_id, &task);
    if (ret != LOS_OK)
    {
        printf("Failed to create key task ret:0x%x\n", ret);
    }
}

key_id_t key_take_event(void)
{
    key_id_t e = g_event;

    g_event = KEY_NONE;
    return e;
}

key_id_t key_take_long_event(void)
{
    key_id_t e = g_long_event;

    g_long_event = KEY_NONE;
    return e;
}

float key_last_voltage(void)
{
    return g_last_v;
}
