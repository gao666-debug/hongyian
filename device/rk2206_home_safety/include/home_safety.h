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

#ifndef __HOME_SAFETY_H__
#define __HOME_SAFETY_H__

#include <stdbool.h>

/* ======================= 可调参数 ======================= */

/* 长时间无活动判定阈值（毫秒）。
 * 演示建议 60 秒；实际部署建议 30 分钟：30 * 60 * 1000UL */
#define NO_ACTIVITY_TIMEOUT_MS      (60 * 1000UL)

/* 环境异常阈值 */
#define TEMP_HIGH_LIMIT             35.0    /* 温度上限（℃） */
#define TEMP_LOW_LIMIT              10.0    /* 温度下限（℃） */
#define HUMI_HIGH_LIMIT             80.0    /* 湿度上限（%RH） */
#define HUMI_LOW_LIMIT              20.0    /* 湿度下限（%RH） */

/* 燃气泄漏阈值（ppm） */
#define GAS_PPM_LIMIT               800.0

/* 烟雾/浓烟阈值（ppm，MQ 传感器同源，用于“烟雾监测”维度） */
#define SMOKE_PPM_LIMIT             2000.0

/* Legacy prototype thresholds retained for tests/history compatibility.
 * The fixed gateway does not use board motion to detect human falls.
 * Do not drop or throw the development board to demonstrate this feature. */
#define FALL_FREEFALL_G             0.6f
#define FALL_WINDOW_MS              1500UL
#define FALL_IMPACT_G               2.5f
#define FALL_CONFIRM_MS             (5 * 1000UL)

/* 报警后蜂鸣器自动静音时长（毫秒）：超过此时长停止蜂鸣，
 * 但报警状态、报警灯、LCD 显示与语音保持，避免长时间扰民。 */
#define ALARM_MUTE_MS               (30 * 1000UL)
#define ACCEL_LSB_PER_G             2048.0f /* MPU6050 ±16g 量程：2048 LSB/g */

/* 语音播报帧 index。
 * VOICE_DEDICATED_IDX = 0：关闭报警语音，避免工厂数值词条把报警播成温度0等；蜂鸣器保留。
 * VOICE_DEDICATED_IDX = 1：使用专用报警词条 4~8（需先在 SU-03T 平台配好 4~8 词条）*/
#ifndef VOICE_DEDICATED_IDX
#define VOICE_DEDICATED_IDX         0
#endif

#if VOICE_DEDICATED_IDX
#define VOICE_IDX_NO_ACTIVITY       4       /* 长时间无活动 */
#define VOICE_IDX_ENV_ABNORMAL      5       /* 环境异常 */
#define VOICE_IDX_GAS               6       /* 燃气泄漏 */
#define VOICE_IDX_FALL              7       /* 疑似跌倒 */
#define VOICE_IDX_HELP              8       /* 紧急求助 */
#else
#define VOICE_IDX_NO_ACTIVITY       1       /* 长时间无活动（模块：温度模板） */
#define VOICE_IDX_ENV_ABNORMAL      2       /* 环境异常（模块：湿度模板） */
#define VOICE_IDX_GAS               3       /* 燃气泄漏（模块：光照模板） */
#define VOICE_IDX_FALL              4       /* 疑似跌倒 */
#define VOICE_IDX_HELP              5       /* 紧急求助 */
#endif

/* 外接传感器开关：硬件未接入时置 0，避免悬空引脚误读导致误报。
 * 接好门窗磁 / 无线按钮后再置 1 并重新编译。 */
#define DOOR_SENSOR_ENABLED         0 /* Rollback: no external door sensor connected. */
#define WIRELESS_BTN_ENABLED        0 /* Receiver disconnected: local keys restored. */
#define KEY_ADC_DIAGNOSTIC          0 /* Display raw ADC; suppress local/remote button actions. */

/* Legacy compatibility flag, network operation uses AP/STA exclusive modes. */
#define WIFI_COEXIST                1

/* Wi-Fi(AP) + HTTP 服务开关（供北向 App 局域网轮询）；不需要则置 0 */
#define NET_SERVER_ENABLED          1

/* NFC 标签（写“求助/信息”链接）开关 */
#define NFC_TAG_ENABLED             1

/* MQTT allowed only in STA mode. Runtime settings: /config; IoTDA is deferred. */
#define MQTT_ENABLED                1

/* ======================= 设备封装 ======================= */

/* 人体感应（热释电红外） */
void body_induction_dev_init(void);
void body_induction_get_state(bool *dat);
typedef struct {
    int raw; /* -1 unavailable, 0 low, 1 high */
    unsigned int init_rc, read_rc, high_seconds, changes;
    unsigned int gpio_rc, mux_rc, dir_rc, samples;
    int init_called, filtered_valid, filtered_active;
} body_diagnostics_t;
void body_induction_diagnostics(body_diagnostics_t *out);

/* 蜂鸣器（无源，PWM 驱动） */
void beep_dev_init(void);
void beep_set_state(bool state);

/* 报警灯 */
void alarm_light_init(void);
void alarm_light_set_gpio(bool state);

#endif /* __HOME_SAFETY_H__ */
