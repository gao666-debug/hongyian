/*
 * 手表扩展模块 - 传感器层对外接口
 *
 * 硬件（HTXH_XS_WEAR_SCH V0）：
 *   MPU6050   3 轴加速度计   I2C1  addr 0x68
 *   MAX3010x  心率血氧       I2C1  addr 0x57   (原理图丝印 MAX30100)
 *   I2C1: SCL=GPIO16(UART1_TXD, mode2)  SDA=GPIO15(UART1_RXD, mode2)
 *
 * 采样节奏：10ms 一次
 *   - MAX3010x 每 10ms 取 1 组 (red, ir)，即 100Hz，正好匹配 algorithm.h 的 FS=100
 *   - MPU6050  每 20ms 取 1 组 (ax, ay, az)，即 50Hz
 */
#ifndef __WATCH_SENSOR_H__
#define __WATCH_SENSOR_H__

#define WATCH_PAGE_NUM 3

typedef struct {
    int acc_ok;      /* 1 = MPU6050 初始化成功 */
    int hr_ok;       /* 1 = MAX3010x 初始化成功 */
    int ax, ay, az;  /* 加速度，单位 mg（1g = 1000mg） */
    int steps;       /* 计步 */
    int hr;          /* 心率 bpm，0 = 未测到 */
    int spo2;        /* 血氧 %，0 = 未测到 */
    int ir_pct;      /* IR 信号归一化 0..100，用来画脉搏柱 */
    int been_on;     /* 上电秒数 */
} watch_state_t;

extern watch_state_t g_watch;

/* 传感器任务主体，由 main.c 挂到内核线程 */
void watch_sensor_task(void);

#endif
