#ifndef __BSP_MPU6050_H__
#define __BSP_MPU6050_H__

#include "bsp_iic.h"

#define	CHIP_SW63

/* 模块的A0引脚接GND，IIC的7位地址为0x68，若接到VCC，需要改为0x69 */
#define MPU6050_SLAVE_ADDRESS		0x68

#define MPU6050_WHO_AM_I        	0x75
#define MPU6050_SMPLRT_DIV      	0  		 /* 8000Hz */
#define MPU6050_DLPF_CFG       		0
#define MPU6050_GYRO_OUT        	0x43     /* MPU6050陀螺仪数据寄存器地址 */
#define MPU6050_ACC_OUT         	0x3B     /* MPU6050加速度数据寄存器地址 */
#define MPU6050_RA_TEMP_OUT_H		0x41	 /* MPU6050温度测量寄存器地址 */

#define MPU6050_RA_SMPLRT_DIV       0x19
#define MPU6050_RA_CONFIG           0x1A
#define MPU6050_RA_GYRO_CONFIG      0x1B
#define MPU6050_RA_ACCEL_CONFIG     0x1C

#define MPU6050_RA_PWR_MGMT_1       0x6B
#define MPU6050_RA_PWR_MGMT_2       0x6C

#define	MPU6050_RA_WHO_AM_I			0x75

#if defined(CHIP_SW63)
#include "pinctrl.h"
#include "soc_osal.h"

#define MPU6050_IIC_BUS				1

char bsp_mpu6050_init
	(
	void
	);

char bsp_mpu6050_read_id
	(
	void
	);

char bsp_mpu6050_read_gyro
	(
	short *gyro_data
	);

char bsp_mpu6050_read_acc
	(
	short *acc_data
	);

char bsp_mpu6050_read_temp
	(
	short *temp
	);

#endif

#endif
