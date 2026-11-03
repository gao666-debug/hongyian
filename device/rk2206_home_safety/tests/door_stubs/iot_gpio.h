#define GPIO0_PB6 14
#define IOT_GPIO_DIR_IN 0
typedef enum { IOT_GPIO_VALUE0, IOT_GPIO_VALUE1 } IotGpioValue;
unsigned int IoTGpioInit(unsigned int id);
unsigned int IoTGpioSetDir(unsigned int id, unsigned int dir);
unsigned int IoTGpioGetInputVal(unsigned int id, IotGpioValue *v);
