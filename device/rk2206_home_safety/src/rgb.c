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

#include "rgb.h"

#include "iot_pwm.h"

/* 板载 5050 RGB 对应 PWM 通道（与 b9_rgb_led 例程一致） */
#define LED_R_PORT      EPWMDEV_PWM1_M1
#define LED_G_PORT      EPWMDEV_PWM7_M1
#define LED_B_PORT      EPWMDEV_PWM0_M1

#define RGB_PWM_FREQ    1000

/* 占空比换算到 1~99（PWM 要求） */
static int duty_of(uint8_t v)
{
    int d = (int)(v * 99 / 255);

    if (d < 1)
    {
        d = 1;
    }
    if (d > 99)
    {
        d = 99;
    }
    return d;
}

void rgb_dev_init(void)
{
    IoTPwmInit(LED_R_PORT);
    IoTPwmInit(LED_G_PORT);
    IoTPwmInit(LED_B_PORT);
    rgb_set_color(0, 0, 0);
}

void rgb_set_color(uint8_t r, uint8_t g, uint8_t b)
{
    if(r) IoTPwmStart(LED_R_PORT, duty_of(r), RGB_PWM_FREQ); else IoTPwmStop(LED_R_PORT);
    if(g) IoTPwmStart(LED_G_PORT, duty_of(g), RGB_PWM_FREQ); else IoTPwmStop(LED_G_PORT);
    if(b) IoTPwmStart(LED_B_PORT, duty_of(b), RGB_PWM_FREQ); else IoTPwmStop(LED_B_PORT);
}
