/**
 * 华为云 IoTDA 上报
 *
 * 两条 Topic 同时发：
 *   1) $oc/devices/{id}/sys/properties/report  → 控制台「最新上报数据」可见，答辩演示用
 *   2) $oc/devices/{id}/sys/messages/up        → 自建后端只认这条（device.message 转发）
 *
 * 注意：设备**无法订阅** properties/report/response（平台返回 0x80），
 * 所以拿不到平台对属性上报的校验回执，属性对不上时只能看控制台「消息跟踪」。
 */

#ifndef HOME_CLOUD_IOTDA_H
#define HOME_CLOUD_IOTDA_H

#include "td_type.h"
#include "sensors.h"

/* 连接 IoTDA 并订阅下行命令；成功返回 0 */
td_s32 iotda_connect(td_void);

/* 上报一次：属性 + device_health 消息。返回 0 表示两条都发成功 */
td_s32 iotda_report(const sensors_snapshot_t *s);

#endif /* HOME_CLOUD_IOTDA_H */
