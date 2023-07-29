
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
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "soc/soc.h"  //disable brownout problems
#include "esp_efuse.h"
#include "esp_timer.h"
#include "soc/rtc_cntl_reg.h" //disable brownout problems (RTC_CNTL_BROWN_OUT_REG)
#include "esp_system.h"
#include "esp_spi_flash.h"
#include "esp_task_wdt.h"
#include <esp_random.h>
#include "esp32-hal-log.h"

#include <string.h>
#include <stdio.h>
#include <string>
#include <vector>

//#include "D:/dev/esp-idf/components/lwip/lwip/src/include/lwip/ip4_addr.h"

using namespace std;


#ifdef  __cplusplus
extern "C" {
#endif

//____________________________________________________________________________________________________________//
#define BLACK   "30"
#define RED     "31" //ERROR
#define GREEN   "32" //INFO
#define YELLOW  "33" //WARNING
#define BLUE    "34"
#define MAGENTA "35"
#define CYAN    "36" //DEBUG
#define GRAY    "37" //VERBOSE
#define WHITE   "38"

#define _COLOR(COLOR)  "\033[0;" COLOR "m"
#define _BOLD(COLOR)   "\033[1;" COLOR "m"
#define _RESET_COLOR   "\033[0m"

#define _COLOR_PRINT(_clr) printf(_COLOR(_clr))
#define _COLOR_PRINT_END printf(_RESET_COLOR)

#define clr_printf(clr,format) {_COLOR_PRINT(clr);printf(format);_COLOR_PRINT_END;}

//байтов от указателя _fr до указателя _to в кольцевом буфере размером _buf_size
#define _bytesFromTo(_fr,_to,_buf_size) ((_fr)<=(_to)?(int)((_to)-(_fr)):((int)(_buf_size)-((int)(_fr)-(int)(_to))))
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
inline unsigned long IRAM_ATTR millis(){return micros()/1000;}
void IRAM_ATTR delayMicroseconds(uint32_t us);
inline void delay(uint32_t ms){vTaskDelay(ms / portTICK_PERIOD_MS);} // sleep ms millisec
inline void Sleep(uint32_t ms){vTaskDelay(ms / portTICK_PERIOD_MS);} // sleep ms millisec

//return MAC string
uint64_t getEfuseMac(void);
string get_chip_id(bool echoMAC=false);
inline string get_chip_mac(){return get_chip_id();}
string mac_toString(uint8_t* mac,bool hex=true);

string ip32_to_string(uint32_t ip);
string ip4_to_string(esp_ip4_addr_t &ip4);

inline bool is_mac_equivalent(uint8_t mac1[6],uint8_t mac2[6]){
  return (mac1[0]==mac2[0]&&mac1[1]==mac2[1]&&mac1[2]==mac2[2]&&mac1[3]==mac2[3]&&mac1[4]==mac2[4]&&mac1[5]==mac2[5]);
}
inline bool is_macs_equ(uint8_t mac1[6],uint8_t mac2[6]){
  return (mac1[0]==mac2[0]&&mac1[1]==mac2[1]&&mac1[2]==mac2[2]&&mac1[3]==mac2[3]&&mac1[4]==mac2[4]&&mac1[5]==mac2[5]);
}

uint16_t get_sockaddr_port_addr(struct sockaddr *s, char* _ipdest[INET_ADDRSTRLEN]=NULL);

//GPIO config.
void  gpioInit (uint32_t num, gpio_mode_t mode=GPIO_MODE_INPUT, gpio_pullup_t pullup_enable=GPIO_PULLUP_DISABLE, gpio_pulldown_t pulldown_enable=GPIO_PULLDOWN_DISABLE,
 gpio_int_type_t interrupt_type=GPIO_INTR_DISABLE);

//получает значение из строки, если есть: val1=123&val2=456&val3=asd...
char *getTagValue(const char *a_tag_list, const char *a_tag, const char *delimiter = "&,;");
//вернёт значение,если найдёт
string getTagStringValue(string a_tag_list, string a_tag, string delimiter = "&,;");
//=========================================================================================================//
class fifo_buffer{
 private:
  bool dbg=false;
  int size=0;
  int freesz=0;
  char *buf=NULL;
  int r=0;//read pointer
  int w=0;//write pointer
  portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED; 

 public:
    fifo_buffer(int maxsize,bool debug=false){
      int64_t t=esp_timer_get_time();
      size=freesz=maxsize;dbg=debug;
      buf=(char*)malloc(maxsize);r=w=0;
      if(dbg)printf(" >>> [%lld μs] fifo: created buffer with size=%d bytes <<<\n",t,size);
    }

    ~fifo_buffer(){if(buf)free(buf);}

  //return inserting size
  int push(char* data, int len){
    portENTER_CRITICAL(&mux); 
    int64_t t=esp_timer_get_time();
    int l=len;int p=0; 
    if(l>freesz)l=freesz;
    int copyed=l;int fr=freesz;

    //copy
    while(l){
      if(w<r){ //будет скопировано не более свободного места, до r
        memcpy(&buf[w],&data[p],l);
        w+=l;freesz-=l;
        break;
      }
      else{ //w>=r
        int rr=l; 
        if(w+rr>size)rr=size-w;
        memcpy(&buf[w],&data[p],rr);
        p+=rr;
        w+=rr;if(w>=size)w=0;
        freesz-=rr;l-=rr;
        //далее копируем начало
      }
    }
    int fr2=freesz;
    portEXIT_CRITICAL(&mux);
    if(dbg)printf("+ [%lld μs] fifo: inserted %d bytes, free before=%d, after=%d\n",t,copyed,fr,fr2);
    return copyed;
  }
  //извлечь не более len,вернёт количество извлечённых
  int pop(char* data, int len){
    portENTER_CRITICAL(&mux); 
    int64_t t=esp_timer_get_time();
    int filled=size-freesz;
    int l=len;if(l>filled)l=filled;
    int copyed=l;
    int p=0; int fr=freesz;

    while(l){
      if(r<w){
        memcpy(&data[p],&buf[r],l);
        r+=l;freesz+=l;
        break;
      }
      else{//r>=w
        int rr=l; 
        if(r+rr>size)rr=size-r;
        memcpy(&data[p],&buf[r],rr);
        p+=rr;
        r+=rr;if(r>=size)r=0;
        freesz+=rr;l-=rr;
      }
    }
    int fr2=freesz;
    portEXIT_CRITICAL(&mux);
    if(dbg)printf("- [%lld μs] fifo: extracted %d bytes, free before=%d, after=%d\n",t,copyed,fr,fr2);
    return copyed;
  }
  //размер буфера
  int get_size(){ return size;}
  //свободное место
  int free_size(){
    portENTER_CRITICAL(&mux); 
    int64_t t=esp_timer_get_time();
    int fr=freesz;
    portEXIT_CRITICAL(&mux);
    
    if(dbg){
      static int b0f=0;
      if(b0f!=fr)printf(" ??? [%lld μs] fifo: free %d bytes\n",t,fr);
      b0f=fr;
    }
    return fr;
  }

  int filled(){return size-free_size();}
  int count (){return size-free_size();}
};
//____________________________________________________________________________________________________________//


#ifdef  __cplusplus
};
#endif

#endif // __FUNCTIONS_H