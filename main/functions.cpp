
#include "functions.hpp"

#define NOP() asm volatile ("nop")

using namespace std;
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

string get_chip_id(bool echoMAC/*=false*/) {
  char temp[32]={0};
  uint64_t chipid = getEfuseMac(); //The chip ID is essentially its MAC address(length: 6 bytes).
  esp_chip_info_t chip_info;
  esp_chip_info(&chip_info);
  if(echoMAC)printf("This is ESP32 chip with %d CPU cores, WiFi%s%s, ",
            chip_info.cores,
            (chip_info.features & CHIP_FEATURE_BT) ? "/BT" : "",
            (chip_info.features & CHIP_FEATURE_BLE) ? "/BLE" : "");
  sprintf(temp,"%04X", (uint16_t)(chipid >> 32)); //print High 2 bytes
  sprintf(&temp[4],"%08X", (uint32_t)chipid); //print Low 4bytes.
  if(echoMAC)printf(" MAC: %08X\n", (uint32_t)chipid); //print 6 bytes.
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
string ip32_to_string(uint32_t ip)
{
  union
  {
    uint32_t raw;
    uint8_t octet[4];
  } _ip;
  _ip.raw = ip;
  string s = to_string(_ip.octet[0]) + "." + to_string(_ip.octet[1]) + "." + to_string(_ip.octet[2]) + "." + to_string(_ip.octet[3]);
  // char s[64];
  // sprintf("%hhu.%hhu.%hhu.%hhu", s, _ip.octet[0], _ip.octet[1],_ip.octet[2], _ip.octet[3]);
  return s; // string(s);
}
string ip4_to_string(esp_ip4_addr_t &ip4) { return ip32_to_string(ip4.addr); }
//____________________________________________________________________________________________________________//
//get port & addr from sockaddr
uint16_t get_sockaddr_port_addr(struct sockaddr *s, char* _ipdest[INET_ADDRSTRLEN])
{
 struct sockaddr_in *sin = (struct sockaddr_in *)s;
 char ip[INET_ADDRSTRLEN]; //IP4ADDR_STRLEN_MAX==16
 uint16_t port;
 //преобразует адрес сети Интернет IPv4 или IPv6 в стандартной текстовой форме представления в числовую двоичную форму. 
 inet_pton (AF_INET,(char*)&(sin->sin_addr), ip);//, sizeof (ip));
 /*
 Порядок байтов Intel, называемый прямым порядком байтов, также является обратным по отношению к сетевому стандарту с обратным порядковым порядком байтов. 
 Big-Endian	Наиболее значимый байт находится в левом конце слова.
 Little-Endian	Наиболее значимый байт находится в правом конце слова.
 */
 port = htons (sin->sin_port);//Функция htons преобразует u_short из узла в порядок байтов сети TCP/IP (это большой байт).
 //printf ("host %s:%d\n", ip, port);
 if(_ipdest)memcpy(*_ipdest,ip,INET_ADDRSTRLEN);
 return port;
}
//=========================================================================================================//
//получает значение из строки, если есть: val1=123&val2=456&val3=asd...
char *getTagValue(const char *a_tag_list, const char *a_tag, const char *delimiter /* = "& ,;" */)
{
  if (a_tag_list == NULL)return NULL;
  /* 'strtok' modifies the string. */
  char *tag_list_copy = (char *)malloc(strlen(a_tag_list) + 1);if (tag_list_copy == NULL)return NULL;
  char *result = NULL;
  char *s;

  strcpy(tag_list_copy, a_tag_list); // original to copy

  s = strtok(tag_list_copy, delimiter); // Use delimiter "&"
  while (s)
  {
    char *equals_sign = strchr(s, '=');
    if (equals_sign)
    {
      *equals_sign = 0;
      // Use string compare to find required tag
      if (0 == strcmp(s, a_tag))
      {
        equals_sign++;
        result = (char *)malloc(strlen(equals_sign) + 1);
        strcpy(result, equals_sign);
      }
    }
    s = strtok(0, delimiter);
  }
  free(tag_list_copy);
  return result;
}
//вернёт значение,если найдёт
string getTagStringValue(string a_tag_list, string a_tag, string delimiter /* = "& ,;" */) {
	char* r = getTagValue(a_tag_list.c_str(), a_tag.c_str(), delimiter.c_str()); string rr = r?r:"undefined"; if(r)free(r); return rr;
}
//=========================================================================================================//
