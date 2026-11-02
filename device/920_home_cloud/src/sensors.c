/**
 * 传感器采集实现（光敏/烟雾/人体感应/门磁）+ 开门提示灯
 *
 * 关键约束（踩过的坑）：
 *  - 必须用 osal_msleep 让出 CPU。osal_mdelay / uapi_tcxo_delay_ms 是忙等，
 *    会把 CPU 钉在 100%，饿死空闲任务后触发 CpupGuard 复位。
 *  - ADC 全局只初始化一次，两路通道共用，全程不 deinit。
 *  - 本文件的代码只能跑在任务里，不能放到 app_run 的 entry（Init 阶段调度器未就绪，
 *    任何阻塞调用都会 mcause 0x5 Load access fault）。
 */

#include "cfg.h"
#include "sensors.h"

#include "pinctrl.h"
#include "gpio.h"
#include "adc.h"
#include "adc_porting.h"
#include "common_def.h"
#include "soc_osal.h"

#define SENSOR_LOG              "[SENSOR] "

static volatile sensors_snapshot_t g_snap = {0};
static volatile td_u8  g_event = 0;
static volatile td_u8  g_lamp_on = 0;

/* 开门提示灯：门开→亮，门关→延时 CFG_LAMP_HOLD_MS 后灭 */
static volatile td_u32 g_lamp_off_at_tick = 0;
static volatile td_u32 g_tick_ms = 0;

static td_void lamp_set(td_u8 on)
{
    if (g_lamp_on == on) {
        return;
    }
    uapi_gpio_set_val(CFG_LAMP_PIN, on ? GPIO_LEVEL_HIGH : GPIO_LEVEL_LOW);
    g_lamp_on = on;
    osal_printk(SENSOR_LOG "lamp %s\r\n", on ? "ON" : "OFF");
}

static td_void *sensor_task(const td_char *arg)
{
    td_u16 light_mv = 0;
    td_u16 smoke_mv = 0;
    td_s32 door_cand = -1;
    td_s32 door_stable = -1;
    td_u32 door_cnt = 0;
    td_u32 ticks = 0;
    td_u8  pir_last = 0xFF;

    unused(arg);

    /* 引脚初始化放在任务里，避免在 Init 阶段做任何可能阻塞的动作 */
    uapi_pin_set_mode(CFG_PIR_PIN, HAL_PIO_FUNC_GPIO);
    uapi_gpio_set_dir(CFG_PIR_PIN, GPIO_DIRECTION_INPUT);

    uapi_pin_set_mode(CFG_DOOR_PIN, HAL_PIO_FUNC_GPIO);
    uapi_gpio_set_dir(CFG_DOOR_PIN, GPIO_DIRECTION_INPUT);
    uapi_pin_set_pull(CFG_DOOR_PIN, PIN_PULL_TYPE_UP);

    osal_printk(SENSOR_LOG "task started: light=adc%d smoke=adc%d pir=gpio%d door=gpio%d\r\n",
                CFG_LIGHT_ADC_CH, CFG_SMOKE_ADC_CH, CFG_PIR_PIN, CFG_DOOR_PIN);

    while (1) {
        /* --- 门磁：轮询 + 去抖，只在稳定后变化时上报 --- */
        td_s32 now = (uapi_gpio_get_val(CFG_DOOR_PIN) != GPIO_LEVEL_LOW) ? 1 : 0;
        if (now == door_cand) {
            if (door_cnt < CFG_DOOR_DEBOUNCE) {
                door_cnt++;
            }
            if (door_cnt >= CFG_DOOR_DEBOUNCE && now != door_stable) {
                door_stable = now;
#if CFG_DOOR_MAGNET_NEAR_IS_HIGH
                td_u8 open = (door_stable == 0) ? 1 : 0;
#else
                td_u8 open = (door_stable == 1) ? 1 : 0;
#endif
                g_snap.door_open = open;
                g_snap.door_valid = 1;
                g_event = 1;
                osal_printk(SENSOR_LOG "door %s\r\n", open ? "OPEN" : "CLOSED");

                /* 开门提示灯 */
                if (open) {
                    lamp_set(1);
                    g_lamp_off_at_tick = 0;
                } else {
                    g_lamp_off_at_tick = g_tick_ms + CFG_LAMP_HOLD_MS;
                }
            }
        } else {
            door_cand = now;
            door_cnt = 0;
        }

        /* --- 人体感应 --- */
        td_u8 pir = (uapi_gpio_get_val(CFG_PIR_PIN) != GPIO_LEVEL_LOW) ? 1 : 0;
        if (pir != pir_last) {
            if (pir_last != 0xFF) {          /* 首次读数不算事件 */
                g_event = 1;
                osal_printk(SENSOR_LOG "pir %s\r\n", pir ? "HAVE PEOPLE" : "no people");
            }
            pir_last = pir;
            g_snap.pir = pir;
        }

        /* --- ADC：每秒读一次就够，没必要跟 200ms 同频 --- */
        if (ticks % CFG_ADC_EVERY_TICKS == 0) {
            (void)adc_port_read(CFG_SMOKE_ADC_CH, &smoke_mv);
            (void)adc_port_read(CFG_LIGHT_ADC_CH, &light_mv);
            g_snap.smoke_mv = smoke_mv;
            g_snap.light_mv = light_mv;
        }

        /* --- 关门延时灭灯 --- */
        if (g_lamp_off_at_tick != 0 && g_tick_ms >= g_lamp_off_at_tick) {
            lamp_set(0);
            g_lamp_off_at_tick = 0;
        }

        g_tick_ms += CFG_SENSOR_TICK_MS;
        ticks++;
        osal_msleep(CFG_SENSOR_TICK_MS);
    }

    return NULL;
}

td_void sensors_start(td_void)
{
    osal_task *handle = NULL;

    /* 主板灯带接口（IO9）本板实测点不亮，拉低确保不亮 */
    uapi_pin_set_mode(CFG_TAIL_LED_PIN, HAL_PIO_FUNC_GPIO);
    uapi_gpio_set_dir(CFG_TAIL_LED_PIN, GPIO_DIRECTION_OUTPUT);
    uapi_gpio_set_val(CFG_TAIL_LED_PIN, GPIO_LEVEL_LOW);

    /* 扩展板灯组（IO10）：初始化成输出低，熄灭 */
    uapi_pin_set_mode(CFG_LAMP_PIN, HAL_PIO_FUNC_GPIO);
    uapi_gpio_set_dir(CFG_LAMP_PIN, GPIO_DIRECTION_OUTPUT);
    uapi_gpio_set_val(CFG_LAMP_PIN, GPIO_LEVEL_LOW);
    g_lamp_on = 0;

    /* ADC 全局只初始化一次，两路通道共用 */
    uapi_adc_init(ADC_CLOCK_NONE);

    osal_kthread_lock();
    handle = osal_kthread_create((osal_kthread_handler)sensor_task, 0,
                                 "SensorTask", CFG_SENSOR_STACK_SIZE);
    if (handle != NULL) {
        osal_kthread_set_priority(handle, CFG_SENSOR_TASK_PRIO);
        osal_kfree(handle);
    } else {
        osal_printk(SENSOR_LOG "create task failed\r\n");
    }
    osal_kthread_unlock();
}

td_void sensors_get(sensors_snapshot_t *out)
{
    if (out == TD_NULL) {
        return;
    }
    out->light_mv = g_snap.light_mv;
    out->smoke_mv = g_snap.smoke_mv;
    out->pir = g_snap.pir;
    out->door_open = g_snap.door_open;
    out->door_valid = g_snap.door_valid;
}

td_u8 sensors_take_event(td_void)
{
    td_u8 v = g_event;
    if (v != 0) {
        g_event = 0;
    }
    return v;
}
