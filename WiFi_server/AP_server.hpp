
#ifndef _AP_SERVER
#define _AP_SERVER
//____________________________________________________________________________________________________________//
#include "functions.hpp"
#include "sdkconfig.h"
//#include <sys/param.h>
#include "esp_err.h"
#include "esp_log.h"
#include "esp_event.h"
#include "lwip/sockets.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_event_legacy.h"
#include <esp_wifi.h>
#include "esp_task_wdt.h"
#include <WiFiServer.hpp>
#include "spi_drv/spi_drv.hpp" //my driver
//#include <esp_task_wdt.h>
#include <cstring>

#include "lwip/sockets.h"

// #ifdef  __cplusplus
// extern "C" {
// #endif

#if !defined(EAI_OVERFLOW) 
#define EAI_OVERFLOW 208
#endif

#if !defined(NI_NUMERICHOST)
#define NI_NUMERICHOST 0x02
#endif

//____________________________________________________________________________________________________________//
//ERRORS
#define ERR_DATA_ZERO         1 //нули в потоке
#define ERR_FREE_NEGATIVE     2 //св.место отриц.
#define ERR_DATA_INCORRECT    4 //ошибка данных в потоке
#define ERR_SPI_ERROR    			8 //ошибка данных в потоке SPI

//-----------------DEBUG-----------------------
//#define DBG_UDP
//#define DBG_STREAM
//#define DBG_TEST_STREAM
//#define DBG_TCP_REQ
//#define DBG_SPI
//#define DBG_CHECK_STREAM 
//#define DBG_CHECK_SPI_STREAM
//#define DBG_SPI_SENDBUF
//---------------------------------------------
//флаги режимов 
#define MODE_FIFO 						1
#define MODE_FIXED_DMA				2
#define MODE_CSUMM						4
#define MODE_NO_ACK						8 //no send answer after receive data
#define MODE_STREAM 					16
#define MODE_UDP							32

#define DMA_BLOCK	1024
//____________________________________________________________________________________________________________//
#define  WIFI_SERVER_HOSTNAME    					"LSCAN_2"
#define  WIFI_SERVER_IP     							"192.168.0.13"
#define  WIFI_SERVER_GW     							"192.168.0.13"
#define  WIFI_SERVER_MASK     						"255.255.255.0"
#define  WIFI_SERVER_PORT   							1305
#define  WIFI_SERVER_UDP_PORT   					1313
#define  WIFI_SERVER_UDP_RESPONSE_PORT   	1315
#define  WIFI_MAX_CLIENTS   							4 
#define  WIFI_CHANNEL       							10 

#define  WIFI_PASS          "13051978"
#define  WIFI_AUTH_MODE     WIFI_AUTH_WPA_WPA2_PSK

#define  WIFI_SSID					"Redmi dem1305"
#define  WIFI_SSID_PASS			"Quantum_1305"
//-----------------------------------
extern WiFiServer* server;

esp_err_t WiFi_AP_start(bool restart_socket_if_error=true);
esp_err_t set_esp_interface_ip(esp_interface_t interface, IPAddress local_ip, IPAddress gateway, IPAddress subnet);

#define WCMD_FREE             1         //размер свободного места в приемном буфере 
#define WCMD_SAMPLESIZE       2         //минимальный размер пакета передачи
#define WCMD_STREAM_BLOCKSIZE 4         //минимальный размер пакета передачи DMA
#define WCMD_PING							8					//пинг
#define WCMD_BUFSIZE					16				//получить размер приемного буфера
#define WCMD_PREP_DMA					32				//подготовить DMA устройства к приёму
#define WCMD_STREAM	      		64				//переключить в режим потока
#define WCMD_STA_SSID         128       //установка STA SSID
#define WCMD_STA_PASS         129       //установка STA PASS
#define WCMD_STA_RECONNECT    130       //переподключение STA SSID
#define WCMD_STA_GET_PAR    	131       //запрос параметров IP STA SSID (строка) //{IP(32 bit) GATEWAY(32 bit) MASK(32 bit) PORT(16 bit)}
#define WCMD_STA_BRO_PAR    	132       //запрос параметров IP через broadcast

#pragma pack(1)
typedef struct 
{
	uint8_t cmd;    //команда 
	uint8_t discrMs;//дискретизация,точек в миллисекунду
	uint8_t lx ;    //младший полубайт X
	uint8_t ly;			//младший полубайт Y
	uint8_t hxhy;   //старшие полубайты XY
	uint8_t r;	    //1 байт RED,		0-255
	uint8_t g;	    //1 байт GREEN, 0-255
	uint8_t b;	    //1 байт BLUE,	0-255
}ICP;
#pragma pack()

#pragma pack(1)
typedef struct 
{
	char sta_ssid[32];//=WIFI_SSID;
	char sta_pass[64];//=WIFI_SSID_PASS;
	char ap_ssid[32];//=WIFI_SERVER_HOSTNAME;
	char ap_pass[64];//=WIFI_PASS;
	char server_ip[32];//=WIFI_SERVER_IP;
	uint32_t port;//=WIFI_SERVER_PORT;
}dev_data;
#pragma pack()

//аргумент может быть длиной до 24 бит включительно
uint32_t sendCmd(uint8_t cmd,uint32_t arg24bit=0);
int getBufferSize();
int getFreeSize();
int getStreamBlockSize();
//вернёт freeAfter предположительное оставшееся свободное место после отправки (будет увеличиваться)
esp_err_t prepareDeviceDMA(int& len, int* freeAfter=NULL);
//переслать большой буфер.Устройство вернёт свободное место в spiDeviceFreeBufSize
esp_err_t send_spi_buf(char* buf,int buflen,int& spiDeviceReturnFreeSize);
//____________________________________________________________________________________________________________//

// #ifdef  __cplusplus
// }
// #endif

#endif //_AP_SERVER