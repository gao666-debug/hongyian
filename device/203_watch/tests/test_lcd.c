/* Host protocol test: fake GPIO edges, decode exactly what panel would receive.
 * Does not prove physical wiring, voltage, clock waveform, or SDK compatibility. */
#include "lcd_bringup.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int pins[32];
static unsigned int bits, value, bytes, commands, pixels, watchdogs, windows;
static unsigned int expected_pixel, pixel_half, pixel_value;
static unsigned int payload, param_value, sleep_out, display_on, format;
static uint8_t current_cmd;
static int fail_pin = -1, fail_mux, expect_bars;

static int allowed(int pin)
{
    return pin == 7 || pin == 9 || pin == 10 || pin == 11 || pin == 12;
}

static void receive_byte(uint8_t byte)
{
    ++bytes;
    if (!pins[HY_LCD_DC]) {
        ++commands;
        current_cmd = byte;
        payload = param_value = 0;
        if (byte == 0x11) ++sleep_out;
        if (byte == 0x29) ++display_on;
    } else if (current_cmd == 0x2C) {
        if (!pixel_half) { pixel_value = byte << 8; pixel_half = 1; }
        else {
            static const unsigned int palette[] = {0xF800, 0x07E0, 0x001F, 0xFFFF, 0, 0xFFE0};
            pixel_value |= byte;
            assert(pixel_value == (expect_bars ? palette[(pixels % 240) / 40] : expected_pixel));
            ++pixels;
            pixel_half = 0;
        }
    } else {
        /* Raising CS must not split command and its parameters. */
        ++payload;
        param_value = (param_value << 8) | byte;
        if (current_cmd == 0x3A) { assert(byte == 0x05); ++format; }
        if ((current_cmd == 0x2A || current_cmd == 0x2B) && payload == 4) {
            assert(param_value == 239); ++windows;
        }
    }
}

uint32_t uapi_gpio_set_val(int pin, int val)
{
    assert(allowed(pin));
    if (pin == fail_pin) return 0x80001234u;
    if (pin == HY_LCD_CS && val == 1 && pins[pin] == 0) {
        assert(bits == 0);
        if (current_cmd != 0x2C) {
            /* Check initialized commands with nonempty parameters kept CS low. */
            if (current_cmd == 0x3A) assert(payload == 1);
            if (current_cmd == 0x2A || current_cmd == 0x2B) assert(payload == 4);
        }
    }
    if (pin == HY_LCD_CLK && val == 1 && !pins[pin] && !pins[HY_LCD_CS]) {
        value = (value << 1) | (unsigned int)pins[HY_LCD_MOSI];
        if (++bits == 8) { receive_byte((uint8_t)value); bits = value = 0; }
    }
    pins[pin] = val;
    return 0;
}
uint32_t uapi_pin_set_mode(int pin, int mode)
{
    assert(allowed(pin)); assert(mode == 0);
    return fail_mux ? 0x80005678u : 0;
}
uint32_t uapi_gpio_set_dir(int pin, int dir)
{
    assert(allowed(pin)); assert(dir == 1); return 0;
}
void uapi_tcxo_delay_us(unsigned int us) { assert(us >= 2); }
void osal_msleep(unsigned int ms) { assert(ms <= 10); }
void uapi_watchdog_kick(void) { ++watchdogs; }

int main(void)
{
    assert(hy_lcd_fill(0) != 0); /* cannot stream before reset/init */
    assert(hy_lcd_init() == 0);
    assert(sleep_out == 1 && display_on == 1 && format == 1);
    assert(commands == 50 && pins[HY_LCD_CS] == 1 && pins[HY_LCD_RST] == 1);
    expected_pixel = 0xF800;
    watchdogs = pixels = windows = 0;
    assert(hy_lcd_fill((uint16_t)expected_pixel) == 0);
    assert(pixels == 240 * 240 && windows == 2 && watchdogs == 240);
    assert(pixel_half == 0 && pins[HY_LCD_CS] == 1 && pins[HY_LCD_CLK] == 0);
    pixels = windows = 0; expect_bars = 1;
    assert(hy_lcd_bars() == 0);
    assert(pixels == 240 * 240 && windows == 2);
    fail_pin = HY_LCD_CLK;
    assert(hy_lcd_fill(0) == 0x80001234u);
    fail_pin = -1; fail_mux = 1;
    assert(hy_lcd_init() == 0x80005678u);
    assert(hy_lcd_fill(0) == 0x80005678u);
    puts("PASS: 50 init commands, CS framing, 57600 RGB565 pixels/frame, bars, bounds, watchdog and API error stop");
    return 0;
}
