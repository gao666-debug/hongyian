/**
 * 920_home_cloud：独居老人居家安全监测 —— WS63 传感器节点上云
 *
 * 链路：四路传感器 → Wi-Fi → SNTP 校时 → MQTT → 华为云 IoTDA → AMQP → 自建后端
 *
 * 任务划分：
 *   SensorTask   每 200ms 采集门磁/PIR，每秒读一次 ADC，负责开门提示灯
 *   app_task     连网 → 连 MQTT → 定时上报（10s）+ 事件立即上报
 *
 * 注意：app_run 注册的 entry 跑在 osMain 的 Init 阶段，调度器还没起来，
 * 里面不能有任何阻塞调用（osal_msleep 之类会 mcause 0x5 反复重启）。
 * 所以所有实际工作都放到 osThreadNew 创建的任务里。
 */

#include "cfg.h"
#include "sensors.h"
#include "net.h"
#include "iotda.h"

#include "cmsis_os2.h"
#include "common_def.h"
#include "soc_osal.h"
#include "app_init.h"

#define APP_LOG                 "[APP] "

#define REPORT_TICKS            (CFG_REPORT_PERIOD_MS / 10)     /* tick = 10ms */
#define LOOP_TICKS              50                              /* 500ms 轮询一次 */
#define EVENT_GAP_TICKS         (CFG_EVENT_REPORT_MIN_MS / 10)  /* 事件上报最小间隔 */
#define MAX_FAIL_BEFORE_RECONN  3

static td_s32 app_task(td_void *param)
{
    sensors_snapshot_t snap;
    td_u32 last_report = 0;
    td_u32 last_event_report = 0;
    td_u32 fails = 0;

    unused(param);

    /* 先起采集任务，保证上报时一定有数据 */
    sensors_start();

    osal_printk(APP_LOG "connecting wifi...\r\n");
    (void)net_connect();

    while (iotda_connect() != 0) {
        osal_printk(APP_LOG "mqtt retry in 10s\r\n");
        osal_msleep(1000);
    }

    last_report = osKernelGetTickCount();
    last_event_report = last_report;

    for (;;) {
        td_u32 now = osKernelGetTickCount();
        td_u8  event = sensors_take_event();
        td_bool due = ((now - last_report) >= REPORT_TICKS);
        td_bool event_ready = (event != 0) &&
                              ((now - last_event_report) >= EVENT_GAP_TICKS);

        if (due || event_ready) {
            sensors_get(&snap);
            if (iotda_report(&snap) == 0) {
                fails = 0;
            } else {
                fails++;
            }
            last_report = now;
            if (event_ready) {
                last_event_report = now;
            }

            /* 连续失败就让上层重连一次，避免连接悄悄断掉后一直空转 */
            if (fails >= MAX_FAIL_BEFORE_RECONN) {
                osal_printk(APP_LOG "report failed %u times, reconnecting\r\n", (unsigned)fails);
                fails = 0;
                if (iotda_connect() != 0) {
                    osal_msleep(1000);
                }
            }
        }

        osal_msleep(LOOP_TICKS);
    }

    return 0;
}

static td_void home_cloud_entry(td_void)
{
    osThreadAttr_t attr;

    attr.name = "home_cloud";
    attr.attr_bits = 0U;
    attr.cb_mem = NULL;
    attr.cb_size = 0U;
    attr.stack_mem = NULL;
    attr.stack_size = CFG_APP_STACK_SIZE;
    attr.priority = CFG_APP_TASK_PRIO;

    osal_printk("\r\n\r\n===== %s =====\r\n", CFG_FW_VERSION);
    osal_printk("device=%s service=%s\r\n", CFG_IOTDA_DEVICE_ID, CFG_IOTDA_SERVICE_ID);

    if (osThreadNew((osThreadFunc_t)app_task, NULL, &attr) == NULL) {
        osal_printk(APP_LOG "create task failed\r\n");
    }
}

app_run(home_cloud_entry);
