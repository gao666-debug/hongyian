/*
 * hy_sensor_main.c —— HY-SENSOR-1 入口
 *
 * 和上一版 HY-LCD-ONLY-1 的关系：
 *   - LCD 通路原样保留（同样的引脚、同样的初始化序列），但逻辑独立成任务，
 *     传感器挂在更高优先级的任务上，两者互不抢资源（LCD 走软 SPI，传感器走 I2C1）。
 *   - 屏的结论没变：软件侧已经发完一整帧，仍然是均匀灰，剩下的是硬件问题。
 *     所以这一版的目的是把两个传感器单独跑通，让手表这条线能继续往下做。
 *
 * 想只看传感器日志、不刷 LCD：把 HY_LCD_ENABLE 改成 0。
 */
#include "lcd_bringup.h"
#include "hy_sensor.h"
#include "hy_cloud_cfg.h"

#if HY_CLOUD_ENABLE
#include "hy_cloud.h"
#endif

#include "soc_osal.h"
#include "app_init.h"
#include "watchdog.h"
#include "tcxo.h"

#include <stdio.h>

#define HY_LCD_ENABLE            1
#define HY_LCD_FRAME_GAP_MS      5000

#define HY_SENSOR_TASK_STACK     0x4000
#define HY_SENSOR_TASK_PRIO      20
#define HY_LCD_TASK_STACK        0x2000
#define HY_LCD_TASK_PRIO         24
#define HY_CLOUD_TASK_STACK      0x3000
#define HY_CLOUD_TASK_PRIO       16

#if HY_LCD_ENABLE
static void *hy_lcd_task(const char *arg)
{
    static const uint16_t colors[] = {0xF800, 0x07E0, 0x001F, 0xFFFF, 0x0000};
    static const char *names[] = {"RED", "GREEN", "BLUE", "WHITE", "BLACK", "BARS"};
    unsigned int frame = 0;
    uint32_t ret;

    (void)arg;
    printf("[hy-lcd] GC9A01 240x240 CLK=IO7 MOSI=IO9 DC=IO10 CS=IO11 RST=IO12\r\n");

    ret = hy_lcd_init();
    while (ret == 0) {
        uint64_t start = uapi_tcxo_get_ms();

        printf("[hy-lcd] frame=%u begin=%s\r\n", frame, names[frame % 6]);
        ret = (frame % 6 == 5) ? hy_lcd_bars() : hy_lcd_fill(colors[frame % 6]);
        printf("[hy-lcd] frame=%u sent=%s elapsed_ms=%lu rc=0x%08lx; 显示仍需目视确认\r\n",
               frame, names[frame % 6], (unsigned long)(uapi_tcxo_get_ms() - start), (unsigned long)ret);
        ++frame;

        {
            unsigned int i;
            for (i = 0; i < HY_LCD_FRAME_GAP_MS / 100; ++i) {
                uapi_watchdog_kick();
                osal_msleep(100);
            }
        }
    }
    printf("[hy-lcd] 已停止（API 报错），不再重试\r\n");
    for (;;) {
        uapi_watchdog_kick();
        osal_msleep(100);
    }
    return NULL;
}
#endif

static void hy_sensor_entry(void)
{
    osal_task *task = NULL;

    printf("\r\n\r\n==============================================\r\n");
    printf("  BUILD=HY-CLOUD-19 (服务ID改为 11) %s %s\r\n", __DATE__, __TIME__);
    printf("  MPU6050(0x68/0x69) + MAX3010x(0x57)  I2C 自诊断点亮\r\n");
    printf("  LCD=%s  上云=%s\r\n", HY_LCD_ENABLE ? "ON" : "OFF",
           HY_CLOUD_ENABLE ? "ON" : "OFF");
#if HY_CLOUD_ENABLE
    printf("  云端: %s:%d  服务=%s\r\n", HY_HW_DOMAIN, HY_HW_PORT, HY_HW_SERVICE_ID);
#endif
    printf("==============================================\r\n");

    osal_kthread_lock();

#if HY_LCD_ENABLE
    task = osal_kthread_create((osal_kthread_handler)hy_lcd_task, 0, "hy_lcd", HY_LCD_TASK_STACK);
    if (task != NULL) {
        osal_kthread_set_priority(task, HY_LCD_TASK_PRIO);
    } else {
        printf("[hy-sensor] LCD 任务创建失败\r\n");
    }
#endif

    task = osal_kthread_create((osal_kthread_handler)hy_sensor_task, 0, "hy_sensor", HY_SENSOR_TASK_STACK);
    if (task != NULL) {
        osal_kthread_set_priority(task, HY_SENSOR_TASK_PRIO);
    } else {
        printf("[hy-sensor] 传感器任务创建失败\r\n");
    }

#if HY_CLOUD_ENABLE
    /* 优先级压得比传感器低，WiFi 再忙也不许抢采样 */
    task = osal_kthread_create((osal_kthread_handler)hy_cloud_task, 0, "hy_cloud", HY_CLOUD_TASK_STACK);
    if (task != NULL) {
        osal_kthread_set_priority(task, HY_CLOUD_TASK_PRIO);
    } else {
        printf("[hy-sensor] 上云任务创建失败\r\n");
    }
#endif

    osal_kthread_unlock();
}

app_run(hy_sensor_entry);
