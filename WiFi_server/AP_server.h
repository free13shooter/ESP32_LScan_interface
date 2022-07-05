
#ifndef _AP_SERVER
#define _AP_SERVER
//____________________________________________________________________________________________________________//
#include "functions.h" 
#include "sdkconfig.h"
#include <sys/param.h>
#include "esp_err.h"
#include "esp_log.h"
#include "esp_event.h"
#include "lwip/sockets.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_event_legacy.h"
#include <esp_wifi.h>
#include "esp_task_wdt.h"
#include <WiFiServer.h>
#include "spi_drv/spi_drv.h" //my driver

// #ifdef  __cplusplus
// extern "C" {
// #endif
//____________________________________________________________________________________________________________//
#define  WIFI_SERVER_IP     "192.168.0.130"
#define  WIFI_SERVER_PORT   1305
#define  WIFI_MAX_CLIENTS   4 
#define  WIFI_CHANNEL       6 

#define  WIFI_PASS          "13051978"
#define  WIFI_AUTH_MODE     WIFI_AUTH_WPA_WPA2_PSK
//-----------------------------------
extern WiFiServer* server;
esp_err_t WiFi_AP_start();
esp_err_t set_esp_interface_ip(esp_interface_t interface, IPAddress local_ip, IPAddress gateway, IPAddress subnet);
//____________________________________________________________________________________________________________//

// #ifdef  __cplusplus
// }
// #endif

#endif //_AP_SERVER