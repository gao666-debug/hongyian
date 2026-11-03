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

#include "home_safety.h"

#include <stdbool.h>
#include <stdio.h>
#include "iot_errno.h"
#include "los_tick.h"
#include "safety_runtime.h"
#include "monitor_logic.h"
#include <string.h>

#include "iot_pwm.h"
#include "iot_gpio.h"
#include "lz_hardware.h"
#include "lz_hardware/pinctrl.h"

/* 引脚定义（与通晓开发板原理图 / d2_smart_security 一致） */
#define BEEP_PORT               EPWMDEV_PWM5_M0     /* 蜂鸣器 PWM5 (GPIO0_PC5) */
#define GPIO_ALARM_LIGHT        GPIO0_PA5           /* 报警灯 */
#define GPIO_BODY_INDUCTION     GPIO0_PA3           /* 人体感应（热释电红外） */

static body_diagnostics_t g_body_diag = {.raw = -1, .init_rc = 1, .read_rc = 1};
static uint64_t g_body_high_since;
static monitor_pir_t g_body_filter;

void body_induction_dev_init(void)
{
    /* Restore repository baseline: both calls are made unconditionally. */
    unsigned int gpio_rc = IoTGpioInit(GPIO_BODY_INDUCTION);
    unsigned int dir_rc = IoTGpioSetDir(GPIO_BODY_INDUCTION, IOT_GPIO_DIR_IN);
    unsigned int rc = dir_rc; /* GPIO Init may report already initialized. */
    safety_lock();
    g_body_diag.init_called = 1;
    g_body_diag.gpio_rc = gpio_rc;
    g_body_diag.mux_rc = 0; /* No independent mux call in baseline path. */
    g_body_diag.dir_rc = dir_rc;
    g_body_diag.init_rc = rc;
    g_body_diag.raw = -1;
    g_body_diag.read_rc = 1;
    g_body_diag.high_seconds = 0;
    g_body_diag.changes = 0;
    g_body_diag.samples = 0;
    memset(&g_body_filter, 0, sizeof(g_body_filter));
    monitor_pir_step(&g_body_filter, -1, LOS_TickCountGet());
    g_body_diag.filtered_valid = 0;
    g_body_diag.filtered_active = 0;
    safety_unlock();
    printf("[pir] baseline PA3 init=%u dir=%u\r\n", gpio_rc, dir_rc);
}

void body_induction_get_state(bool *dat)
{
    IotGpioValue value = IOT_GPIO_VALUE0;
    /* Always attempt the read, including when Init reports already initialized.
     * Keep raw diagnostic electrical levels separate from active-low activity. */
    unsigned int rc = IoTGpioGetInputVal(GPIO_BODY_INDUCTION, &value);
    if (g_body_diag.dir_rc != IOT_SUCCESS) rc = g_body_diag.dir_rc;
    uint64_t now = LOS_TickCountGet();
    int raw = rc == IOT_SUCCESS ? (value != IOT_GPIO_VALUE0) : -1;
    safety_lock();
    int previous = g_body_diag.raw;
    if (raw != previous) {
        if (previous >= 0 && raw >= 0) g_body_diag.changes++;
        g_body_high_since = now;
    }
    g_body_diag.samples++;
    g_body_diag.raw = raw;
    g_body_diag.read_rc = rc;
    monitor_pir_step(&g_body_filter, raw, now);
    g_body_diag.filtered_valid = g_body_filter.valid;
    g_body_diag.filtered_active = g_body_filter.valid && g_body_filter.active;
    bool active = g_body_diag.filtered_active;
    g_body_diag.high_seconds = raw == 1 ? (unsigned int)((now - g_body_high_since) / 1000) : 0;
    safety_unlock();
    *dat = active;
    if (raw != previous) printf("[pir] PA3 raw=%d read_rc=%u\r\n", raw, rc);
}

void body_induction_diagnostics(body_diagnostics_t *out)
{
    safety_lock();
    *out = g_body_diag;
    safety_unlock();
}

/***************************************************************
 * 函数名称: beep_dev_init
 * 说    明: 蜂鸣器初始化
 * 参    数: 无
 * 返 回 值: 无
 ***************************************************************/
void beep_dev_init(void)
{
    IoTPwmInit(BEEP_PORT);
}

/***************************************************************
 * 函数名称: beep_set_state
 * 说    明: 设置蜂鸣器状态（true=响）
 * 参    数: state - 期望状态
 * 返 回 值: 无
 ***************************************************************/
void beep_set_state(bool state)
{
    static bool last_state = false;

    if (state == last_state)
    {
        return;
    }

    if (state)
    {
        IoTPwmStart(BEEP_PORT, 20, 1000);
    }
    else
    {
        IoTPwmStart(BEEP_PORT, 1, 1000);
        IoTPwmStop(BEEP_PORT);
    }

    last_state = state;
}

/***************************************************************
 * 函数名称: alarm_light_init
 * 说    明: 报警灯初始化
 * 参    数: 无
 * 返 回 值: 无
 ***************************************************************/
void alarm_light_init(void)
{
    IoTGpioInit(GPIO_ALARM_LIGHT);
    IoTGpioSetDir(GPIO_ALARM_LIGHT, IOT_GPIO_DIR_OUT);
}

/***************************************************************
 * 函数名称: alarm_light_set_gpio
 * 说    明: 设置报警灯（true=亮）
 * 参    数: state - 期望状态
 * 返 回 值: 无
 ***************************************************************/
void alarm_light_set_gpio(bool state)
{
    static bool last_state = false;

    if (state == last_state)
    {
        return;
    }

    IoTGpioSetOutputVal(GPIO_ALARM_LIGHT, state ? IOT_GPIO_VALUE1 : IOT_GPIO_VALUE0);
    last_state = state;
}
