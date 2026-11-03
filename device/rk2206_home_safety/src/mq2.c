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

#include "mq2.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <math.h>

#include "iot_errno.h"
#include "iot_adc.h"

#define CAL_PPM 20 // 校准环境中PPM值
#define RL 1       // RL阻值

static float m_r0; // 元件在干净空气中的阻值

#define MQ2_ADC_CHANNEL 4

/***************************************************************
* 函数名称: mq2_dev_init
* 说    明: 初始化ADC
* 参    数: 无
* 返 回 值: 0为成功，反之为失败
***************************************************************/
unsigned int mq2_dev_init(void)
{
    unsigned int ret = 0;

    ret = IoTAdcInit(MQ2_ADC_CHANNEL);

    if(ret != IOT_SUCCESS)
    {
        printf("%s, %s, %d: ADC Init fail\n", __FILE__, __func__, __LINE__);
    }

    return 0;
}

/***************************************************************
* 函数名称: Get_Voltage
* 说    明: 获取ADC电压值
* 参    数: 无
* 返 回 值: 电压值
***************************************************************/
static float adc_get_voltage(void)
{
    unsigned int ret = IOT_SUCCESS;
    unsigned int data = 0;

    ret = IoTAdcGetVal(MQ2_ADC_CHANNEL, &data);

    if (ret != IOT_SUCCESS)
    {
        printf("%s, %s, %d: ADC Read Fail\n", __FILE__, __func__, __LINE__);
        return 0.0;
    }

    return (float)(data * 3.3 / 1024.0);
}

/***************************************************************
 * 函数名称: mq2_ppm_calibration
 * 说    明: 传感器校准函数
 * 参    数: 无
 * 返 回 值: 无
 ***************************************************************/
void mq2_ppm_calibration(void) 
{
  float voltage = adc_get_voltage();
  float rs = (5 - voltage) / voltage * RL;

  m_r0 = rs / powf(CAL_PPM / 613.9f, 1 / -2.074f);
}

/***************************************************************
 * 函数名称: get_mq2_ppm
 * 说    明: 获取PPM函数
 * 参    数: 无
 * 返 回 值: ppm
 ***************************************************************/
float get_mq2_ppm(void) 
{
  float voltage, rs, ppm;

  voltage = adc_get_voltage();
  rs = (5 - voltage) / voltage * RL;      // 计算rs
  ppm = 613.9f * powf(rs / m_r0, -2.074f); // 计算ppm
  return ppm;
}
