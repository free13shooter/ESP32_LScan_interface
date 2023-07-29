
#ifndef __SPIDRV_H
#define __SPIDRV_H
//____________________________________________________________________________________________________________//
#include "functions.hpp" 
#include "soc/rtc_cntl_reg.h"  //disable brownout problems
#include "hal/spi_types.h"
#include "driver/spi_common.h"
#include "driver/spi_master.h"
#include "driver/spi_slave.h"
#include <string>
#include "esp_task_wdt.h"

using namespace std;

#ifdef  __cplusplus
extern "C" {
#endif
//____________________________________________________________________________________________________________//
//при старте esp уровень сигнала на ноге 12 определяет питание встроенной флеш памяти, т.е. может привести к невозможности загрузить esp32
#ifndef SOC_TX0
#if CONFIG_IDF_TARGET_ESP32
#define SOC_TX0 1
#define SOC_TX2 17
#elif CONFIG_IDF_TARGET_ESP32S2
#define SOC_TX0 43
#elif CONFIG_IDF_TARGET_ESP32C3
#define SOC_TX0 21
#endif
#endif

#ifndef SOC_RX0
#if CONFIG_IDF_TARGET_ESP32
#define SOC_RX0 3
#define SOC_RX2 16
#elif CONFIG_IDF_TARGET_ESP32S2
#define SOC_RX0 44
#elif CONFIG_IDF_TARGET_ESP32C3
#define SOC_RX0 20
#endif
#endif

#define  INTERRUPT_ATTR IRAM_ATTR
//#define  INTERRUPT_ATTR ICACHE_RAM_ATTR
/*
SPI	  MOSI  	MISO	  CLK	    CS
HSPI	GPIO13	GPIO12	GPIO14	GPIO15 == SPI2
VSPI	GPIO23	GPIO19	GPIO18	GPIO5  == SPI3

      SPI_HOST   ===   SPI1_HOST
      HSPI_HOST  ===   SPI2_HOST
      VSPI_HOST  ===   SPI3_HOST
*/

#define GPIO_HANDSHAKE  2 //pin handshake

#define SPIH            VSPI_HOST //spi host


// use HSPI or VSPI with default pin assignment
// VSPI (CS:  5, CLK: 18, MOSI: 23, MISO: 19) -> default
// HSPI (CS: 15, CLK: 14, MOSI: 13, MISO: 12) - used led in 13 pin (dev board v.4)
#if SPIH==VSPI_HOST
  #define SPI_SCK 18    
  #define SPI_MISO 19
  #define SPI_MOSI 23   
  #define SPI_CS 5   
#else // HSPI_HOST
  #define SPI_SCK 14  // - PB3  STM32
  #define SPI_MISO 12 // - PB4  STM32
  #define SPI_MOSI 13 // - PB5  STM32    
  #define SPI_CS 15   // - PA15 STM32 + HANDSHAKE 2 IN <- PB8 STM32 OUT
#endif

#define SPI_SOFT_CS 5 //soft chip select

#ifdef SPI_SOFT_CS
  #undef SPI_CS
#endif

#define SPI_DMA_MODE    1//SPI_DMA_DISABLED//SPI_DMA_CH_AUTO
#define TICKS_WAIT      200
#define SPI_FREQ        SPI_MASTER_FREQ_20M 
#define MAX_QUEUE       8 //максимум в очереди
//--------------------------------------------//
extern spi_bus_config_t buscfg;
extern volatile uint32_t queue_cntr;      //queue counter
extern volatile uint32_t sampleLen;       //длина пакета передачи/приема
extern volatile uint32_t CRC_retry;       //повторить передачу CRC_retry раз при ошибке CRC
extern volatile int spi_stream_mode;
//-----------------
//вернет тектовое описание ошибки
string get_errstring(esp_err_t error);
//инициализация - опрос готовности до получения "READY"
esp_err_t spi_master_config(bool waitREADYstring=false,bool need_install_GPIO_isr_service=false);
esp_err_t reinitSlaveOverCS();

//поместить в очередь асинхронной передачи
esp_err_t asyncTX64(uint8_t* txbuf,uint32_t txlen,uint8_t* rxbuf=NULL,uint32_t rxlen=0);
//синхронная передача любой размер: rxlen-длина приемного буфера
esp_err_t syncTXRXlong(uint8_t* txbuf,uint32_t txlen,uint8_t* rxbuf=NULL,uint32_t rxlen=0,uint32_t* pRxTotal=NULL, TickType_t wait=TICKS_WAIT);
//синхронная передача до 64 байтов:  rxlen-длина приемного буфера
esp_err_t syncTXRX(uint8_t* txbuf,uint32_t txlen,uint8_t* rxbuf=NULL,uint32_t rxlen=0,uint32_t* pRxTotal=NULL, TickType_t wait=TICKS_WAIT);

//синхронная передача . 
esp_err_t spixfer(uint8_t* txbuf,uint32_t txlen,int* iv=NULL,TickType_t wait=TICKS_WAIT);
esp_err_t spitx(uint8_t* txbuf,uint32_t txlen,int* iv=NULL,TickType_t wait=TICKS_WAIT);

esp_err_t device_spi_get_ready(int cyclesMax=3);
void device_spi_dma_reset();
void reset_device();//reboot over cs
//____________________________________________________________________________________________________________//
//CHIP SELECT PIN=0
inline void spi_cs_low()
{
#ifdef SPI_CS
  gpio_set_level((gpio_num_t)SPI_CS, 0);
#endif
#ifdef SPI_SOFT_CS
  gpio_set_level((gpio_num_t)SPI_SOFT_CS, 0);
#endif
}
//CHIP SELECT PIN=1
inline void spi_cs_high()
{
#ifdef SPI_CS
  gpio_set_level((gpio_num_t)SPI_CS, 1);
#endif
#ifdef SPI_SOFT_CS
  gpio_set_level((gpio_num_t)SPI_SOFT_CS, 1);
#endif
}
//____________________________________________________________________________________________________________//
#ifdef  __cplusplus
};
#endif

#endif //__SPIDRV_H