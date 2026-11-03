#include "lcd.h"
#include "iot_spi.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned calls,bytes,large_calls,yields,maxlen,pixel_bytes,windows;static int fail,dc;static unsigned char cmd;
static unsigned char params[4];static unsigned param_count;static unsigned x0,y0,x1,y1,pixels,expected;static int writing;
static void finish(void){if(writing&&!fail)assert(pixels==expected);writing=0;}
unsigned IoTSpiInit(unsigned id,IoT_SPI_InitTypeDef*p){return 0;}
unsigned IoTSpiDeinit(unsigned id){return 0;}
unsigned IoTGpioInit(unsigned id){return 0;}
unsigned IoTGpioDeinit(unsigned id){return 0;}
unsigned IoTGpioSetDir(unsigned id,int value){return 0;}
unsigned IoTGpioSetOutputVal(unsigned id,int value){if(id==4)dc=value;return 0;}
void LOS_Msleep(unsigned ms){if(ms==1)yields++;}
unsigned IoTSpiWrite(unsigned id,unsigned char*data,unsigned n){
 calls++;bytes+=n;if(n>1)large_calls++;if(n>maxlen)maxlen=n;
 if(fail)return 1;
 if(!dc){assert(n==1);finish();cmd=data[0];param_count=0;
  if(cmd==0x2c){assert(x0<=x1&&y0<=y1&&x1<LCD_W&&y1<LCD_H);expected=(x1-x0+1)*(y1-y0+1)*2;pixels=0;writing=1;windows++;}
 }else if(cmd==0x2c){pixels+=n;pixel_bytes+=n;assert(pixels<=expected);}
 else if(cmd==0x2a||cmd==0x2b){for(unsigned i=0;i<n;i++){assert(param_count<4);params[param_count++]=data[i];}
  if(param_count==4){unsigned a=params[0]*256+params[1],b=params[2]*256+params[3];if(cmd==0x2a){x0=a;x1=b;}else{y0=a;y1=b;}}
 }
 return 0;
}
static void reset(void){finish();calls=bytes=large_calls=yields=maxlen=pixel_bytes=windows=0;}
int main(void){
 dc=1;lcd_fill(0,0,LCD_W,LCD_H,LCD_WHITE);finish();
 assert(pixels==LCD_W*LCD_H*2);assert(calls<1300&&maxlen<=128&&large_calls>=1200&&yields==30);
 printf("PASS full clear: %u SPI calls, %u cooperative yields (formerly over 153600 byte calls)\n",calls,yields);
 reset();lcd_fill(1,1,1,1,0);lcd_fill(65535,65535,0,0,0);assert(!calls);
 lcd_fill(319,239,65535,65535,0);finish();assert(pixels==2);
 reset();unsigned char mixed[]="K6设置 08:00";lcd_show_chinese(0,0,mixed,LCD_BLACK,LCD_WHITE,16,0);finish();assert(calls>0&&maxlen<=128&&windows==10&&pixel_bytes==3072);
 for(unsigned n=1;n<4;n++){
  unsigned char *bad=malloc(n+1);assert(bad);memset(bad,0xE4,n);bad[n]=0;lcd_show_chinese(0,0,bad,0,0,16,0);finish();free(bad);
 }
 unsigned char *long_text=malloc(4097);assert(long_text);memset(long_text,'A',4096);long_text[4096]=0;
 reset();lcd_show_chinese(0,0,long_text,0,0,16,0);finish();assert(calls<1000);free(long_text);
 reset();lcd_show_chinese(319,0,mixed,0,0,16,0);lcd_show_chinese(0,239,mixed,0,0,16,0);lcd_show_char(0,0,255,0,0,16,0);finish();
 reset();fail=1;lcd_fill(0,0,320,240,0);assert(calls<20);fail=0;writing=0;
 puts("PASS real LCD driver: exact pixel counts, clipping, mixed UTF-8/ASCII, truncated sequences, long strings and bounded SPI failure");
}
