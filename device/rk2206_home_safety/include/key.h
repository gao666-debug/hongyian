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

#ifndef __KEY_H__
#define __KEY_H__

#include <stdbool.h>

/* 通晓板 4 个用户按键（ADC 电阻分压网络，通道 7 / GPIO0_PC7） */
typedef enum
{
    KEY_NONE = 0,
    KEY_UP,       /* K3 */
    KEY_DOWN,     /* K4 */
    KEY_LEFT,     /* K5 */
    KEY_RIGHT,    /* K6 */
} key_id_t;

/* 初始化按键并启动独立扫描线程（50ms 周期 + 去抖） */
void key_dev_init(void);

/* 取一次短按事件：K6 松开时触发且长按不触发短按；其他键仍在按下时触发。 */
key_id_t key_take_event(void);

/* 取一次“长按”事件（按住≥KEY_LONG_PRESS_MS，只上报一次），无则返回 KEY_NONE */
key_id_t key_take_long_event(void);

/* 最近一次按键 ADC 电压(V)，调试用 */
float key_last_voltage(void);

#endif /* __KEY_H__ */
