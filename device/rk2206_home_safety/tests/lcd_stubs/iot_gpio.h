#ifndef LCD_TEST_GPIO_H
#define LCD_TEST_GPIO_H
#define GPIO0_PA4 4
#define GPIO0_PC0 16
#define GPIO0_PC1 17
#define GPIO0_PC2 18
#define GPIO0_PC3 19
#define GPIO0_PC6 22
#define IOT_GPIO_DIR_OUT 1
#define IOT_GPIO_VALUE0 0
#define IOT_GPIO_VALUE1 1
unsigned IoTGpioInit(unsigned);
unsigned IoTGpioDeinit(unsigned);
unsigned IoTGpioSetDir(unsigned,int);
unsigned IoTGpioSetOutputVal(unsigned,int);
#endif
