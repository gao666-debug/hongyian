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

#ifndef __SU_03T_H__
#define __SU_03T_H__

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "home_safety.h"
/* Factory profile indices 1..3 are numeric queries, not alarm phrases.
 * Enable only after installing and validating the custom alarm vocabulary. */
#ifndef SU03T_ALARM_PROFILE_ENABLED
#define SU03T_ALARM_PROFILE_ENABLED VOICE_DEDICATED_IDX
#endif

/* ================= 语音模块帧约定（需与 SU-03T 配置一致） =================
 *
 * 报警播报（单字节帧）： AA 55 <index> <dat> 55 AA，index 见 home_safety.h 的 VOICE_IDX_*
 * 数值播报（双精度帧）： AA 55 <index> <8 字节 double(小端)> 55 AA，
 *                       index 见下方 SU03T_VAL_IDX_*
 * 板子接收指令（2 字节）：<hi> <lo>，见下方 SU03T_CMD_*_GET
 */

/* 数值播报 index（与实验手册09 的 SU-03T 配置一致：1=温度 2=湿度 3=光照） */
#define SU03T_VAL_IDX_TEMP      1
#define SU03T_VAL_IDX_HUMI      2
#define SU03T_VAL_IDX_GAS       3   /* 模块中 3 是“光照”，用燃气需在模块里另配模板 */

/* 板子接收到的查询指令字（需在 SU-03T 平台配置对应命令词发送此两字节） */
#define SU03T_CMD_TEMP_GET      0x0301
#define SU03T_CMD_HUMI_GET      0x0302
#define SU03T_CMD_GAS_GET       0x0303

/* Requires separately verified vocabulary index 9; off by default. */
#ifndef SU03T_MEDICINE_PROFILE_ENABLED
#define SU03T_MEDICINE_PROFILE_ENABLED 0
#endif
void su03t_process_voices(const bool alarms[5], unsigned medicine_mask,
                         int medicine_audible, int muted, uint64_t now);
void su03t_init(void);

/* 发送报警播报帧（单字节数据） */
void su03t_send_u8_msg(uint8_t index, uint8_t dat);

/* 发送数值播报帧（double 数据） */
void su03t_send_double_msg(uint8_t index, double dat);

/* Consume a UART stream, retaining split queries and handling concatenation. */
void su03t_receive_bytes(const uint8_t *data, size_t len);

#endif /* __SU_03T_H__ */
