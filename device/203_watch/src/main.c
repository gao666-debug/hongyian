/*
 * 手表扩展模块（203_watch）
 *
 * 一块表壳里同时跑三件事，这是 200 / 201 / 202 三个 demo 合起来的效果：
 *   1. 圆屏 UI      —— 表盘 / 心率血氧 / 运动三页，翻手腕切页
 *   2. MPU6050      —— 三轴加速度 + 计步
 *   3. MAX3010x     —— 心率 + 血氧 + 脉搏
 *
 * 为什么合成一个 demo 而不是开三个：
 *   201 和 202 各带一份 bsp_iic.c，同时编译必然同名符号重复定义；
 *   而且 app_run() 只能有一个入口，三份各自 app_run 会互相打架。
 *   所以这里只留一份 bsp_iic.c，一个入口，两个任务。
 *
 * 硬件引脚（HTXH_XS_WEAR_SCH V0）：
 *   LCD  软SPI  SCL=IO7  MOSI=IO9  DC=IO10  CS=IO11  RST=IO12
 *   I2C1       SCL=IO16 (UART1_TXD)  SDA=IO15 (UART1_RXD)
 *   MPU6050    addr 0x68
 *   MAX3010x   addr 0x57
 */
#include "pinctrl.h"
#include "common_def.h"
#include "soc_osal.h"
#include "gpio.h"
#include "app_init.h"

#include "watch_sensor.h"
#include "watch_ui.h"
#include "board_check.h"

#define WATCH_UI_TASK_STACK       0x2000
#define WATCH_UI_TASK_PRIO        24
#define WATCH_SENSOR_TASK_STACK   0x2000
#define WATCH_SENSOR_TASK_PRIO    20

static void watch_entry(void)
{
    osal_task *ui_task = NULL;
    osal_task *sensor_task = NULL;

    printf("\r\n\r\n");
    printf("==============================================\r\n");
    printf("  demo 203_watch : watch extension module\r\n");
    printf("  LCD 1.28\" GC9A01 240x240 + MPU6050 + MAX3010x\r\n");
    printf("==============================================\r\n");

    /* 整板自检：跑在起任务之前，单独占 CPU、单独占 I2C 引脚，
     * 免得和传感器任务的 I2C 初始化抢总线。
     * 自检若定位到屏的时钟脚，会自己把 LCD 的 clk 换成那个脚
     * （见 board_check.c 的 bc_lcd()）。 */
    board_check_run();

    osal_kthread_lock();

    sensor_task = osal_kthread_create((osal_kthread_handler)watch_sensor_task, 0,
                                      "watch_sensor", WATCH_SENSOR_TASK_STACK);
    if (sensor_task != NULL) {
        osal_kthread_set_priority(sensor_task, WATCH_SENSOR_TASK_PRIO);
    } else {
        printf("[watch] create sensor task fail\r\n");
    }

    ui_task = osal_kthread_create((osal_kthread_handler)watch_ui_task, 0,
                                  "watch_ui", WATCH_UI_TASK_STACK);
    if (ui_task != NULL) {
        osal_kthread_set_priority(ui_task, WATCH_UI_TASK_PRIO);
    } else {
        printf("[watch] create ui task fail\r\n");
    }

    osal_kthread_unlock();
}

app_run(watch_entry);
