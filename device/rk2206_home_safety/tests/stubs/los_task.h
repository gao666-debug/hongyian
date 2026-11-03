#ifndef TEST_STUB_LOS_TASK_H
#define TEST_STUB_LOS_TASK_H
#define LOS_OK 0
#define LOS_WAIT_FOREVER 0xffffffffU
typedef void (*TSK_ENTRY_FUNC)(void*);
typedef struct {TSK_ENTRY_FUNC pfnTaskEntry;unsigned int uwStackSize;char *pcName;unsigned short usTaskPrio;} TSK_INIT_PARAM_S;
int LOS_TaskCreate(unsigned int*,TSK_INIT_PARAM_S*);
void LOS_Msleep(unsigned int);

#endif
