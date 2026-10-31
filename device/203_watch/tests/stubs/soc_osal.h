#include <stddef.h>
typedef struct { int unused; } osal_task;
typedef void *(*osal_kthread_handler)(const char *);
void osal_msleep(unsigned int ms);
void osal_kthread_lock(void);
void osal_kthread_unlock(void);
osal_task *osal_kthread_create(osal_kthread_handler f, void *arg, const char *name, unsigned int stack);
void osal_kthread_set_priority(osal_task *task, unsigned int priority);
