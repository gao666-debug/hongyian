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

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "safety_runtime.h"
#include "ui.h"
#include "los_task.h"

#include "iot_i2c.h"
#include "iot_errno.h"

/* sht30对应i2c */
#define SHT30_I2C_PORT EI2C0_M2

/* sht30地址 */
#define SHT30_I2C_ADDRESS      0x44

/***************************************************************
* 函数名称: sht30_init
* 说    明: 初始化SHT30，设置测量周期
* 参    数: 无
* 返 回 值: 无
***************************************************************/
void sht30_init(void)
{
    uint32_t ret = 0;
    uint8_t send_data[2] = {0x22, 0x36};
    uint32_t send_len = 2;

    ui_sensor_progress("SHT-LOCK");
    if (!safety_i2c_try_lock(200)) return;
    if (safety_i2c_prepare_locked() != 0)
    {
        safety_i2c_unlock();
        return;
    }

    ret = IoTI2cWrite(SHT30_I2C_PORT, SHT30_I2C_ADDRESS, send_data, send_len);
    safety_i2c_result_locked(ret == IOT_SUCCESS);
    safety_i2c_unlock();
    if (ret != IOT_SUCCESS) printf("[sht30] measurement setup failed rc=%u\r\n", ret);
}

/***************************************************************
* 函数名称: sht30_calc_RH
* 说    明: 湿度计算
* 参    数: u16sRH：读取到的湿度原始数据
* 返 回 值: 计算后的湿度数据
***************************************************************/
static float sht30_calc_RH(uint16_t u16sRH)
{
    float humidityRH = 0;
    
    /*calculate relative humidity [%RH]*/
    /*RH = rawValue / (2^16-1) * 10*/
    humidityRH = (100 * (float)u16sRH / 65535);

    return humidityRH;
}


/***************************************************************
* 函数名称: sht30_calc_temperature
* 说    明: 温度计算
* 参    数: u16sT：读取到的温度原始数据
* 返 回 值: 计算后的温度数据
***************************************************************/
static float sht30_calc_temperature(uint16_t u16sT)
{
    float temperature = 0;

    /*calculate temperature [℃]*/
    /*T = -45 + 175 * rawValue / (2^16-1)*/
    temperature = (175 * (float)u16sT / 65535 - 45);

    return temperature;
}

/***************************************************************
* 函数名称: sht30_check_crc
* 说    明: 检查数据正确性
* 参    数: data：读取到的数据
            nbrOfBytes：需要校验的数量
            checksum：读取到的校对比验值
* 返 回 值: 校验结果，0-成功 1-失败
***************************************************************/
static uint8_t sht30_check_crc(uint8_t *data, uint8_t nbrOfBytes, uint8_t checksum)
{
    uint8_t crc = 0xFF;
    uint8_t bit = 0;
    uint8_t byteCtr ;
    const int16_t POLYNOMIAL = 0x131;

    /*calculates 8-Bit checksum with given polynomial*/
    for(byteCtr = 0; byteCtr < nbrOfBytes; ++byteCtr)
    {
        crc ^= (data[byteCtr]);
        for ( bit = 8; bit > 0; --bit)
        {
            if (crc & 0x80) crc = (crc << 1) ^ POLYNOMIAL;
            else crc = (crc << 1);
        }
    }

    if(crc != checksum)
        return 1;
    else
        return 0;
}

/***************************************************************
* 函数名称: sht30_read_data
* 说    明: 读取温度、湿度
* 参    数: dat：读取到的数据 0: 温度 1: 湿度
* 返 回 值: 无
***************************************************************/
static int sht30_read_once(double *dat)
{
    /*checksum verification*/
    uint8_t data[3];
    uint16_t tmp;
    uint8_t rc;
    /*byte 0,1 is temperature byte 4,5 is humidity*/
    uint8_t SHT30_Data_Buffer[6];
    memset(SHT30_Data_Buffer, 0, 6);
    uint8_t send_data[2] = {0xE0, 0x00};

    uint32_t send_len = 2;
    uint32_t receive_len = 6;
    ui_sensor_progress("SHT-LOCK");
    if (!safety_i2c_try_lock(200)) return -1;
    ui_sensor_progress("SHT-BUS");
    if (safety_i2c_prepare_locked() != 0) { safety_i2c_unlock(); return -1; }
    ui_sensor_progress("SHT-WR");
    uint32_t wr = IoTI2cWrite(SHT30_I2C_PORT, SHT30_I2C_ADDRESS, send_data, send_len);
    ui_sensor_progress("SHT-RD");
    uint32_t rd = wr == IOT_SUCCESS ? IoTI2cRead(SHT30_I2C_PORT, SHT30_I2C_ADDRESS, SHT30_Data_Buffer, receive_len) : wr;
    safety_i2c_result_locked(wr == IOT_SUCCESS && rd == IOT_SUCCESS);
    safety_i2c_unlock();
    ui_sensor_progress("SHT-CRC");
    if (wr != IOT_SUCCESS || rd != IOT_SUCCESS || sht30_check_crc(SHT30_Data_Buffer,2,SHT30_Data_Buffer[2]) || sht30_check_crc(SHT30_Data_Buffer+3,2,SHT30_Data_Buffer[5])) return -1;

    /*check temperature*/
    data[0] = SHT30_Data_Buffer[0];
    data[1] = SHT30_Data_Buffer[1];
    data[2] = SHT30_Data_Buffer[2];
    rc = sht30_check_crc(data, 2, data[2]);
    if(!rc)
    {
        tmp = ((uint16_t)data[0] << 8) | data[1];
        dat[0] = sht30_calc_temperature(tmp);
    }
    
    /*check humidity*/
    data[0] = SHT30_Data_Buffer[3];
    data[1] = SHT30_Data_Buffer[4];
    data[2] = SHT30_Data_Buffer[5];
    rc = sht30_check_crc(data, 2, data[2]);
    if(!rc)
    {
        tmp = ((uint16_t)data[0] << 8) | data[1];
        dat[1] = sht30_calc_RH(tmp);
    }
    return 0;
}

/* Bounded retry, with the shared bus released during the backoff. Output values
 * are changed only after a complete successful frame with both CRCs valid. */
int sht30_read_data(double *dat)
{
    if (!dat) return -1;
    for (int attempt = 0; attempt < 2; ++attempt) {
        double sample[2];
        if (sht30_read_once(sample) == 0) {
            dat[0] = sample[0]; dat[1] = sample[1]; return 0;
        }
        if (attempt == 0) LOS_Msleep(25);
    }
    return -1;
}
