#ifndef __LCD_INIT_H
#define __LCD_INIT_H

#include "gpio.h"
#include "tcxo.h"
#include "pinctrl.h"
#include "soc_osal.h"

#define LCD_SCL_PIN     7  //CK
#define LCD_SDA_PIN     9  //MOSI
#define LCD_RST_PIN     12 //RST
#define LCD_DC_PIN      10 //DC
#define LCD_CS_PIN      11 //CS

/* 四个信号脚运行时可换。
 *
 * 原因：这块扩展板的 LCD1 座是按 VCC GND SCL SDA DC CS RST 排的，而模块
 * 手册给的管脚顺序是 VCC GND DIN CLK CS DC RST —— 只要实物和座子有一处
 * 对不上（哪怕是 DC/CS 互换），屏就表现为「背光亮、画面全灰」。所以把
 * 角色做成变量，固件里逐个排列试，用颜色区分，见 lcd_role_sweep()。 */
extern int g_lcd_clk_pin;
extern int g_lcd_mosi_pin;
extern int g_lcd_dc_pin;
extern int g_lcd_cs_pin;
extern int g_lcd_rst_pin;
void LCD_SetPins(int clk, int mosi, int dc, int cs);
void LCD_SetClkPin(int pin);
void LCD_SetRstPin(int pin);


#define USE_HORIZONTAL 0  //设置横屏或者竖屏显示 0或1为竖屏 2或3为横屏

#define LCD_W 240
#define LCD_H 240

//-----------------LCD端口定义----------------
/* 完全恢复官方 200_lcd_display 例程的 GPIO 写法（uapi_gpio_set_val）。
 *
 * 说明：之前那套「寄存器直写」会在访问 IO9（channel 1）时把 UI 任务搞挂，
 * 而官方例程用这套 uapi 路径是验证过能点亮屏的。先把屏点亮、确认硬件通路，
 * 速度优化（寄存器直写）等屏亮之后单独排查 channel 1 的差异再做。 */
#define LCD_SCLK_Clr() uapi_gpio_set_val(g_lcd_clk_pin, 0)   //SCL=SCLK
#define LCD_SCLK_Set() uapi_gpio_set_val(g_lcd_clk_pin, 1)

#define LCD_MOSI_Clr() uapi_gpio_set_val(g_lcd_mosi_pin, 0)  //SDA=MOSI
#define LCD_MOSI_Set() uapi_gpio_set_val(g_lcd_mosi_pin, 1)

#define LCD_RES_Clr()  uapi_gpio_set_val(g_lcd_rst_pin, 0)   //RES
#define LCD_RES_Set()  uapi_gpio_set_val(g_lcd_rst_pin, 1)

#define LCD_DC_Clr()   uapi_gpio_set_val(g_lcd_dc_pin, 0)    //DC
#define LCD_DC_Set()   uapi_gpio_set_val(g_lcd_dc_pin, 1)

#define LCD_CS_Clr()   uapi_gpio_set_val(g_lcd_cs_pin, 0)    //CS
#define LCD_CS_Set()   uapi_gpio_set_val(g_lcd_cs_pin, 1)

/* 软件 SPI 半周期延时：官方例程不插延时。先给一个较小的 nop 保证时序稳定，
 * 等屏亮后再调。 */
#define LCD_SPI_NOP      20
#define LCD_SPI_DELAY()  do { \
        volatile unsigned int _dl; \
        for (_dl = 0; _dl < LCD_SPI_NOP; _dl++) { __asm__ volatile ("nop"); } \
    } while (0)


typedef unsigned char u8;
typedef unsigned short int u16;

/* 引脚回读自检开关。
 *
 * 实测（v12）：回读 IO7/IO9/IO10 都是 =1/0 OK，走到 IO11 时 UI 任务就没了
 * （系统还活着：I2C 报错、温补日志照常打），说明这一步有问题。
 * 但此前整屏填充能跑完 461 万次 CS 翻转，说明 IO11 本身是可用的。
 * 结论：回读只用来排查，不进正式启动路径。 */
#define LCD_PIN_PROBE    0

void LCD_GPIO_Init(void);//初始化GPIO
void LCD_PinProbe(void);//引脚回读自检（打印到串口）
void LCD_Writ_Bus(u8 dat);//模拟SPI时序
void LCD_WR_DATA8(u8 dat);//写入一个字节
void LCD_WR_DATA(u16 dat);//写入两个字节
void LCD_WR_REG(u8 dat);//写入一个指令
void LCD_Address_Set(u16 x1,u16 y1,u16 x2,u16 y2);//设置坐标函数
void LCD_Init(void);//LCD初始化

#endif

