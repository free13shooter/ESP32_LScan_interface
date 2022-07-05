
#ifndef __FUNCTIONS_H
#define __FUNCTIONS_H

//____________________________________________________________________________________________________________//
#include "sdkconfig.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "soc/soc.h"  //disable brownout problems
#include "esp_efuse.h"
#include "esp_event_legacy.h"
//#include "esp32-hal-log.h"

#include <string.h>
#include <stdio.h>
#include <string>

//#include "D:/dev/esp-idf/components/lwip/lwip/src/include/lwip/ip4_addr.h"

using namespace std;


#ifdef  __cplusplus
extern "C" {
#endif
//____________________________________________________________________________________________________________//
#ifndef __println
  #define println println 
  #define __println println 
  int println(const char* str, ...);
#endif
#ifndef __print
  #define print print
  #define __print print
  int print(const char* pstr, ...);
#endif

#define GPIOLED0      GPIO_NUM_13    // RED LED ACTIVE FOR LED IS LOW
#define STATE_LED_OFF 1
#define STATE_LED_ON  0

//assoc_index-index in array of leds
void initLED(int pin=GPIOLED0,bool postInitON=false);
void ledOn(int pin=GPIOLED0);
void ledOff(int pin=GPIOLED0);
int toggleLED(int pin=GPIOLED0);
int ledState(int pin=GPIOLED0);

int modVolatileInt(int volatile *pInt,int newValue);
int modVolatileInc(int volatile *pInt);
int modVolatileDec(int volatile *pInt);

unsigned long IRAM_ATTR micros();
void IRAM_ATTR delayMicroseconds(uint32_t us);
inline void delay(uint32_t ms){vTaskDelay(ms / portTICK_PERIOD_MS);} // sleep ms millisec
inline void Sleep(uint32_t ms){vTaskDelay(ms / portTICK_PERIOD_MS);} // sleep ms millisec

//return MAC string
uint64_t getEfuseMac(void);
string get_chip_id();
inline string get_chip_mac(){return get_chip_id();}
string mac_toString(uint8_t* mac,bool hex=true);

//GPIO config.
void  gpioInit (uint32_t num, gpio_mode_t mode=GPIO_MODE_INPUT, gpio_pullup_t pullup_enable=GPIO_PULLUP_DISABLE, gpio_pulldown_t pulldown_enable=GPIO_PULLDOWN_DISABLE,
 gpio_int_type_t interrupt_type=GPIO_INTR_DISABLE);
//____________________________________________________________________________________________________________//
#ifdef  __cplusplus
};
#endif

#endif // __FUNCTIONS_H