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

#ifndef __DOOR_H__
#define __DOOR_H__

#include <stdbool.h>

/* KY-025 powered module: + to 3.3V, G to GND, DO to B6,
 * AO not connected. Verified on this module: magnet near = high/closed, away = low/open. */
void door_dev_init(void);
/* Returns false until debounced, or on GPIO failure; leaves *open untouched. */
bool door_read(unsigned long long now_ms, bool *open);
bool door_is_open(void);

#endif /* __DOOR_H__ */
