#include "lcd_init.h"

int g_lcd_clk_pin  = LCD_SCL_PIN;
int g_lcd_mosi_pin = LCD_SDA_PIN;
int g_lcd_dc_pin   = LCD_DC_PIN;
int g_lcd_cs_pin   = LCD_CS_PIN;
int g_lcd_rst_pin  = LCD_RST_PIN;

void LCD_SetRstPin(int pin)
{
	g_lcd_rst_pin = pin;
	uapi_pin_set_mode(pin, 0);
	uapi_gpio_set_dir(pin, GPIO_DIRECTION_OUTPUT);
	uapi_gpio_set_val(pin, 1);
}

void LCD_SetClkPin(int pin)
{
	g_lcd_clk_pin = pin;
	uapi_pin_set_mode(pin, 0);
	uapi_gpio_set_dir(pin, GPIO_DIRECTION_OUTPUT);
	uapi_gpio_set_val(pin, 1);
}

/* 一次性换掉 4 个信号脚的「角色」，用于扫引脚排列。
 * 只配方向，不碰电平，随后 LCD_Init -> LCD_GPIO_Init 会重新按当前角色配置。 */
void LCD_SetPins(int clk, int mosi, int dc, int cs)
{
	g_lcd_clk_pin  = clk;
	g_lcd_mosi_pin = mosi;
	g_lcd_dc_pin   = dc;
	g_lcd_cs_pin   = cs;
}

/* 引脚级自检：只用官方 uapi 路径逐个翻转并读回，确认引脚真的在动。
 *
 * 只测 LCD 真正要用的 5 个脚（IO7/9/10/11/12，手册《1.28寸SPI圆屏》引脚选择一节
 * 就是这 5 个）。
 *
 * 踩过的坑：之前为了找飞线落点，把 IO0~IO6、IO13、IO14 也塞进来扫，结果一碰
 * IO3 整机就硬 fault —— IO0~IO5 是 QSPI0 主 Flash 的引脚，程序 XIP 就地跑在
 * 那片 Flash 上，改它的复用功能等于把取指总线拆了，必崩。这类脚永远不要碰。 */
void LCD_PinProbe(void)
{
	struct {
		const char *name;
		int pin;
	} tab[] = {
		{ "CS  ", 11 },
		{ "DC  ", 10 },
		{ "RST ", 12 },
		{ "MOSI", 9  },
		{ "CLK ", 7  },
	};
	int i;
	int good = 0;

	for (i = 0; i < (int)(sizeof(tab) / sizeof(tab[0])); i++) {
		int h, l;

		printf("[pin] probe IO%-2d %s ...\r\n", tab[i].pin, tab[i].name);

		uapi_pin_set_mode(tab[i].pin, 0);
		uapi_gpio_set_dir(tab[i].pin, GPIO_DIRECTION_OUTPUT);
		uapi_gpio_set_val(tab[i].pin, 1);
		h = (int)uapi_gpio_get_val(tab[i].pin);
		uapi_gpio_set_val(tab[i].pin, 0);
		l = (int)uapi_gpio_get_val(tab[i].pin);

		if (h == 1 && l == 0) {
			good++;
			printf("[pin] IO%-2d %s =%d/%d  OK\r\n", tab[i].pin, tab[i].name, h, l);
		} else {
			printf("[pin] IO%-2d %s =%d/%d  FAIL\r\n", tab[i].pin, tab[i].name, h, l);
		}
	}
	printf("[pin] ==== %d/%d pins OK ====\r\n", good, (int)(sizeof(tab) / sizeof(tab[0])));
}

void LCD_GPIO_Init(void)
{
	/* 逐脚打印：万一某个脚配置时把任务搞挂，串口最后一行就能指出是哪个脚。
	 * 只在这一处打，LCD_Init 全程只调一次，不占带宽。 */
	printf("[gpio] cfg");
	uapi_pin_set_mode(g_lcd_clk_pin, 0);
    uapi_gpio_set_dir(g_lcd_clk_pin, GPIO_DIRECTION_OUTPUT);
	printf(" clk=%d", g_lcd_clk_pin);
	uapi_pin_set_mode(g_lcd_mosi_pin, 0);
    uapi_gpio_set_dir(g_lcd_mosi_pin, GPIO_DIRECTION_OUTPUT);
	printf(" mosi=%d", g_lcd_mosi_pin);
	uapi_pin_set_mode(g_lcd_cs_pin, 0);
    uapi_gpio_set_dir(g_lcd_cs_pin, GPIO_DIRECTION_OUTPUT);
	printf(" cs=%d", g_lcd_cs_pin);
	uapi_pin_set_mode(g_lcd_dc_pin, 0);
    uapi_gpio_set_dir(g_lcd_dc_pin, GPIO_DIRECTION_OUTPUT);
	printf(" dc=%d", g_lcd_dc_pin);
	uapi_pin_set_mode(g_lcd_rst_pin, 0);
    uapi_gpio_set_dir(g_lcd_rst_pin, GPIO_DIRECTION_OUTPUT);
	printf(" rst=%d\r\n", g_lcd_rst_pin);

	/* 方向都配好之后，把控制线拉到空闲电平 */
	LCD_SCLK_Set();
	LCD_CS_Set();
	LCD_DC_Set();
	LCD_RES_Set();
	printf("[gpio] idle levels set\r\n");
}



/*********
	

*********************************************************************
      函数说明：LCD串行数据写入函数
      入口数据：dat  要写入的串行数据
      返回值：  无
******************************************************************************/
void LCD_Writ_Bus(u8 dat) 
{	
	u8 i;
	LCD_CS_Clr();
	for(i=0;i<8;i++)
	{			  
		LCD_SCLK_Clr();
		if(dat&0x80)
		{
		   LCD_MOSI_Set();
		}
		else
		{
		   LCD_MOSI_Clr();
		}
		/* WS63 主频高，GPIO 翻转在 10ns 级，GC9A01 撑不住这么快。
		 * 这里插入约 200ns 忙等，把 SPI 时钟压到 ~5MHz，屏才收得稳。 */
		LCD_SPI_DELAY();
		LCD_SCLK_Set();
		LCD_SPI_DELAY();
		dat<<=1;
	}	
  LCD_CS_Set();	
}


/******************************************************************************
      函数说明：LCD写入数据
      入口数据：dat 写入的数据
      返回值：  无
******************************************************************************/
void LCD_WR_DATA8(u8 dat)
{
	LCD_Writ_Bus(dat);
}


/******************************************************************************
      函数说明：LCD写入数据
      入口数据：dat 写入的数据
      返回值：  无
******************************************************************************/
void LCD_WR_DATA(u16 dat)
{
	LCD_Writ_Bus(dat>>8);
	LCD_Writ_Bus(dat);
}


/******************************************************************************
      函数说明：LCD写入命令
      入口数据：dat 写入的命令
      返回值：  无
******************************************************************************/
void LCD_WR_REG(u8 dat)
{
	LCD_DC_Clr();//写命令
	LCD_Writ_Bus(dat);
	LCD_DC_Set();//写数据
}


/******************************************************************************
      函数说明：设置起始和结束地址
      入口数据：x1,x2 设置列的起始和结束地址
                y1,y2 设置行的起始和结束地址
      返回值：  无
******************************************************************************/
void LCD_Address_Set(u16 x1,u16 y1,u16 x2,u16 y2)
{
		LCD_WR_REG(0x2a);//列地址设置
		LCD_WR_DATA(x1);
		LCD_WR_DATA(x2);
		LCD_WR_REG(0x2b);//行地址设置
		LCD_WR_DATA(y1);
		LCD_WR_DATA(y2);
		LCD_WR_REG(0x2c);//储存器写
}

void LCD_Init(void)
{
	LCD_GPIO_Init();//初始化GPIO
	
	LCD_RES_Clr();//复位
	uapi_tcxo_delay_ms(100);
	LCD_RES_Set();
	uapi_tcxo_delay_ms(100);
	
	LCD_WR_REG(0xEF);
	LCD_WR_REG(0xEB);
	LCD_WR_DATA8(0x14); 
	
  	LCD_WR_REG(0xFE);			 
	LCD_WR_REG(0xEF); 

	LCD_WR_REG(0xEB);	
	LCD_WR_DATA8(0x14); 

	LCD_WR_REG(0x84);			
	LCD_WR_DATA8(0x40); 

	LCD_WR_REG(0x85);			
	LCD_WR_DATA8(0xFF); 

	LCD_WR_REG(0x86);			
	LCD_WR_DATA8(0xFF); 

	LCD_WR_REG(0x87);			
	LCD_WR_DATA8(0xFF);

	LCD_WR_REG(0x88);			
	LCD_WR_DATA8(0x0A);

	LCD_WR_REG(0x89);			
	LCD_WR_DATA8(0x21); 

	LCD_WR_REG(0x8A);			
	LCD_WR_DATA8(0x00); 

	LCD_WR_REG(0x8B);			
	LCD_WR_DATA8(0x80); 

	LCD_WR_REG(0x8C);			
	LCD_WR_DATA8(0x01); 

	LCD_WR_REG(0x8D);			
	LCD_WR_DATA8(0x01); 

	LCD_WR_REG(0x8E);			
	LCD_WR_DATA8(0xFF); 

	LCD_WR_REG(0x8F);			
	LCD_WR_DATA8(0xFF); 


	LCD_WR_REG(0xB6);
	LCD_WR_DATA8(0x00);
	LCD_WR_DATA8(0x20);

	LCD_WR_REG(0x36);
	if(USE_HORIZONTAL==0)LCD_WR_DATA8(0x08);
	else if(USE_HORIZONTAL==1)LCD_WR_DATA8(0xC8);
	else if(USE_HORIZONTAL==2)LCD_WR_DATA8(0x68);
	else LCD_WR_DATA8(0xA8);

	LCD_WR_REG(0x3A);			
	LCD_WR_DATA8(0x05); 


	LCD_WR_REG(0x90);			
	LCD_WR_DATA8(0x08);
	LCD_WR_DATA8(0x08);
	LCD_WR_DATA8(0x08);
	LCD_WR_DATA8(0x08); 

	LCD_WR_REG(0xBD);			
	LCD_WR_DATA8(0x06);
	
	LCD_WR_REG(0xBC);			
	LCD_WR_DATA8(0x00);	

	LCD_WR_REG(0xFF);			
	LCD_WR_DATA8(0x60);
	LCD_WR_DATA8(0x01);
	LCD_WR_DATA8(0x04);

	LCD_WR_REG(0xC3);			
	LCD_WR_DATA8(0x13);
	LCD_WR_REG(0xC4);			
	LCD_WR_DATA8(0x13);

	LCD_WR_REG(0xC9);			
	LCD_WR_DATA8(0x22);

	LCD_WR_REG(0xBE);			
	LCD_WR_DATA8(0x11); 

	LCD_WR_REG(0xE1);			
	LCD_WR_DATA8(0x10);
	LCD_WR_DATA8(0x0E);

	LCD_WR_REG(0xDF);			
	LCD_WR_DATA8(0x21);
	LCD_WR_DATA8(0x0c);
	LCD_WR_DATA8(0x02);

	LCD_WR_REG(0xF0);   
	LCD_WR_DATA8(0x45);
	LCD_WR_DATA8(0x09);
	LCD_WR_DATA8(0x08);
	LCD_WR_DATA8(0x08);
	LCD_WR_DATA8(0x26);
 	LCD_WR_DATA8(0x2A);

 	LCD_WR_REG(0xF1);    
 	LCD_WR_DATA8(0x43);
 	LCD_WR_DATA8(0x70);
 	LCD_WR_DATA8(0x72);
 	LCD_WR_DATA8(0x36);
 	LCD_WR_DATA8(0x37);  
 	LCD_WR_DATA8(0x6F);


 	LCD_WR_REG(0xF2);   
 	LCD_WR_DATA8(0x45);
 	LCD_WR_DATA8(0x09);
 	LCD_WR_DATA8(0x08);
 	LCD_WR_DATA8(0x08);
 	LCD_WR_DATA8(0x26);
 	LCD_WR_DATA8(0x2A);

 	LCD_WR_REG(0xF3);   
 	LCD_WR_DATA8(0x43);
 	LCD_WR_DATA8(0x70);
 	LCD_WR_DATA8(0x72);
 	LCD_WR_DATA8(0x36);
 	LCD_WR_DATA8(0x37); 
 	LCD_WR_DATA8(0x6F);

	LCD_WR_REG(0xED);	
	LCD_WR_DATA8(0x1B); 
	LCD_WR_DATA8(0x0B); 

	LCD_WR_REG(0xAE);			
	LCD_WR_DATA8(0x77);
	
	LCD_WR_REG(0xCD);			
	LCD_WR_DATA8(0x63);		


	LCD_WR_REG(0x70);			
	LCD_WR_DATA8(0x07);
	LCD_WR_DATA8(0x07);
	LCD_WR_DATA8(0x04);
	LCD_WR_DATA8(0x0E); 
	LCD_WR_DATA8(0x0F); 
	LCD_WR_DATA8(0x09);
	LCD_WR_DATA8(0x07);
	LCD_WR_DATA8(0x08);
	LCD_WR_DATA8(0x03);

	LCD_WR_REG(0xE8);			
	LCD_WR_DATA8(0x34);

	LCD_WR_REG(0x62);			
	LCD_WR_DATA8(0x18);
	LCD_WR_DATA8(0x0D);
	LCD_WR_DATA8(0x71);
	LCD_WR_DATA8(0xED);
	LCD_WR_DATA8(0x70); 
	LCD_WR_DATA8(0x70);
	LCD_WR_DATA8(0x18);
	LCD_WR_DATA8(0x0F);
	LCD_WR_DATA8(0x71);
	LCD_WR_DATA8(0xEF);
	LCD_WR_DATA8(0x70); 
	LCD_WR_DATA8(0x70);

	LCD_WR_REG(0x63);			
	LCD_WR_DATA8(0x18);
	LCD_WR_DATA8(0x11);
	LCD_WR_DATA8(0x71);
	LCD_WR_DATA8(0xF1);
	LCD_WR_DATA8(0x70); 
	LCD_WR_DATA8(0x70);
	LCD_WR_DATA8(0x18);
	LCD_WR_DATA8(0x13);
	LCD_WR_DATA8(0x71);
	LCD_WR_DATA8(0xF3);
	LCD_WR_DATA8(0x70); 
	LCD_WR_DATA8(0x70);

	LCD_WR_REG(0x64);			
	LCD_WR_DATA8(0x28);
	LCD_WR_DATA8(0x29);
	LCD_WR_DATA8(0xF1);
	LCD_WR_DATA8(0x01);
	LCD_WR_DATA8(0xF1);
	LCD_WR_DATA8(0x00);
	LCD_WR_DATA8(0x07);

	LCD_WR_REG(0x66);			
	LCD_WR_DATA8(0x3C);
	LCD_WR_DATA8(0x00);
	LCD_WR_DATA8(0xCD);
	LCD_WR_DATA8(0x67);
	LCD_WR_DATA8(0x45);
	LCD_WR_DATA8(0x45);
	LCD_WR_DATA8(0x10);
	LCD_WR_DATA8(0x00);
	LCD_WR_DATA8(0x00);
	LCD_WR_DATA8(0x00);

	LCD_WR_REG(0x67);			
	LCD_WR_DATA8(0x00);
	LCD_WR_DATA8(0x3C);
	LCD_WR_DATA8(0x00);
	LCD_WR_DATA8(0x00);
	LCD_WR_DATA8(0x00);
	LCD_WR_DATA8(0x01);
	LCD_WR_DATA8(0x54);
	LCD_WR_DATA8(0x10);
	LCD_WR_DATA8(0x32);
	LCD_WR_DATA8(0x98);

	LCD_WR_REG(0x74);			
	LCD_WR_DATA8(0x10);	
	LCD_WR_DATA8(0x85);	
	LCD_WR_DATA8(0x80);
	LCD_WR_DATA8(0x00); 
	LCD_WR_DATA8(0x00); 
	LCD_WR_DATA8(0x4E);
	LCD_WR_DATA8(0x00);					
	
  	LCD_WR_REG(0x98);			
	LCD_WR_DATA8(0x3e);
	LCD_WR_DATA8(0x07);

	LCD_WR_REG(0x35);	
	LCD_WR_REG(0x21);

	LCD_WR_REG(0x11);
	uapi_tcxo_delay_ms(120);
	LCD_WR_REG(0x29);
	uapi_tcxo_delay_ms(20);
} 







