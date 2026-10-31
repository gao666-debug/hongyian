#ifndef HONGYIAN_LCD_BRINGUP_H
#define HONGYIAN_LCD_BRINGUP_H
#include <stdint.h>

/* EXB V1 + WEAR V0, supplier 200_lcd_display GPIO numbers. */
#define HY_LCD_CLK 7
#define HY_LCD_MOSI 9
#define HY_LCD_DC 10
#define HY_LCD_CS 11
#define HY_LCD_RST 12
#define HY_LCD_WIDTH 240
#define HY_LCD_HEIGHT 240

uint32_t hy_lcd_init(void);
uint32_t hy_lcd_fill(uint16_t rgb565);
uint32_t hy_lcd_bars(void);
#endif
