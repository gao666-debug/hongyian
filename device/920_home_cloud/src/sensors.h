/**
 * 传感器采集：光敏 / 烟雾（ADC）、人体感应 / 门磁（GPIO）
 *
 * 设计约定：
 *  - 只有一个写者（sensor_task），其他任务通过 sensors_get() 读快照
 *  - 任一状态变化会置位事件标志，由上报任务消费，实现"事件立即上报"
 */

#ifndef HOME_CLOUD_SENSORS_H
#define HOME_CLOUD_SENSORS_H

#include "td_type.h"

typedef struct {
    td_u16 light_mv;      /* 光敏 ADC 电压，mV */
    td_u16 smoke_mv;      /* 烟雾 ADC 电压，mV */
    td_u8  pir;           /* 1=有人 0=无人 */
    td_u8  door_open;     /* 1=开 0=关 */
    td_u8  door_valid;    /* 门磁是否已有有效读数 */
} sensors_snapshot_t;

/* 初始化引脚与 ADC，并创建采集任务。必须在有调度器的任务里调用。 */
td_void sensors_start(td_void);

/* 取当前快照，可多任务并发调用 */
td_void sensors_get(sensors_snapshot_t *out);

/* 取并清除事件标志：返回 1 表示自上次调用以来有变化 */
td_u8 sensors_take_event(td_void);

#endif /* HOME_CLOUD_SENSORS_H */
