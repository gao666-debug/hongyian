#ifndef __BSP_IIC_H__
#define __BSP_IIC_H__

#define CHIP_SW63

#if defined(CHIP_SW63)
#include "pinctrl.h"
#include "i2c.h"

char bsp_iic_master_init
	(
	unsigned char 	io_scl,
	unsigned char 	io_sda,
	unsigned int	baudrate,
	unsigned char	hscode
	);

char bsp_iic_master_write_data
	(
	unsigned char 	bus,
	unsigned char 	dev_addr,
	unsigned char	*buffer,
	unsigned int	buffer_len
	);

char bsp_iic_master_read_data
	(
	unsigned char 	bus,
	unsigned char 	dev_addr,
	unsigned char	*buffer,
	unsigned int	buffer_len
	);
#endif

#endif
