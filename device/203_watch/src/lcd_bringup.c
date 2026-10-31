/* Fixed-pin write-only bring-up. No GPIO search, I2C, or readback assumptions. */
#include "lcd_bringup.h"
#include "gpio.h"
#include "pinctrl.h"
#include "tcxo.h"
#include "watchdog.h"
#include "soc_osal.h"
#include <stddef.h>
#include <stdio.h>

static uint32_t s_error;
static int s_ready;

static void remember(uint32_t ret, const char *op, int pin)
{
    if (ret != 0 && s_error == 0) {
        s_error = ret;
        printf("[hy-lcd] ERROR op=%s pin=%d rc=0x%08lx\r\n", op, pin, (unsigned long)ret);
    }
}

static void level(int pin, int high)
{
    if (s_error == 0) {
        remember((uint32_t)uapi_gpio_set_val(pin, high), "write", pin);
    }
}

static void output(int pin, int idle)
{
    remember((uint32_t)uapi_pin_set_mode(pin, 0), "mux", pin);
    /* Program the output latch before enabling the output driver. */
    level(pin, idle);
    if (s_error == 0) {
        remember((uint32_t)uapi_gpio_set_dir(pin, GPIO_DIRECTION_OUTPUT), "direction", pin);
    }
}

static void delay_ms(unsigned int ms)
{
    /* Short waits allow scheduling and keep watchdog gaps bounded. */
    while (ms != 0) {
        unsigned int step = ms > 10 ? 10 : ms;
        uapi_watchdog_kick();
        osal_msleep(step);
        ms -= step;
    }
}

static void byte_out(uint8_t data)
{
    unsigned int i;
    if (s_error != 0) return;
    for (i = 0; i < 8; ++i) {
        /* Clock idle low, MSB first, data stable before rising edge. */
        level(HY_LCD_CLK, 0);
        level(HY_LCD_MOSI, (data & 0x80u) != 0);
        uapi_tcxo_delay_us(2);
        level(HY_LCD_CLK, 1);
        uapi_tcxo_delay_us(2);
        data <<= 1;
    }
    level(HY_LCD_CLK, 0);
}

static void command(uint8_t cmd, const uint8_t *data, unsigned int len)
{
    unsigned int i;
    level(HY_LCD_DC, 0);
    level(HY_LCD_CS, 0);
    uapi_tcxo_delay_us(2);
    byte_out(cmd);
    level(HY_LCD_DC, 1);
    uapi_tcxo_delay_us(2);
    for (i = 0; i < len; ++i) byte_out(data[i]);
    level(HY_LCD_CS, 1);
    uapi_tcxo_delay_us(2);
}

struct panel_command {
    uint8_t cmd;
    uint8_t count;
    uint16_t wait_ms;
    uint8_t data[12];
};

static const struct panel_command s_init[] = {
#include "lcd_vendor_init.inc"
};

uint32_t hy_lcd_init(void)
{
    size_t i;
    s_ready = 0;
    s_error = 0;
    output(HY_LCD_CS, 1);
    output(HY_LCD_RST, 1);
    output(HY_LCD_CLK, 0);
    output(HY_LCD_MOSI, 0);
    output(HY_LCD_DC, 1);
    if (s_error != 0) return s_error;

    delay_ms(150); /* power settling, then one explicit hardware reset */
    level(HY_LCD_RST, 0);
    delay_ms(100);
    level(HY_LCD_RST, 1);
    delay_ms(150);

    for (i = 0; i < sizeof(s_init) / sizeof(s_init[0]); ++i) {
        command(s_init[i].cmd, s_init[i].data, s_init[i].count);
        if (s_error != 0) return s_error;
        if (s_init[i].wait_ms) delay_ms(s_init[i].wait_ms);
    }
    s_ready = 1;
    printf("[hy-lcd] init bytes sent; this does NOT confirm visible display\r\n");
    return 0;
}

static uint32_t draw(uint16_t color, int bars)
{
    static const uint16_t palette[] = {0xF800, 0x07E0, 0x001F, 0xFFFF, 0x0000, 0xFFE0};
    const uint8_t bounds[] = {0, 0, 0, 239}; /* inclusive endpoints, never 240 */
    unsigned int x, y;
    if (!s_ready) return s_error != 0 ? s_error : 1;
    if (s_error != 0) return s_error;
    command(0x2A, bounds, sizeof(bounds));
    command(0x2B, bounds, sizeof(bounds));
    /* One CS transaction covers RAMWR plus all pixel bytes. */
    level(HY_LCD_DC, 0);
    level(HY_LCD_CS, 0);
    byte_out(0x2C);
    level(HY_LCD_DC, 1);
    uapi_tcxo_delay_us(2);
    for (y = 0; y < HY_LCD_HEIGHT; ++y) {
        for (x = 0; x < HY_LCD_WIDTH; ++x) {
            uint16_t pixel = bars ? palette[x / 40] : color;
            byte_out((uint8_t)(pixel >> 8));
            byte_out((uint8_t)pixel);
            if (s_error != 0) return s_error;
        }
        uapi_watchdog_kick();
        if ((y & 3u) == 3u) osal_msleep(1);
    }
    level(HY_LCD_CS, 1);
    return s_error;
}

uint32_t hy_lcd_fill(uint16_t rgb565) { return draw(rgb565, 0); }
uint32_t hy_lcd_bars(void) { return draw(0, 1); }
