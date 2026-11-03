#ifndef LCD_TEST_SPI_H
#define LCD_TEST_SPI_H
#define ESPI0_M1 0
#define SPI_MODE_MASTER 0
#define SPI_DIRECTION_1LINE_TX 1
#define SPI_DATASIZE_8BIT 8
#define SPI_POLARITY_HIGH 1
#define SPI_PHASE_2EDGE 1
#define SPI_BAUDRATEPRESCALER_1 1
#define SPI_FIRSTBIT_MSB 0
typedef struct {int Mode,Direction,DataSize,CLKPolarity,CLKPhase,BaudRatePrescaler,FirstBit;} IoT_SPI_InitTypeDef;
unsigned IoTSpiInit(unsigned,IoT_SPI_InitTypeDef*);
unsigned IoTSpiDeinit(unsigned);
unsigned IoTSpiWrite(unsigned,unsigned char*,unsigned);
#endif
