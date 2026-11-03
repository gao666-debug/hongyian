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

#ifndef __MQTT_CLIENT_H__
#define __MQTT_CLIENT_H__

#include <stdbool.h>

/* 启动 MQTT 上云（板子切 STA 连路由器 → 连云 broker → 周期性发布状态） */
void mqtt_start(void);
void mqtt_config_init(void);
int mqtt_connect_wifi(void);
void mqtt_queue_info(unsigned int *pending,unsigned int *dropped);

/* 更新待发布的数据（监测线程周期调用） */
void mqtt_update(double temp, double humi, double gas, double light, bool body,
                 unsigned int inactive_ms, int alarm, int status, int smoke);

/* 将当前状态入队，告警开始或解除时调用；本地事件历史独立保存。 */
void mqtt_alarm(int code, int value);
void mqtt_alarm_event(int code, int value, unsigned int id, int ended, int duration);

/* Wi-Fi 名/密码（供开机自动选模式判断能否连上路由器） */
const char *mqtt_wifi_ssid(void);
const char *mqtt_wifi_pass(void);


/* Cloud identity used by the active build. */
const char *mqtt_device_id(void);

#endif /* __MQTT_CLIENT_H__ */
