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
#include "mpu6050.h"
#include "iot_errno.h"
#include "iot_i2c.h"
#include "los_task.h"
#include "safety_runtime.h"
#include <stdint.h>
#include <stdio.h>
#define MPU_PORT EI2C0_M2
static int read_regs(uint8_t reg, uint8_t *data, unsigned int n)
{
    if (!safety_i2c_try_lock(200)) return -1;
    if (safety_i2c_prepare_locked() != 0) { safety_i2c_unlock(); return -1; }
    unsigned int rc = IoTI2cWrite(MPU_PORT, MPU6050_SLAVE_ADDRESS, &reg, 1);
    if (rc == IOT_SUCCESS)
        rc = IoTI2cRead(MPU_PORT, MPU6050_SLAVE_ADDRESS, data, n);
    safety_i2c_result_locked(rc == IOT_SUCCESS);
    safety_i2c_unlock();
    return rc == IOT_SUCCESS ? 0 : -1;
}
static int write_reg(uint8_t reg, uint8_t value)
{
    uint8_t data[2] = {reg, value};
    if (!safety_i2c_try_lock(200)) return -1;
    if (safety_i2c_prepare_locked() != 0) { safety_i2c_unlock(); return -1; }
    unsigned int rc = IoTI2cWrite(MPU_PORT, MPU6050_SLAVE_ADDRESS, data, 2);
    safety_i2c_result_locked(rc == IOT_SUCCESS);
    safety_i2c_unlock();
    return rc == IOT_SUCCESS ? 0 : -1;
}
int mpu6050_init(void)
{
    if (!safety_i2c_try_lock(200)) return -1;
    int rc = safety_i2c_prepare_locked();
    safety_i2c_unlock();
    if (rc != 0)
        return -1;
    LOS_Msleep(1000);
    uint8_t id = 0, range = 0;
    if (read_regs(MPU6050_RA_WHO_AM_I, &id, 1)) {
        printf("[imu] WHO_AM_I read failed address=0x%02x\r\n", MPU6050_SLAVE_ADDRESS);
        return -1;
    }
    if (id != 0x68) {
        printf("[imu] unexpected WHO_AM_I=0x%02x expected=0x68\r\n", id);
        return -1;
    }
    if (write_reg(MPU6050_RA_PWR_MGMT_1, 0x80))
        return -1;
    LOS_Msleep(200);
    const uint8_t settings[][2] = {{MPU6050_RA_PWR_MGMT_1, 0},     {MPU6050_RA_INT_ENABLE, 0},
                                   {MPU6050_RA_USER_CTRL, 0},      {MPU6050_RA_FIFO_EN, 0},
                                   {MPU6050_RA_MOT_THR, 3},        {MPU6050_RA_MOT_DUR, 0x14},
                                   {MPU6050_RA_CONFIG, 4},         {MPU6050_RA_ACCEL_CONFIG, 0x18},
                                   {MPU6050_RA_INT_PIN_CFG, 0x1c}, {MPU6050_RA_INT_ENABLE, 0x40}};
    for (unsigned int i = 0; i < sizeof(settings) / sizeof(settings[0]); i++)
        if (write_reg(settings[i][0], settings[i][1]))
            return -1;
    if (read_regs(MPU6050_RA_WHO_AM_I, &id, 1) || id != 0x68 ||
        read_regs(MPU6050_RA_ACCEL_CONFIG, &range, 1) || (range & 0x18) != 0x18)
        return -1;
    printf("[imu] ready WHO_AM_I=0x%02x accel_range=16g\r\n", id);
    return 0;
}
int mpu6050_read_data(short *out)
{
    uint8_t b[6] = {0};
    if (read_regs(MPU6050_ACC_OUT, b, sizeof(b)))
        return -1;
    for (int i = 0; i < 3; i++)
        out[i] = (short)(((uint16_t)b[2 * i] << 8) | b[2 * i + 1]);
    return 0;
}
