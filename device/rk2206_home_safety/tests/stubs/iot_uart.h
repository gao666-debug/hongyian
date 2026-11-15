/* 主机侧 UART 桩：只为在电脑上编译/运行 su_03t.c，真机用的是 base/iot_hardware 里的实现。 */
#ifndef IOT_UART_H
#define IOT_UART_H

#include <stdint.h>

typedef enum {
    EUART0_M1 = 0,
    EUART1_M1 = 1,
    EUART2_M1 = 2
} uart_bus_e;

typedef struct {
    unsigned int baudRate;
    unsigned int dataBits;
    unsigned int pad;
    unsigned int parity;
    unsigned int rxBlock;
    unsigned int stopBits;
    unsigned int txBlock;
} IotUartAttribute;

#define IOT_UART_DATA_BIT_8            0
#define IOT_UART_PARITY_NONE           0
#define IOT_UART_STOP_BIT_1            0
#define IOT_UART_BLOCK_STATE_NONE_BLOCK 0
#define IOT_FLOW_CTRL_NONE             0

unsigned int IoTUartInit(unsigned int id, IotUartAttribute *attr);
unsigned int IoTUartDeinit(unsigned int id);
int IoTUartWrite(unsigned int id, const uint8_t *data, unsigned int len);
int IoTUartRead(unsigned int id, uint8_t *data, unsigned int len);

#endif
