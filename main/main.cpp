#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_spi_flash.h"
#include "esp_task_wdt.h"
#include "spi_drv/spi_drv.h"
#include <AP_server.h>
//ESP32-WROOM-32U MODE DIO 4MB
//____________________________________________________________________________________________________________//

#define _ledRedOff      gpio_set_level( GPIOLED0, 1 ) //off
#define _ledRedOn       gpio_set_level( GPIOLED0, 1 ); //on
#define LED_ON          LOW
#define LED_OFF         HIGH
#define GPIOSW          0     // button (0), dev board v.4
#define _getButton      digitalRead(GPIOSW) //active=LOW

using namespace std;

// GPIOSW interrupt
static void IRAM_ATTR gpio_SW_handler(void* arg);
WORD_ALIGNED_ATTR char rxbuf[128];
//____________________________________________________________________________________________________________//
const char* tstr="+Returns ABCD string for esp_err_t and system error codes This function finds the error code in a pre-generated lookup-table of esp_err_t errors and returns its string representation. If the error code is not found then it is attempted to be found among system errors. The function is generated by the Python script tools/gen_esp_err_to_name.py which should be run each time an esp_err_t error is modified, created or removed from the IDF project+";
//____________________________________________________________________________________________________________// 
extern "C" void app_main()
{
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0); //disable brownout detector 
  delay(100);
  initLED(GPIOLED0);
  //GPIO config for the GPIOSW line.
  gpioInit(GPIOSW,GPIO_MODE_INPUT,GPIO_PULLUP_ENABLE,GPIO_PULLDOWN_DISABLE,GPIO_INTR_POSEDGE);
  //Set up GPIOSW line interrupt.
  gpio_install_isr_service(0);
  gpio_set_intr_type((gpio_num_t)GPIOSW, GPIO_INTR_POSEDGE);
  gpio_isr_handler_add((gpio_num_t)GPIOSW, gpio_SW_handler, NULL);

  //=================================
  println("Start SPI...");
  //spi_master_config(true);
  spi_master_config(false);
  WiFi_AP_start();
  while(1){
    toggleLED();
    delay(1000);
    //println("toogle led...");
  }
  //=================================
  while(0){
    toggleLED();
    delay(1000);
    //println("toogle led...");
  }

  while(1){
    memset(rxbuf,0,128);
    esp_err_t e=syncTX((uint8_t*)tstr,448,(uint8_t*)rxbuf,64);
    //if(ite)Serial.println("===> RES ERROR="+String(esp_err_to_name_r(ite,errbuf,64))+"\n");
    //esp_err_t e=syncTX((uint8_t*)tstr,strlen(tstr),(uint8_t*)rxbuf,64);
    //uint32_t v=*((uint32_t*)rxbuf);
    
    if(e){//e!=BUSY){
      printf(" * ERROR : %s , queue=%d",get_errstring(e).c_str(),queue_cntr);
    }
    else {
      //Serial.print(".");//sync_transaction OK\n");
      printf(rxbuf);
    }

    delay(1);
    //delay(2000);
  }
}
//____________________________________________________________________________________________________________//
// GPIOSW interrupt
static void IRAM_ATTR gpio_SW_handler(void* arg)
{
    println("REBOOT");
    esp_restart();
}
//____________________________________________________________________________________________________________//