#ifndef TEST_STUB_CMSIS_OS2_H
#define TEST_STUB_CMSIS_OS2_H
#include <stdint.h>
typedef void *osMutexId_t;
typedef void *osThreadId_t;
typedef void (*osThreadFunc_t)(void*);
typedef struct {const char *name;uint32_t attr_bits;void *cb_mem;uint32_t cb_size;void *stack_mem;uint32_t stack_size;int priority;} osThreadAttr_t;
#define osWaitForever 0xffffffffU
osMutexId_t osMutexNew(void*);
int osMutexAcquire(osMutexId_t,unsigned int);
int osMutexRelease(osMutexId_t);
osThreadId_t osThreadNew(osThreadFunc_t,void*,const osThreadAttr_t*);
int osThreadGetPriority(osThreadId_t);
osThreadId_t osThreadGetId(void);
unsigned int osKernelGetTickFreq(void);

#endif
