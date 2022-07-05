
#include "functions.h"

#define NOP() asm volatile ("nop")

//____________________________________________________________________________________________________________//
void initLED(int pin,bool postInitON){
  printf("-config pin %d as LED output. %d=ACTIVE, %d=PASSIVE\n",pin,(int)STATE_LED_ON,(int)STATE_LED_OFF);
  gpioInit((gpio_num_t)pin,GPIO_MODE_INPUT_OUTPUT);
  if(postInitON)ledOn(pin);else ledOff(pin);
}
void ledOn(int pin){gpio_set_level((gpio_num_t)pin,(uint32_t)STATE_LED_ON);};
void ledOff(int pin){gpio_set_level((gpio_num_t)pin,(uint32_t)STATE_LED_OFF);};
int toggleLED(int pin){
  if(gpio_get_level((gpio_num_t)pin)==(int)STATE_LED_ON){ledOff(pin);return STATE_LED_OFF;}
  ledOn(pin);return STATE_LED_ON;
}
int ledState(int pin){return gpio_get_level((gpio_num_t)pin);}
//____________________________________________________________________________________________________________//
unsigned long IRAM_ATTR micros()
{
    return (unsigned long) (esp_timer_get_time());
}
void IRAM_ATTR delayMicroseconds(uint32_t us)
{
    uint32_t m = micros();
    if(us){
        uint32_t e = (m + us);
        if(m > e){ //overflow
            while(micros() > e){
                NOP();
            }
        }
        while(micros() < e){
            NOP();
        }
    }
}
//____________________________________________________________________________________________________________//
int println(const char* str, ...){int r=printf(str); r+=printf("\r\n");return r;}
int print(const char* pstr, ...){return printf((pstr));}
//____________________________________________________________________________________________________________//
void  gpioInit(uint32_t num, gpio_mode_t mode, gpio_pullup_t pullup_enable, gpio_pulldown_t pulldown_enable, gpio_int_type_t interrupt_type)
{
  gpio_config_t io_conf={
    .pin_bit_mask=((uint64_t)1<<num),
    .mode=mode,
    .pull_up_en=pullup_enable,
    .pull_down_en=pulldown_enable,
    .intr_type=interrupt_type,  
  };
  gpio_config(&io_conf);
}
//____________________________________________________________________________________________________________//
portMUX_TYPE timerMux = portMUX_INITIALIZER_UNLOCKED;
int modVolatileInt(int volatile *pInt,int newValue)
{
  int result = *pInt;
  
  do{
    portENTER_CRITICAL_ISR(&timerMux); 
    *pInt=newValue;
    portEXIT_CRITICAL_ISR(&timerMux);
    result = *pInt;
  }while(result!=newValue);
  
  return result;
}
//____________________________________________________________________________________________________________//
int modVolatileInc(int volatile *pInt)
{
  int result = *pInt; int t=result+1;
  do{
    portENTER_CRITICAL_ISR(&timerMux); 
    *pInt=t;
    portEXIT_CRITICAL_ISR(&timerMux);
    result = *pInt;
  }while(result!=t);
  
  return result;
}
//____________________________________________________________________________________________________________//
int modVolatileDec(int volatile *pInt)
{
  int result = *pInt; int t=result-1;
  do{
    portENTER_CRITICAL_ISR(&timerMux); 
    *pInt=t;
    portEXIT_CRITICAL_ISR(&timerMux);
    result = *pInt;
  }while(result!=t);
  
  return result;
}
//____________________________________________________________________________________________________________//
uint64_t getEfuseMac(void)
{
    uint64_t _chipmacid = 0LL;
    esp_efuse_mac_get_default((uint8_t*) (&_chipmacid));
    return _chipmacid;
}

string get_chip_id() {
  char temp[32]={0};
  uint64_t chipid = getEfuseMac(); //The chip ID is essentially its MAC address(length: 6 bytes).
  esp_chip_info_t chip_info;
  esp_chip_info(&chip_info);
  printf("This is ESP32 chip with %d CPU cores, WiFi%s%s, ",
            chip_info.cores,
            (chip_info.features & CHIP_FEATURE_BT) ? "/BT" : "",
            (chip_info.features & CHIP_FEATURE_BLE) ? "/BLE" : "");
  sprintf(temp,"%04X", (uint16_t)(chipid >> 32)); //print High 2 bytes
  sprintf(&temp[4],"%08X", (uint32_t)chipid); //print Low 4bytes.
  printf(" MAC: %08X\n", (uint32_t)chipid); //print 6 bytes.
  return string(temp);
}
//____________________________________________________________________________________________________________//
string mac_toString(uint8_t* mac,bool hex)
{
  char szRet[25];
  if(hex)sprintf(szRet,"%02X:%02X:%02X:%02X:%02X:%02X",mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
  else sprintf(szRet,"%u:%u:%u:%u:%u:%u", mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
  //printf("\n MAC=%s",szRet);
  return string(szRet);
}
//____________________________________________________________________________________________________________//
