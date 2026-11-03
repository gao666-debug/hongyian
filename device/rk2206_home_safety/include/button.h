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

#ifndef __BUTTON_H__
#define __BUTTON_H__

#include <stdbool.h>

/* Single channel: +V -> 3.3V, GND -> GND, D0 -> B7.
 * Momentary mode; release after boot, debounce 50ms, one event per press.
 * Monitor resolves event: active HELP -> ACK; pending dose -> confirm; otherwise SOS.
 * D1/D2/D3/VT are disconnected. */
#define WIRELESS_SOS 1U
#define WIRELESS_ACK 2U
#define WIRELESS_MUTE 4U
#define WIRELESS_PAGE 8U
void button_dev_init(void);
/* 扫描任务保留事件，监测任务一次性取走；同时到达时应优先求助。 */
unsigned int button_take_events(void);
#endif
