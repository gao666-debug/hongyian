/*
 * Copyright (c) HiSilicon (Shanghai) Technologies Co., Ltd. 2022. All rights reserved.
 *
 * Description: SLE private service register sample of client.
 */

/**
 * @defgroup SLE UUID CLIENT API
 * @ingroup
 * @{
 */

#include "sle_ssap_client.h"

#ifndef SLE_CLIENT_ADV_H
#define SLE_CLIENT_ADV_H

/**
 * @if Eng
 * @brief  sle uuid client init.
 * @attention  NULL
 * @retval ERRCODE_SLE_SUCCESS    Excute successfully
 * @retval ERRCODE_SLE_FAIL       Execute fail
 * @par Dependency:
 * @li NULL
 * @else
 * @brief  sle uuid客户端初始化。
 * @attention  NULL
 * @retval ERRCODE_SLE_SUCCESS    执行成功
 * @retval ERRCODE_SLE_FAIL       执行失败
 * @par 依赖:
 * @li NULL
 * @endif
 */
void sle_client_init(ssapc_notification_callback notification_cb, ssapc_indication_callback indication_cb);

/**
 * @if Eng
 * @brief  sle start scan.
 * @attention  NULL
 * @retval ERRCODE_SLE_SUCCESS    Excute successfully
 * @retval ERRCODE_SLE_FAIL       Execute fail
 * @par Dependency:
 * @li NULL
 * @else
 * @brief  sle启动扫描。
 * @attention  NULL
 * @retval ERRCODE_SLE_SUCCESS    执行成功
 * @retval ERRCODE_SLE_FAIL       执行失败
 * @par 依赖:
 * @li NULL
 * @endif
 */
void sle_start_scan(void);

/* 启动星闪接收：扫手表、连接、收 JSON 并解析 */
void sle_rx_start(void);

/* 取手表最新一帧，返回 0 成功；age_ms 为距上次收到的毫秒数 */
int sle_rx_get_watch(int *hr, int *spo2, int *steps, int *fall, uint32_t *age_ms);

#endif