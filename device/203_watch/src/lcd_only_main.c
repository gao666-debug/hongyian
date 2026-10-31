#include "lcd_bringup.h"
#include "soc_osal.h"
#include "app_init.h"
#include "watchdog.h"
#include "tcxo.h"
#include <stdio.h>

static void *lcd_only_task(const char *arg)
{
    static const uint16_t colors[] = {0xF800, 0x07E0, 0x001F, 0xFFFF, 0x0000};
    static const char *names[] = {"RED", "GREEN", "BLUE", "WHITE", "BLACK", "BARS"};
    unsigned int frame = 0;
    uint32_t ret;
    (void)arg;
    printf("\r\n[hy-lcd] BUILD=HY-LCD-ONLY-1 %s %s\r\n", __DATE__, __TIME__);
    printf("[hy-lcd] GC9A01 240x240 CLK=7 MOSI=9 DC=10 CS=11 RST=12\r\n");
    printf("[hy-lcd] I2C=OFF scans=OFF; use WEAR 7-pin LCD1, NOT EXB 4-pin OLED\r\n");
    ret = hy_lcd_init();
    while (ret == 0) {
        unsigned int i;
        uint64_t start = uapi_tcxo_get_ms();
        printf("[hy-lcd] frame=%u begin=%s\r\n", frame, names[frame % 6]);
        ret = frame % 6 == 5 ? hy_lcd_bars() : hy_lcd_fill(colors[frame % 6]);
        printf("[hy-lcd] frame=%u sent=%s elapsed_ms=%lu rc=0x%08lx; VISUAL CHECK REQUIRED\r\n",
               frame, names[frame % 6], (unsigned long)(uapi_tcxo_get_ms() - start), (unsigned long)ret);
        ++frame;
        for (i = 0; i < 30; ++i) {
            uapi_watchdog_kick();
            osal_msleep(100);
        }
    }
    printf("[hy-lcd] stopped after API error; no pin search/retry\r\n");
    for (;;) { uapi_watchdog_kick(); osal_msleep(100); }
    return NULL;
}

static void lcd_only_entry(void)
{
    osal_task *task;
    osal_kthread_lock();
    task = osal_kthread_create((osal_kthread_handler)lcd_only_task, 0, "hy_lcd_only", 0x4000);
    if (task != NULL) osal_kthread_set_priority(task, 24);
    osal_kthread_unlock();
    if (task == NULL) printf("[hy-lcd] ERROR task creation failed\r\n");
}
app_run(lcd_only_entry);
