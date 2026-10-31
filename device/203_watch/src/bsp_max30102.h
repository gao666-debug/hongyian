#ifndef __BSP_MAX30102_H__
#define __BSP_MAX30102_H__

#include "bsp_iic.h"

#define	CHIP_SW63

/* 	对于MAX30100：默认I2C地址为0x57（七位地址），如果启用了ADDR引脚，则地址变为0x5A（七位地址）。
	对于MAX30102：I2C地址通常是固定的0xAE（写操作）和0xAF（读操作）。这是因为MAX30102使用的是8位地址格式，其中包含了R/W位。 */
//#define MAX30102_SLAVE_ADDRESS		0x57
#define MAX30102_SLAVE_WRITE_ADDR   0x57//0xAE
#define MAX30102_SLAVE_READ_ADDR    0x57//0xAF

#define REG_INTR_STATUS_1 			0x00
#define REG_INTR_STATUS_2 			0x01
#define REG_INTR_ENABLE_1 			0x02
#define REG_INTR_ENABLE_2 			0x03
#define REG_FIFO_WR_PTR 			0x04
#define REG_OVF_COUNTER 			0x05
#define REG_FIFO_RD_PTR 			0x06
#define REG_FIFO_DATA 				0x07
#define REG_FIFO_CONFIG 			0x08
#define REG_MODE_CONFIG 			0x09
#define REG_SPO2_CONFIG 			0x0A
#define REG_LED1_PA 				0x0C
#define REG_LED2_PA 				0x0D
#define REG_PILOT_PA 				0x10

#if defined(CHIP_SW63)
#include "pinctrl.h"
#include "soc_osal.h"

#define MAX30102_IIC_BUS			1

char bsp_max30102_read_data
	(
	unsigned char	addr,
	unsigned char 	*data,
	unsigned char 	data_len
	);

char bsp_max30102_init
	(
	void
	);

unsigned char bsp_max30102_temp_read
	(
	void
	);

void bsp_max30102_fifo_read
	(
	float *output_data
	);

unsigned short bsp_max30102_get_heart_rate
	(
	float 			*input_data,
	unsigned short	cache_nums
	);

float bsp_max30102_get_spo2
	(
	float 			*ir_input_data,
	float 			*red_input_data,
	unsigned short	cache_nums
	);
	


#endif

#endif
