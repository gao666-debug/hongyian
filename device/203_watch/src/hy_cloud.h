/*
 * hy_cloud.h —— 华为云 IoTDA 上云任务
 *
 * 定位：一个独立任务，只做「读 g_hy -> 组 JSON -> MQTT 发出去」，
 * 不碰 I2C，所以不会干扰 199Hz 的采样。
 */
#ifndef HY_CLOUD_H
#define HY_CLOUD_H

#ifdef __cplusplus
extern "C" {
#endif

/* 上云任务主体（WiFi + SNTP + MQTT 全都在这一个任务里跑） */
void hy_cloud_task(void);

/* 供主程序查询：1 = 已连上华为云 */
int hy_cloud_online(void);

#ifdef __cplusplus
}
#endif

#endif /* HY_CLOUD_H */
