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

#ifndef __UI_H__
#define __UI_H__

#include <stdbool.h>
#include "living.h"
#include "safety_runtime.h"
/* Publish only a latest-frame snapshot; never performs SPI I/O. */
int ui_start(void);
void ui_present(int page,const safety_state_t *state,const char *status,
                const living_config_t *draft,int field,int result,int events_changed);
#ifdef SAFETY_HOST_TEST
void ui_render_pending(void);
#endif
void ui_show_living(const living_config_t *draft,int field,int result);

/* 初始化 LCD 并绘制主界面静态部分 */
void ui_init(void);
/* Stage string must have static lifetime; watchdog diagnostics only. */
void ui_sensor_progress(const char *stage);
void ui_show_diagnostics(void);

/* 刷新主界面动态数据（两栏 10 项） */
void ui_update(double temp, double humi, bool body, double gas_ppm, double light,
               bool smoke, bool fall_suspect, bool help_on, bool door_open,
               bool alarmed, const char *status);

/* 重绘主界面（从事件页切回时用） */
void ui_show_main(void);

/* 显示事件记录页（page: 0=第1页 1=第2页，每页 8 条） */
void ui_show_events(int page);

#endif /* __UI_H__ */
