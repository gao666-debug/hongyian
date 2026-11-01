/*
 * hy_sensor.h —— 加速度 + 心率血氧 点亮层（自诊断版）
 *
 * 与旧 watch_sensor.c 的区别：
 *   1. 上电先量 SCL/SDA 空闲电平，再决定要不要初始化总线（旧版直接 init，
 *      总线被拉死时只能看到一个 0x80001313，不知道是线的问题还是器件的问题）。
 *   2. 三组 I2C 引脚组合自动尝试，不再写死一组。
 *   3. MPU6050 同时试 0x68 / 0x69（ADO 在 J2-7，拉高就是 0x69）。
 *   4. MAX30100 / MAX30102 两套寄存器映射都实现，按 PART_ID 选。
 */
#ifndef HY_SENSOR_H
#define HY_SENSOR_H

/* ---------------- 运行状态 ---------------- */
typedef struct {
    /* 总线探测结果，-1 = 未确定 */
    int bus;
    int scl_pin;
    int sda_pin;
    int bus_ok;          /* 1 = 总线可用（至少能完成事务或收到 ACK） */

    /* 上电瞬间的线电平：1 高 / 0 低 / -2 切不回 GPIO */
    int scl_idle;
    int sda_idle;
    int sda_dead;        /* 1 = SDA 被外部拉死，且 9 时钟恢复无效 */

    /* 器件 */
    int mpu_ok;
    int mpu_addr;        /* 0x68 或 0x69 */
    int max_ok;
    int max_part;        /* 0x11 = MAX30100 / 0x15 = MAX30102 */

    /* 数据 */
    int ax;
    int ay;
    int az;              /* 单位 mg，1g = 1000mg */
    int steps;
    int hr;              /* bpm，0 = 未测到 */
    int spo2;            /* %，0 = 未测到 */
    int ir_pct;          /* 脉搏波形归一化幅度 0..100 */
    int samples;         /* 本轮心率缓冲里已有点数 */

    /* 摔倒检测 */
    int fall_state;      /* 0=正常 1=冲击后观察中 2=疑似摔倒 */
    int fall_events;     /* 累计判定次数 */
    int fall_peak_mg;    /* 本次冲击峰值（mg），便于回看力度 */
    int fall_still_ms;   /* 冲击后已静止的时长（ms） */
    int acc_peak;        /* 最近一个打印周期内的合加速度峰值（mg），标定用 */
} hy_sensor_state_t;

extern hy_sensor_state_t g_hy;

/* 传感器任务主体（内含总线探测与采样死循环） */
void hy_sensor_task(void);

#endif /* HY_SENSOR_H */
