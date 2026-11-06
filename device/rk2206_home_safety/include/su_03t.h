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
 * 报警/应答播报（单字节帧）： AA 55 <index> <dat> 55 AA，index 见下方 SU03T_SAY_*
 * 数值播报（双精度帧）： AA 55 <index> <8 字节 double(小端)> 55 AA，
 *                       index 见下方 SU03T_VAL_IDX_*
 * 板子接收指令（2 字节）：<前缀> <参数>，见下方 SU03T_PFX_* 与指令表
 */

/* 数值播报 index（与实验手册09 的 SU-03T 配置一致：1=温度 2=湿度 3=光照） */
#define SU03T_VAL_IDX_TEMP      1
#define SU03T_VAL_IDX_HUMI      2
#define SU03T_VAL_IDX_LIGHT     3   /* 模块中 3 是“光照”，不是燃气 */

/* 板子接收的出厂查询指令字（保留兼容，实际解析见 su_03t.c） */
#define SU03T_CMD_TEMP_GET      0x0301
#define SU03T_CMD_HUMI_GET      0x0302
#define SU03T_CMD_GAS_GET       0x0303

/* ---------------- 模块 → 主板：两字节指令字 ----------------
 * 模块侧在 SU-03T 平台给每个词条配置“串口输出”为本表的两字节即可，
 * 主板对参数做范围校验，非法参数直接丢弃，不会误触发。
 * 0x00 / 0x01 / 0x03 为出厂遗留前缀，继续兼容：
 *   00 01 灯光自动   00 02 灯光关闭
 *   01 01 开灯       01 02 关灯
 *   03 01 温度       03 02 湿度   （03 03 出厂是光照，主板不响应）
 */
#define SU03T_PFX_HELP          0x10    /* 01 紧急呼救  02 解除求助/报警确认 */
#define SU03T_PFX_MUTE          0x11    /* 01 静音报警  02 取消静音 */
#define SU03T_PFX_MED           0x12    /* 01~03 确认第1~3次服药  04 确认下一次待服药 */
#define SU03T_PFX_LIGHT         0x13    /* 01 开灯  02 关灯  03 灯光自动 */
#define SU03T_PFX_QUERY         0x14    /* 01 温度 02 湿度 03 燃气 04 光照 05 温湿度连播 */
#define SU03T_PFX_PAGE          0x15    /* 01 记录一页  02 记录二页  03 返回主页 */

/* 主板 → 模块：语音应答词条编号（10 起，与报警 4~9 不冲突）。
 * 需在模块上按本表配置 10~22 号固定句，未配置时发送会被模块忽略。 */
#define SU03T_SAY_LIGHT_ON      10      /* 已开灯 */
#define SU03T_SAY_LIGHT_OFF     11      /* 已关灯 */
#define SU03T_SAY_LIGHT_AUTO    12      /* 灯光已切到自动 */
#define SU03T_SAY_SOS           13      /* 已发出紧急求助 */
#define SU03T_SAY_ACK           14      /* 已解除求助 */
#define SU03T_SAY_MUTE          15      /* 已静音报警 */
#define SU03T_SAY_UNMUTE        16      /* 已恢复播报 */
#define SU03T_SAY_MED_OK        17      /* 服药已确认 */
#define SU03T_SAY_MED_NONE      18      /* 现在没有待确认的服药 */
#define SU03T_SAY_NO_DATA       19      /* 数据暂不可用，请稍后再试 */
#define SU03T_SAY_GAS_BAD       20      /* 燃气浓度偏高，请开窗通风 */
#define SU03T_SAY_GAS_OK        21      /* 燃气未检出异常 */
#define SU03T_SAY_BUSY          22      /* 设备正忙，请稍后再试 */

/* 语音应答词条开关：跟随报警专用词条开关，避免工厂模块把应答播成温湿度。
 * 模块配好 10~22 号词条后，把 VOICE_DEDICATED_IDX 置 1 即可同时启用。 */
#ifndef SU03T_ACTION_VOICE_ENABLED
#define SU03T_ACTION_VOICE_ENABLED SU03T_ALARM_PROFILE_ENABLED
#endif

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

/* 最近一条语音动作的界面提示（3 秒内有效），有则返回 1 并写入 out */
int su03t_note(char *out, int n, uint64_t now);

/* 取待执行的界面切换请求（0~4），无请求返回 -1 */
int su03t_take_page(void);

#endif /* __SU_03T_H__ */
