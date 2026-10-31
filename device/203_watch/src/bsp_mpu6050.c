#include "bsp_mpu6050.h"

#define CHIP_SW63

#if defined(CHIP_SW63)
char bsp_mpu6050_reset
	(
	void
	)
{
	osal_mdelay(1000);
	return 0;
}

char bsp_mpu6050_write_data
	(
	unsigned char	addr,
	unsigned char 	data
	)
{
	unsigned char cmd[] = {addr, data};
	char ret = bsp_iic_master_write_data(MPU6050_IIC_BUS, MPU6050_SLAVE_ADDRESS, cmd, sizeof(cmd));
	return ret;
}

char bsp_mpu6050_read_data
	(
	unsigned char	addr,
	unsigned char 	*data,
	unsigned char 	data_len
	)
{
	unsigned char cmd[] = {addr};
	char ret = bsp_iic_master_write_data(MPU6050_IIC_BUS, MPU6050_SLAVE_ADDRESS, cmd, sizeof(cmd));

	ret = bsp_iic_master_read_data(MPU6050_IIC_BUS, MPU6050_SLAVE_ADDRESS, data, data_len);
	return ret;
}

char bsp_mpu6050_init
	(
	void
	)
{
	bsp_mpu6050_reset();
	
	bsp_mpu6050_write_data(MPU6050_RA_PWR_MGMT_1, 0x00);      /* 解除休眠状态 */
	bsp_mpu6050_write_data(MPU6050_RA_SMPLRT_DIV , 0x07);     /* 陀螺仪采样率 */
	bsp_mpu6050_write_data(MPU6050_RA_CONFIG , 0x06);
	bsp_mpu6050_write_data(MPU6050_RA_ACCEL_CONFIG , 0x01);   /* 配置加速度传感器工作在2G模式 */
	bsp_mpu6050_write_data(MPU6050_RA_GYRO_CONFIG, 0x18);     /* 陀螺仪自检及测量范围，典型值：0x18(不自检，2000deg/s) */
	
	return 0;
}

char bsp_mpu6050_read_id
	(
	void
	)
{
	unsigned char temp;

	bsp_mpu6050_read_data(MPU6050_RA_WHO_AM_I, &temp, 1); 
	if (temp != 0x68)
	{
		printf("bsp_mpu6050_read_id: error\r\n");
		return -1;
	} 
	printf("MPU6050 ID = %d\r\n", temp);
	return 0;
}

char bsp_mpu6050_read_gyro
	(
	short *gyro_data
	)
{
    unsigned char buf[6];

    char ret = bsp_mpu6050_read_data(MPU6050_GYRO_OUT, buf, 6);
    gyro_data[0] = (buf[0] << 8) | buf[1];
    gyro_data[1] = (buf[2] << 8) | buf[3];
    gyro_data[2] = (buf[4] << 8) | buf[5];

	return ret;
}

char bsp_mpu6050_read_acc
	(
	short *acc_data
	)
{
    unsigned char buf[6];

    char ret = bsp_mpu6050_read_data(MPU6050_ACC_OUT, buf, 6);
    acc_data[0] = (buf[0] << 8) | buf[1];
    acc_data[1] = (buf[2] << 8) | buf[3];
    acc_data[2] = (buf[4] << 8) | buf[5];

	return ret;
}

char bsp_mpu6050_read_temp
	(
	short *temp
	)
{
	short temp3;
	unsigned char buf[2];
	
	char ret = bsp_mpu6050_read_data(MPU6050_RA_TEMP_OUT_H, buf, 2); 
    temp3 = (buf[0] << 8) | buf[1];	
	//*temp = ((double) temp3 / 340.0) + 36.53;
	*temp = temp3;
	printf("buf:%d,%d", buf[0], buf[1]);

	return ret;
}


#endif


