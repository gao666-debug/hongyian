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

#ifndef __RGB_H__
#define __RGB_H__

#include <stdint.h>

/* 板载 5050 RGB 灯（PWM 驱动） */
void rgb_dev_init(void);

/* 设置颜色，取值 0~255 */
void rgb_set_color(uint8_t r, uint8_t g, uint8_t b);

#endif /* __RGB_H__ */
