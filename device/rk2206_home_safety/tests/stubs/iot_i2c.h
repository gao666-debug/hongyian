#include <stdint.h>
#define EI2C0_M2 0
#define EI2C_FRE_100K 0
#define EI2C_FRE_400K 1
#define EI2C_FRE_1000K 2
unsigned int IoTI2cInit(unsigned int,unsigned int);
unsigned int IoTI2cWrite(unsigned int,unsigned int,const uint8_t*,unsigned int);
unsigned int IoTI2cRead(unsigned int,unsigned int,uint8_t*,unsigned int);

unsigned int IoTI2cDeinit(unsigned int);
