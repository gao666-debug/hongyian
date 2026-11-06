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

#ifndef __NET_SERVER_H__
#define __NET_SERVER_H__

#include <stdbool.h>

/*
 * 板子 Wi-Fi(AP 模式) + 轻量 HTTP 服务
 * 手机连板子热点后，App 轮询 http://192.168.2.1:8080/status 获取 JSON
 * 可通过 NET_SERVER_ENABLED 开关（默认 1）
 */
void net_server_start(void);
void net_server_local_ip(char *ip, int size);

/* 更新对外发布的实时数据（由监测线程周期性调用） */
void net_server_update(double temp, double humi, double gas_ppm, bool body,
                       unsigned int inactive_ms, int alarm, int status,
                       int help, int fall, int gas_alarm, int door,
                       double light, int smoke);

/* 统计回调（供 /stats）：由监测线程在事件发生时调用 */
void net_server_on_activity(void);          /* body 0->1：活动总数 + 当前小时桶 */
void net_server_on_alarm(int status_code);  /* 报警上升沿：总数 + 按 status(1..5) 类型计数 */

/* 远程命令（供 App 事件闭环）：/ack 远程确认、/mute 远程消音 */
#define NET_CMD_NONE   0
#define NET_CMD_ACK    1
#define NET_CMD_MUTE   2
int net_server_take_cmd(void);   /* 取走并清零远程命令，返回 NET_CMD_* */

/* 网络模式：AP=板子热点+HTTP（App 直连 192.168.2.1）；STA=连路由器上云（HTTP 走 STA IP）。
 * 本板射频不支持 AP+STA 同时工作，故每次只启用一种。 */
#define NET_MODE_AP     0
#define NET_MODE_STA    1
void net_set_mode(int mode);
int  net_get_mode(void);

#endif /* __NET_SERVER_H__ */
