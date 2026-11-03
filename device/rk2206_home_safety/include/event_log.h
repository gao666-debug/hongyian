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

#ifndef __EVENT_LOG_H__
#define __EVENT_LOG_H__

typedef struct {
 unsigned int id,boot,up,epoch; char tag[16]; int val,dur;
} safety_event_t;
int event_log_snapshot(safety_event_t entries[16]);
int event_log_entry(int idx,safety_event_t *entry);
unsigned int event_log_last_id(void);

/* 事件本地存储（板载 Flash 的 KV 存储，环形记录最近 16 条）。
 * 存储格式： "up=<开机秒>s <TAG> <值> e=<epoch> d=<持续秒>" */

void event_log_init(void);

/* 记录一条事件，返回持久事件 ID（用于之后更新时长） */
int event_log_add(const char *tag, int value);

/* 按事件 ID 更新持续时长；已覆盖或清空的事件不再更新 */
void event_log_set_duration(int slot, int dur_sec);

/* 清空所有事件记录 */
void event_log_clear(void);

int event_log_slots(void);

/* 取第 idx 条事件（0=最新），成功返回 1，buf 写入文本 */
int event_log_get(int idx, char *buf, int buflen);

/* TAG 的中文名（HELP/NOACT/GAS/FALL/ENV/DOOR；未知 tag 原样返回） */
const char *event_log_tag_cn(const char *tag);

#endif /* __EVENT_LOG_H__ */
