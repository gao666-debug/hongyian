#include "bsp_iic.h"

#if defined(CHIP_SW63)
char bsp_iic_master_init(unsigned char io_scl, unsigned char io_sda, unsigned int baudrate, unsigned char hscode)
{
	char ret = 0;
	
	if (!((io_scl == 16 && io_sda == 15) || (io_scl == 15 && io_sda == 16) || (io_scl == 18 && io_sda == 17)))
	{
		printf("bsp_iic_master_init: io_scl or io_sda error, scl:%d, sda:%d !\r\n", io_scl, io_sda);
		return -1;
	}
	
	ret = uapi_pin_set_mode(io_scl, 2);
    ret = uapi_pin_set_mode(io_sda, 2);
	if (ret != 0)
	{
		printf("bsp_iic_master_init: uapi_pin_set_mode error !\r\n");
		return -1;
	}
	
	if ((io_scl == 16 && io_sda == 15) || (io_scl == 15 && io_sda == 16))
	{
		ret = uapi_i2c_master_init(1, baudrate, hscode);
	}
	else
	{
		ret = uapi_i2c_master_init(0, baudrate, hscode);
	}
	if (ret != 0)
	{
		printf("bsp_iic_master_init: uapi_i2c_master_init error, ret= 0x%x !\r\n", ret);
		return -1;
	}
	
	return ret;
}

char bsp_iic_master_write_data(unsigned char bus, unsigned char dev_addr, unsigned char	*buffer, unsigned int buffer_len)
{
	char ret = 0;
	
	if (bus != 0 && bus != 1)
	{
		printf("bsp_iic_master_write_data: bus error !\r\n");
		return -1;
	}
	
	i2c_data_t data = {0};
    data.send_buf = buffer;
    data.send_len = buffer_len;
    ret = uapi_i2c_master_write(bus, dev_addr, &data);
    if (ret != 0) {
        /* 采样循环里失败会很频繁，全打印会把串口（和 CPU）冲垮，限流 */
        static unsigned int s_wfail = 0;
        if ((s_wfail++ % 500u) == 0u) {
            printf("bsp_iic_master_write_data: uapi_i2c_master_write error, ret= %d !\r\n", ret);
        }
        return -1;
    }
	
	return ret;
}

char bsp_iic_master_read_data(unsigned char bus, unsigned char dev_addr, unsigned char *buffer, unsigned int buffer_len)
{
	char ret = 0;
	
	if (bus != 0 && bus != 1)
	{
		printf("bsp_iic_master_read_data: bus error !\r\n");
		return -1;
	}
	
	i2c_data_t data = {0};
    data.receive_buf = buffer;
    data.receive_len = buffer_len;
    ret = uapi_i2c_master_read(bus, dev_addr, &data);
    if (ret != 0) {
        static unsigned int s_rfail = 0;
        if ((s_rfail++ % 500u) == 0u) {
            printf("bsp_iic_master_read_data: uapi_i2c_master_read error, ret= 0x%08x !\r\n", ret);
        }
        return -1;
    }
	
	return ret;
}
#endif


