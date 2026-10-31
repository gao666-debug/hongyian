#ifndef __BSP_MAX30102_BLOOD_H__
#define __BSP_MAX30102_BLOOD_H__

void max30102_fir_init(void);
void ir_max30102_fir(float *input,float *output);
void red_max30102_fir(float *input,float *output);

#endif
