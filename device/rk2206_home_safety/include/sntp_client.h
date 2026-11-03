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

#ifndef __SNTP_CLIENT_H__
#define __SNTP_CLIENT_H__

/* 启动网络对时（后台线程，联网后自动同步 UTC 时间） */
void sntp_start(void);

/* 取当前 UTC Unix 时间(秒)。未同步成功前返回 0 */
unsigned int sntp_get_epoch(void);

#endif /* __SNTP_CLIENT_H__ */
