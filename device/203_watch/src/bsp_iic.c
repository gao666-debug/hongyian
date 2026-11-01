/*
 * bsp_iic.c —— I2C 封装（返回码完整打印版）
 *
 * 为什么改：
 *   uapi_i2c_master_write/read 返回 errcode_t，是 uint32_t。
 *   原代码用 `char ret = uapi_i2c_master_write(...)` 承接，会把 32 位错误码
 *   截成低 8 位。比如 0x80001313 (I2C_TIMEOUT) 打出来是 19，
 *   看上去像 POSIX errno 19 (ENODEV，"设备不存在")，结论直接被带偏。
 *
 * 现在：
 *   内部一律用 errcode_t 承接，按 0x%08X 完整打印，并解码成错误名。
 *   采样循环里失败很频繁，前 16 次全打，之后每 500 次打一条，避免冲垮串口。
 */
#include "bsp_iic.h"
#include "errcode.h"

#if defined(CHIP_SW63)

/* errcode.h 里 I2C 段的错误码，原样列出来便于对照 */
static const char *i2c_err_name(errcode_t e)
{
    switch (e) {
        case ERRCODE_SUCC:                      return "OK";
        case ERRCODE_FAIL:                      return "FAIL(0xFFFFFFFF)";
        case ERRCODE_INVALID_PARAM:             return "INVALID_PARAM";
        case ERRCODE_I2C_NOT_INIT:              return "I2C_NOT_INIT";
        case ERRCODE_I2C_ALREADY_INIT:          return "I2C_ALREADY_INIT";
        case ERRCODE_I2C_INVALID_PARAMETER:     return "I2C_INVALID_PARAMETER";
        case ERRCODE_I2C_NOT_IMPLEMENT:         return "I2C_NOT_IMPLEMENT";
        case ERRCODE_I2C_EVENT_INIT_FAILURE:    return "I2C_EVENT_INIT_FAILURE";
        case ERRCODE_I2C_RATE_INVALID:          return "I2C_RATE_INVALID";
        case ERRCODE_I2C_ADDRESS_INVLID:        return "I2C_ADDRESS_INVALID";
        case ERRCODE_I2C_PERMISSION_INVALID:    return "I2C_PERMISSION_INVALID";
        case ERRCODE_I2C_NO_VALID_DATA:         return "I2C_NO_VALID_DATA";
        case ERRCODE_I2C_SEND_PARAM_INVALID:    return "I2C_SEND_PARAM_INVALID";
        case ERRCODE_I2C_RECEIVE_PARAM_INVALID: return "I2C_RECEIVE_PARAM_INVALID";
        case ERRCODE_I2C_WAIT_EXCEPTION:        return "I2C_WAIT_EXCEPTION";
        case ERRCODE_I2C_WAIT_CONTINUE:         return "I2C_WAIT_CONTINUE";
        case ERRCODE_I2C_TIMEOUT:               return "I2C_TIMEOUT";
        case ERRCODE_I2C_ACK_ERR:               return "I2C_ACK_ERR(从机没应答)";
        case ERRCODE_I2C_MUTEX_PROC_FAIL:       return "I2C_MUTEX_PROC_FAIL";
        case ERRCODE_I2C_DMA_CONFIG_ERROR:      return "I2C_DMA_CONFIG_ERROR";
        case ERRCODE_I2C_DMA_TRANSFER_ERROR:    return "I2C_DMA_TRANSFER_ERROR";
        default:                                return "UNKNOWN";
    }
}

/* 失败打印限流：前 16 次逐条打，之后每 500 次打一条，并带上累计次数 */
static void i2c_report(const char *who, unsigned char bus, unsigned char addr,
                       errcode_t e, unsigned int *cnt)
{
    unsigned int n = (*cnt)++;

    if (n < 16u || (n % 500u) == 0u) {
        printf("%s: bus=%u addr=0x%02X ret=0x%08X (%s) 第%u次失败 [errcode_t=%u字节]\r\n",
               who, (unsigned)bus, (unsigned)addr, (unsigned)e, i2c_err_name(e), n + 1u,
               (unsigned)sizeof(errcode_t));
    }
}

char bsp_iic_master_init(unsigned char io_scl, unsigned char io_sda, unsigned int baudrate, unsigned char hscode)
{
    errcode_t ret = 0;

    if (!((io_scl == 16 && io_sda == 15) || (io_scl == 15 && io_sda == 16) || (io_scl == 18 && io_sda == 17))) {
        printf("bsp_iic_master_init: io_scl or io_sda error, scl:%d, sda:%d !\r\n", io_scl, io_sda);
        return -1;
    }

    ret = uapi_pin_set_mode(io_scl, 2);
    if (ret != 0) {
        printf("bsp_iic_master_init: pin_mode(SCL=%d) ret=0x%08X (%s)\r\n",
               (int)io_scl, (unsigned)ret, i2c_err_name(ret));
        return -1;
    }
    ret = uapi_pin_set_mode(io_sda, 2);
    if (ret != 0) {
        printf("bsp_iic_master_init: pin_mode(SDA=%d) ret=0x%08X (%s)\r\n",
               (int)io_sda, (unsigned)ret, i2c_err_name(ret));
        return -1;
    }

    if ((io_scl == 16 && io_sda == 15) || (io_scl == 15 && io_sda == 16)) {
        ret = uapi_i2c_master_init(1, baudrate, hscode);
        printf("bsp_iic_master_init: bus=1 scl=%d sda=%d baud=%u ret=0x%08X (%s)\r\n",
               (int)io_scl, (int)io_sda, baudrate, (unsigned)ret, i2c_err_name(ret));
    } else {
        ret = uapi_i2c_master_init(0, baudrate, hscode);
        printf("bsp_iic_master_init: bus=0 scl=%d sda=%d baud=%u ret=0x%08X (%s)\r\n",
               (int)io_scl, (int)io_sda, baudrate, (unsigned)ret, i2c_err_name(ret));
    }
    if (ret != 0) {
        return -1;
    }

    return 0;
}

char bsp_iic_master_write_data(unsigned char bus, unsigned char dev_addr, unsigned char *buffer, unsigned int buffer_len)
{
    errcode_t ret;
    static unsigned int s_wfail = 0;

    if (bus != 0 && bus != 1) {
        printf("bsp_iic_master_write_data: bus error !\r\n");
        return -1;
    }

    i2c_data_t data = {0};
    data.send_buf = buffer;
    data.send_len = buffer_len;
    ret = uapi_i2c_master_write(bus, dev_addr, &data);
    if (ret != 0) {
        i2c_report("bsp_iic_write", bus, dev_addr, ret, &s_wfail);
        return -1;
    }

    return 0;
}

char bsp_iic_master_read_data(unsigned char bus, unsigned char dev_addr, unsigned char *buffer, unsigned int buffer_len)
{
    errcode_t ret;
    static unsigned int s_rfail = 0;

    if (bus != 0 && bus != 1) {
        printf("bsp_iic_master_read_data: bus error !\r\n");
        return -1;
    }

    i2c_data_t data = {0};
    data.receive_buf = buffer;
    data.receive_len = buffer_len;
    ret = uapi_i2c_master_read(bus, dev_addr, &data);
    if (ret != 0) {
        i2c_report("bsp_iic_read ", bus, dev_addr, ret, &s_rfail);
        return -1;
    }

    return 0;
}
#endif
