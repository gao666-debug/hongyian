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

/* 每次按键结束后打印本次按压的最低原始电压。
 * 即使该电压落不进任何档位（decode 判为 KEY_NONE），也能看到实测值，
 * 用来给换批次板重新标定阈值。量产可置 0 关掉。 */
#ifndef KEY_ADC_VERBOSE
#define KEY_ADC_VERBOSE     1
#endif

static volatile key_id_t g_event = KEY_NONE;
static volatile key_id_t g_long_event = KEY_NONE;
static volatile float g_last_v = 0.0f;

/* 按键批次：0=旧板，1=新板（2026-11-13 因语音模块无法烧录更换的那块）。
 *
 * 两块板四颗键的功能顺序完全一致（按电压升序：UP -> RIGHT -> DOWN -> LEFT），
 * 差别只在分压比：新板每档电压都更高，K5 串到 1.66V，超过旧代码 1.64V 的
 * 空载上限，被当成“没按键”，所以表现为无反应。
 *
 * 每键实测最低电压（[adc] min= 打印值）：
 *   档位   旧板      新板      功能
 *   0      0.003V    0.01V     K3  求助   KEY_UP
 *   1      0.358V    0.55V     K6  确认   KEY_RIGHT
 *   2      0.693V    1.06V     K4  静音   KEY_DOWN
 *   3      1.089V    1.66V     K5  翻页   KEY_LEFT
 *   空载   2.19V     2.19V              KEY_NONE
 *
 * 新板阈值取相邻两档中点：0.28 / 0.80 / 1.34 / 1.92V。
 * 以后再换板只重标这四个阈值，映射顺序不用动。 */
#ifndef KEY_LAYOUT_NEW_BOARD
#define KEY_LAYOUT_NEW_BOARD    1
#endif

#if KEY_LAYOUT_NEW_BOARD
#define KEY_V_NONE      1.92f   /* > 此值视为空载（空载实测 2.19V） */
#define KEY_V_LEFT      1.34f   /* 1.34~1.92V  K5 1.66V 翻页 */
#define KEY_V_DOWN      0.80f   /* 0.80~1.34V  K4 1.06V 静音 */
#define KEY_V_RIGHT     0.28f   /* 0.28~0.80V  K6 0.55V 确认 */
#else
#define KEY_V_NONE      1.64f
#define KEY_V_LEFT      0.89f
#define KEY_V_DOWN      0.525f
#define KEY_V_RIGHT     0.18f
#endif

static key_id_t decode(float v)
{
    if (v > KEY_V_NONE)
    {
        return KEY_NONE;     /* 空载，不判键 */
    }
    else if (v > KEY_V_LEFT)
    {
        return KEY_LEFT;     /* K5 翻页 */
    }
    else if (v > KEY_V_DOWN)
    {
        return KEY_DOWN;     /* K4 静音 */
    }
    else if (v > KEY_V_RIGHT)
    {
        return KEY_RIGHT;    /* K6 确认 */
    }
    return KEY_UP;           /* K3 求助 */
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
    float press_min = -1.0f;    /* 本次按压过程的最低电压，用于标定 */

    while (1)
    {
        float v = read_voltage();
        key_id_t cur;

        if (v >= 0.0f)
        {
            g_last_v = v;
        }
        cur = (v >= 0.0f) ? decode(v) : KEY_NONE;

#if KEY_ADC_VERBOSE
        /* 空载约 2.19V；只要低于 2.00V 就认为有人在按，记录最低值，
         * 松手时打印一行，含 decode 结果（dec=0 表示这一档没匹配上）。 */
        if (v >= 0.0f && v < 2.00f)
        {
            if (press_min < 0.0f || v < press_min) press_min = v;
        }
        else if (press_min >= 0.0f)
        {
            printf("[adc] min=%.2fV dec=%d\r\n", press_min, (int)decode(press_min));
            press_min = -1.0f;
        }
#endif

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
