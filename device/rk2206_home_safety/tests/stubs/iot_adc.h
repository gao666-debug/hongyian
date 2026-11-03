#ifndef TEST_IOT_ADC_H
#define TEST_IOT_ADC_H
unsigned int IoTAdcInit(unsigned int channel);
unsigned int IoTAdcGetVal(unsigned int channel, unsigned int *value);
#endif
