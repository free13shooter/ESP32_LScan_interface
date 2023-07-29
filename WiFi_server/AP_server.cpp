#include "AP_server.hpp"

//---------------------------------------------------------------------------------------------------------//
static const int CONNECTED_BIT = BIT0;
static const int DISCONNECTED_BIT = BIT1;
static const int SMARTCONFIG_DONE_BIT = BIT2;
static const int SMARTCONFIG_ERROR_BIT = BIT3;


volatile uint32_t errors=0;

extern spi_bus_config_t buscfg;//for maxtransfer
//------------------------------------------------------
//struct fifobuf{
  int device_SPI_buf_size=0;
  int TCP_FIFO_buf_size=0;
  volatile int StreamBufFree=0;
  char *streamBuffer=NULL;
  volatile char* prd=NULL;volatile char* pwr=NULL;
  char* spibuf_out=NULL;
  portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED; 
  volatile bool stop_spi=false;
  char* transbuf=NULL;//transfer buffer

  EventGroupHandle_t xEventGroupFIFO;
//};

//fifobuf* spi_buffer;

WiFiServer *server = NULL;
IPAddress server_ip(0, 0, 0, 0);
string *ssid;


static const char *TAG = "LScan";

dev_data dev;

wifi_config_t ap_config;
wifi_config_t sta_config;

ip_event_got_ip_t *sta_got_ip = NULL; //информация STA
uint32_t broadcast_addr = 0;
bool disable_sta = false;
int socket_udp = -1;
int socket_stream = -1;
volatile bool restart_stream_socket=false;
volatile bool reinit_stream_buffer=true;

static esp_netif_t *esp_netifs[ESP_IF_MAX] = {NULL, NULL, NULL};
static esp_netif_t *sta_netif = NULL; //------------

// FreeRTOS event group to signal when we are connected & ready to make a request
static EventGroupHandle_t wifi_event_group;

esp_event_handler_instance_t instance_any_id;
esp_event_handler_instance_t instance_got_ip;
esp_event_handler_instance_t sta_instance_got_ip;

const int IPV4_GOTIP_BIT = BIT0;

static void stream_task(void *pvParameters);      //TCP/UDP server
static void udp_server_task(void *pvParameters);  //UDP server
static void fifo_task(void *pvParameters);        //spi transfering
TaskHandle_t spiTask=NULL;
volatile int64_t client_connection_time=0;
volatile int64_t stream_client_connected=0;

volatile int stream_mode=0;
uint8_t* stream_block=NULL;
int stream_block_size=0;

bool save_dev_flash();
bool read_dev_flash();

bool direct_connected = false;
uint8_t assigned_mac[6];

//=========================================================================================================//
//проверяет поток на валидность
void check_stream_buf(char* buf,int buflen,int& free_before){
  //=============ПРОВЕРКА НА ОШИБКИ ДАННЫХ=============
  for(int i=0;i<buflen;i+=8){
    ICP* d=(ICP*)&buf[i];
    if(d->cmd || d->discrMs==(uint8_t)0){
      errors|=ERR_DATA_INCORRECT;
      printf("!!! error in data[i=%d/b=%d], len=%d: cmd=%d, discr=%d\n",i/8,i,buflen,(int)d->cmd,(int)d->discrMs);
      char dd[9];memcpy(dd,d,8);dd[8]=0;
      printf("data: \"%s\" [ %d %d %d %d %d %d %d %d ]\n",dd,(int)dd[0],(int)dd[1],(int)dd[2],(int)dd[3],(int)dd[4],(int)dd[5],(int)dd[6],(int)dd[7]);
          
      if(prd<=pwr) printf("buf: [0x%08X   rd=0x%08X    wr=0x%08X   ]0x%08X ,len=%d, free=%d\n",(uint32_t)streamBuffer,(uint32_t)prd,(uint32_t)pwr,(uint32_t)spibuf_out,device_SPI_buf_size,free_before);
        else printf("buf: [0x%08X   wr=0x%08X    rd=0x%08X   ]0x%08X ,len=%d, free=%d\n",(uint32_t)streamBuffer,(uint32_t)pwr,(uint32_t)prd,(uint32_t)spibuf_out,device_SPI_buf_size,free_before);
      while(1)vTaskDelay(1000);
    }
  }
}
//=========================================================================================================//
//softAP: get STA mac from IP
bool ap_get_sta_mac_from_ip(ip4_addr_t _ip, uint8_t* retMac){
  wifi_sta_list_t stationList;
 
  esp_wifi_ap_get_sta_list(&stationList);  
 
  printf("AP: connected stations=%d\n",stationList.num);
 
  for(int i = 0; i < stationList.num; i++) {
 
    wifi_sta_info_t station = stationList.sta[i];
    ip4_addr_t sip;

    if(!dhcp_search_ip_on_mac(station.mac,&sip) || sip.addr!=_ip.addr)continue;
    memcpy(retMac,&station.mac,6);
    printf("AP: found ip=%s, assigned to mac %s\n",ip32_to_string(sip.addr).c_str(),mac_toString(station.mac).c_str());
    return true;
  }
  return false;
}
//=========================================================================================================//
esp_netif_t *get_esp_interface_netif(esp_interface_t interface)
{
  if (interface < ESP_IF_MAX)
  {
    return esp_netifs[interface];
  }
  return NULL;
}
//=========================================================================================================//
int create_socket(uint32_t ip, uint16_t port, int socketType, int protocol, char *return_addr_str/*[INET_ADDRSTRLEN]*/, sockaddr_in *return_sockaddr = NULL)
{
  sockaddr_in destAddr;
  sockaddr_in *pdestAddr = return_sockaddr ? return_sockaddr : &destAddr;
  pdestAddr->sin_addr.s_addr = htonl(ip); // Change hostname to network byte order
  pdestAddr->sin_family = AF_INET;        // Define address family as Ipv4
  pdestAddr->sin_port = htons(port);      // htons(PORT); 	//Define PORT
  int addr_family = AF_INET;              // Define address family as Ipv4 int addr_family Ipv4 address protocol variable
  //преобразует Интернет-адрес IPv4 в строку (например, 127.0.0.1)
  if(return_addr_str){
    inet_ntoa_r(pdestAddr->sin_addr, return_addr_str, INET_ADDRSTRLEN);
  }
  /* Create socket*/
  int type = socketType==-1?(protocol == IPPROTO_UDP ? SOCK_DGRAM : SOCK_STREAM):socketType;
  int id = socket(addr_family, type, protocol);
  if (id < 0){ printf("Unable to create socket: errno %d\n", errno);delay(1000);return id;}
  string s=to_string((int)protocol);
  if (protocol == IPPROTO_TCP) s = "TCP";
  else if (protocol == IPPROTO_UDP) s = "UDP";
  else if (protocol == IPPROTO_ICMP) s = "ICMP";
  else if (protocol == IPPROTO_IP) s = "IP";

  string sty=to_string((int)type);
  if (type == SOCK_DGRAM)sty= "SOCK_DGRAM";
  else if (type == SOCK_STREAM)sty = "SOCK_STREAM";
  else if (type == SOCK_RAW)sty = "SOCK_RAW";

  printf("Socket %d (protocol %s) created, type=%s, ip: %s:%d\n", id, s.c_str(),sty.c_str(), ip32_to_string(ip).c_str(), (int)port);
  return id;
}

int create_socket(const char* ip, uint16_t port, int type,int protocol,sockaddr_in *return_sockaddr = NULL){
  struct sockaddr_in sa;
  //char str[INET_ADDRSTRLEN];
  printf("create socket with ip=%s\n",ip);
  //преобразует строку символов в сетевой адрес (типа af AF_INET/AF_INET6),копирует полученную структуру с адресом в dst.
  inet_pton(AF_INET, ip, &(sa.sin_addr));
  return create_socket(sa.sin_addr.s_addr, port, type, protocol, NULL, return_sockaddr);
}
int create_socket(const char* ip, uint16_t port,int protocol,sockaddr_in *return_sockaddr = NULL){
  return create_socket(ip,port,-1,protocol,return_sockaddr);
}
//=========================================================================================================//
bool disconnect_this(uint8_t tmac[6])
{
  uint16_t aid = 0;
  esp_err_t r = esp_wifi_ap_get_sta_aid(tmac, &aid);
  if (ESP_OK != r)
  {
    ESP_LOGE(TAG, "deauth_sta err=%d", r);
    return false;
  }

  r = esp_wifi_deauth_sta(aid);
  if (ESP_OK != r)
  {
    ESP_LOGE(TAG, "deauth_sta err=%d", r);
    return false;
  }

  return true;
}
//---------------------------------------------------------------------------------------------------------//
static bool _ac = false;
static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
  esp_err_t err;
  // If esp_wifi_start() returned ESP_OK and WiFi mode is in station mode
  if (/*event_base == WIFI_EVENT && */ event_id == WIFI_EVENT_AP_START)
  {
    const char *name;
    const char *g_hostname = WIFI_SERVER_HOSTNAME;
    // Set the hostname for the default TCP/IP station interface
    if ((err = tcpip_adapter_set_hostname(TCPIP_ADAPTER_IF_AP, g_hostname)) != ESP_OK)
    {
      ESP_LOGE(TAG, "Err Set Hostname: %s", esp_err_to_name(err));
    }
    else
    {
      if ((err = tcpip_adapter_get_hostname(TCPIP_ADAPTER_IF_AP, &name)) != ESP_OK)
      {
        ESP_LOGE(TAG, "Err Get Hostname: %s", esp_err_to_name(err));
      }
      else
      {
        ESP_LOGI(TAG, "Hostname: %s", (name == NULL ? "<None>" : name));
      }
    }
  }
  //-------------------------------------------
  if (event_id == WIFI_EVENT_AP_STACONNECTED)
  {
    wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *)event_data;
    if (direct_connected)
    {
      disconnect_this(event->mac);
      // ESP_LOGW(TAG, "disconnected " MACSTR, MAC2STR(event->mac));
      printf("disconnected " MACSTR "\n", MAC2STR(event->mac));
      return;
    }

    memcpy(assigned_mac, event->mac, 6);

    // ESP_LOGW(TAG, "station " MACSTR " join, AID=%d", MAC2STR(event->mac), event->aid);
    printf("+ station " MACSTR " join, AID=%d \n", MAC2STR(event->mac), event->aid);
    xEventGroupSetBits(wifi_event_group, IPV4_GOTIP_BIT);
  }
  else if (event_id == WIFI_EVENT_AP_STADISCONNECTED)
  {
    wifi_event_ap_stadisconnected_t *event = (wifi_event_ap_stadisconnected_t *)event_data;
    // ESP_LOGW(TAG, "station " MACSTR " leave, AID=%d", MAC2STR(event->mac), event->aid);
    //  On disconnet, close TCP socket client
    if (assigned_mac[0] == event->mac[0] &&
        assigned_mac[1] == event->mac[1] &&
        assigned_mac[2] == event->mac[2] &&
        assigned_mac[3] == event->mac[3] &&
        assigned_mac[4] == event->mac[4] &&
        assigned_mac[5] == event->mac[5])
    {
      // printf("DISCONNECTED :" MACSTR, MAC2STR(event->mac));
      direct_connected = false;
    }
    printf("- station " MACSTR " leave, AID=%d, direct_connected=%s\n", MAC2STR(event->mac), event->aid,(direct_connected?"TRUE":"FALSE"));
  }
  else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
  {
    xEventGroupClearBits(wifi_event_group, CONNECTED_BIT);
    if (sta_got_ip)
    {
      bzero(sta_got_ip, sizeof(ip_event_got_ip_t));
      sta_got_ip = NULL;
    }
    broadcast_addr = 0;
    if (!_ac)
      printf("WIFI_EVENT_STA_DISCONNECTED\n");
    if (!disable_sta && string(dev.sta_ssid) != "")
    {
      memset(sta_config.sta.ssid, 0, 32);
      memset(sta_config.sta.password, 0, 64);
      strcpy((char *)sta_config.sta.ssid, dev.sta_ssid);
      strcpy((char *)sta_config.sta.password, dev.sta_pass);
      if (!_ac)
        printf("ATTEMPT CONNECT to: ssid=%s password=%s\n", (char *)sta_config.sta.ssid, (char *)sta_config.sta.password);
      esp_wifi_connect();
    }
    _ac = true;
  }
  //else printf("UNPROCESSED wifi_event_handler %d\n",(int)event_id);
}
//---------------------------------------------------------------------------------------------------------//
static void IP_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
  if (event_base == IP_EVENT && event_id == IP_EVENT_AP_STAIPASSIGNED)//direct connect
  {
    ip_event_ap_staipassigned_t *event = (ip_event_ap_staipassigned_t *)event_data;
    if (direct_connected)
    {
      uint8_t m[6];ip4_addr_t _ip;memcpy(&_ip,&event->ip,4);
      if(ap_get_sta_mac_from_ip(_ip, m)){
        disconnect_this(m);
        printf("IP_EVENT_AP_STAIPASSIGNED: already connected! -> disconnect " MACSTR "\n", MAC2STR(m));
      }
      else printf("IP_EVENT_AP_STAIPASSIGNED: already connected! -> ERROR-mac not found for ip %s\n",ip4_to_string(event->ip).c_str());
      return;
    }

    direct_connected = true;
    // ESP_LOGW(TAG, "ASSIGNED IP:" IPSTR, IP2STR(&event->ip));
    printf("ASSIGNED IP:" IPSTR, IP2STR(&event->ip)); printf(" (direct connection to AP)\n");
  }
  else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
  {
    _ac = false;
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    if (sta_got_ip)bzero(sta_got_ip, sizeof(ip_event_got_ip_t));
    if (socket_udp > 0)
    {
      shutdown(socket_udp, 0);
      close(socket_udp);
    }
    // if (socket_stream != -1)
    // {
    //   restart_stream_socket=true;
    //   shutdown(socket_stream, 0);
    //   close(socket_stream);
    // }
    sta_got_ip = new ip_event_got_ip_t; //новая структура
    memcpy(sta_got_ip, event, sizeof(ip_event_got_ip_t));
    broadcast_addr = (sta_got_ip->ip_info.ip.addr | (~sta_got_ip->ip_info.netmask.addr));
    xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);
    printf("  > New WiFi connection: SSID \"%s\", ip: " IPSTR " <\n", sta_config.sta.ssid, IP2STR(&event->ip_info.ip));//, ip32_to_string(broadcast_addr).c_str());
    
    //xTaskCreate(udp_server_task, "udp_sta_server", 8092, (void *)sta_got_ip, 5, NULL); // UDP
  }
  else
    printf("OTHER IP event: %d\n", event_id);
}
//=========================================================================================================//
bool is_sta_connected(int timeout_ms = 2000)
{
  int bits = xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT, pdFALSE, pdTRUE, timeout_ms / portTICK_PERIOD_MS);
  // ESP_LOGI(TAG, "bits=%x", bits);
  if ((bits & CONNECTED_BIT) != 0)
  {
    if (sta_netif)
      printf(" +++ STA CONNECTED: SSID:%s password:%s +++\n", sta_config.sta.ssid, sta_config.sta.password);
  }
  else
  {
    if (sta_netif)
      printf(" --- STA not connected. SSID:%s password:%s ---\n", sta_config.sta.ssid, sta_config.sta.password);
  }
  return (bits & CONNECTED_BIT) != 0;
}

bool sta_connect(int timeout_ms = 2000)
{
  // station
  memset(&sta_config, 0, sizeof(wifi_config_t));
  strcpy((char *)sta_config.sta.ssid, dev.sta_ssid);
  strcpy((char *)sta_config.sta.password, dev.sta_pass);
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
  disable_sta = false;
  ESP_ERROR_CHECK(esp_wifi_connect());
  return is_sta_connected(timeout_ms);
}
//=========================================================================================================//
esp_err_t wifi_init_softAPSTA(void)
{
  memset((void *)&dev, 0, sizeof(dev_data));
  // Initialize NVS
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
  {
    // NVS partition was truncated and needs to be erased
    // Retry nvs_flash_init
    ESP_ERROR_CHECK(nvs_flash_erase());
    err = nvs_flash_init();
  }
  ESP_ERROR_CHECK(err); 

  if (!read_dev_flash())
  {
    printf("NVS initialization save ... ");
    strcpy(dev.sta_ssid, WIFI_SSID);
    strcpy(dev.sta_pass, WIFI_SSID_PASS);
    strcpy(dev.ap_ssid, WIFI_SERVER_HOSTNAME);
    strcpy(dev.ap_pass, WIFI_PASS);
    strcpy(dev.server_ip, WIFI_SERVER_IP);
    dev.port = WIFI_SERVER_PORT;
    save_dev_flash();
  }

  static uint8_t chan = 1;
  esp_wifi_disconnect();
  esp_wifi_stop();
  //---------------------------------------
  if (!server_ip.fromString(WIFI_SERVER_IP))
  {
    ESP_LOGE(TAG, "ERROR IP::fromString(%s), init failed.\n", WIFI_SERVER_IP);
    return ESP_ERR_ESP_NETIF_INIT_FAILED;
  }
  wifi_event_group = xEventGroupCreate(); // Create listener thread
  ESP_ERROR_CHECK(esp_netif_init());      // Initialize the underlying TCP/IP lwIP stack
                                          // -- ESP_ERROR_CHECK(esp_event_loop_init(event_handler, NULL) ); //Start event_handler loop
  ESP_ERROR_CHECK(esp_event_loop_create_default());

  esp_netif_t *wifiAP = esp_netifs[WIFI_IF_AP] = esp_netif_create_default_wifi_ap();
  esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
  assert(sta_netif);

  esp_netif_ip_info_t ipInfo;
  IP4_ADDR(&ipInfo.ip, server_ip[0], server_ip[1], server_ip[2], server_ip[3]);
  IP4_ADDR(&ipInfo.gw, 0, 0, 0, 0); // не рекламировать как маршрутизатор-шлюз
  IP4_ADDR(&ipInfo.netmask, 255, 255, 255, 0);
  esp_netif_dhcps_stop(wifiAP);
  esp_netif_set_ip_info(wifiAP, &ipInfo);
  
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));
  //---------------------
  ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &instance_any_id));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_AP_STAIPASSIGNED, &IP_event_handler, NULL, &instance_got_ip));

  ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &wifi_event_handler, NULL)); // STA
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &IP_event_handler, NULL));

  ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_NULL));

  memset(&ap_config, 0, sizeof(wifi_config_t));
  ap_config.ap.channel = chan; // WIFI_CHANNEL;
  if (++chan > 13)
    chan = 1;
  // station
  memset(&sta_config, 0, sizeof(wifi_config_t));
  // sta_config.sta.channel = chan;
  strcpy((char *)sta_config.sta.ssid, dev.sta_ssid);
  strcpy((char *)sta_config.sta.password, dev.sta_pass);
  // AP
  ap_config.ap.max_connection = WIFI_MAX_CLIENTS;
  ap_config.ap.authmode = WIFI_AUTH_MODE;

  sprintf((char *)ap_config.ap.ssid, (string("LASER_SCAN_2_") + get_chip_id()).c_str());
  sprintf((char *)ap_config.ap.password, WIFI_PASS);

  ap_config.ap.ssid_len = (uint8_t)strlen((char *)ap_config.ap.ssid);

  if (ap_config.ap.ssid_len == 0)
    ap_config.ap.authmode = WIFI_AUTH_OPEN;

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
  ESP_ERROR_CHECK(esp_wifi_start());

  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
  sta_connect();
  //==========
  if (esp_wifi_set_ps(WIFI_PS_NONE) != ESP_OK) // set sleep false
  {
    ESP_LOGE(TAG, "esp_wifi_set_ps WIFI_PS_NONE failed!");
  }

  esp_netif_ip_info_t ip;
  printf("*********************************************************************\n");
  printf("AP: \"%s\" passwd: \"%s\"", (char *)ap_config.ap.ssid,(char *)ap_config.ap.password);
  if (esp_netif_get_ip_info(get_esp_interface_netif(ESP_IF_WIFI_AP), &ip) != ESP_OK) printf(" Netif Get IP Failed!\n");
  else {IPAddress myIP = ip.ip.addr;printf(" , ip: %d.%d.%d.%d\n", (int)myIP[0], (int)myIP[1], (int)myIP[2], (int)myIP[3]);}
  printf("STA: set connection to SSID: \"%s\" , passwd: \"%s\"\n", dev.sta_ssid, dev.sta_pass);
  printf("*********************************************************************\n");

  esp_netif_dhcps_start(wifiAP);
  //==========
  return ESP_OK;
}
//=========================================================================================================//
esp_err_t WiFi_AP_start(bool restart_socket_if_error)
{
  // esp_netif_destroy_default_wifi();
  // Initialize NVS
  esp_err_t e = nvs_flash_init();
  if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND)
  {
    ESP_ERROR_CHECK(nvs_flash_erase());
    e = nvs_flash_init();
  }
  ESP_ERROR_CHECK(e);

  bool init = true;

  while (1)
  {
    if (init || errno == 112 || errno == 23)
    {
      if (!init)
      {
        esp_netif_dhcps_stop(esp_netifs[WIFI_IF_AP]);
        esp_wifi_stop();
        esp_wifi_set_mode(WIFI_MODE_STA);
        esp_wifi_disconnect();
        esp_wifi_deinit();
        esp_event_loop_delete_default();
        esp_netif_destroy_default_wifi(esp_netifs[WIFI_IF_AP]);
        esp_netifs[WIFI_IF_AP] = NULL;
        vTaskDelay(100 / portTICK_PERIOD_MS);
      }
      // gpio_set_level( (gpio_num_t)SPI_SOFT_CS, 1 );
      // vTaskDelay(100 / portTICK_PERIOD_MS);
      // gpio_set_level( (gpio_num_t)SPI_SOFT_CS, 0 );
      e = wifi_init_softAPSTA(); // first start or EHOSTDOWN      112: Host is down, 23: Too many open files in system
    }
    if (e)
    {
      ESP_LOGE(TAG, "FAILED wifi_init_softAPSTA,error = %d", e);
      return e;
    }

    //stream_mode|=MODE_STREAM|MODE_UDP;

    if(init){
      printf("FREE HEAP AVAILABLE: %.1f KB\n",(double)esp_get_free_heap_size()/1024);
      xTaskCreate(udp_server_task, "udp_sta_server", 4096, NULL, 5, NULL); // UDP
      init = false;
    }
    
    // xTaskCreate(stream_task, "tcp_server", 4096, NULL, 5, NULL);
    // xTaskCreate(WiFiServer_task, "tcp_server", 4096, NULL, 5, NULL);
    // WiFiServer_task(NULL);
    
    stream_task(NULL);
    
    if(!restart_socket_if_error)break;
    //vTaskDelay(100 / portTICK_PERIOD_MS);
  }
  return ESP_OK;
}
//--------check if for need delimiter------------------------------------------------------------------
static void cdelim(char* buf,const char* delimiter="&"){
  if (strlen(buf) != 0)sprintf(&buf[strlen(buf)], delimiter);
}
//======================== UDP SOCKET ===============================================================//
static void udp_server_task(void *pvParameters)
{
  //static TickType_t time_start = xTaskGetTickCount();
  
  while(1){
  ip_event_got_ip_t *info = (ip_event_got_ip_t *)pvParameters;
  int sock = 0;  bool reconnect = false;
  char *device = (char *)ap_config.ap.ssid;

  char addr_str[128]; // char array to store client IP
  //int bytes_received; // immediate bytes received

  int rxsize = 1472; int txsize = 1472; //+1 term
  char *rxbuf = (char *)malloc(rxsize + 1);char *txbuf = (char *)malloc(txsize + 1);
  int er = 0;int dobreak=0;//1 - break, 2-reboot

  sockaddr_in destAddr; socket_udp = sock = create_socket(INADDR_ANY, WIFI_SERVER_UDP_PORT,SOCK_DGRAM,IPPROTO_UDP, addr_str, &destAddr);

  sockaddr_in addr_broad_response;
  addr_broad_response.sin_family = AF_INET; addr_broad_response.sin_port = htons(WIFI_SERVER_UDP_RESPONSE_PORT); addr_broad_response.sin_addr.s_addr =INADDR_BROADCAST;

  int flag = 1;int sr=0;
  if (0 != setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag)) || 0!=(sr=setsockopt(sock, SOL_SOCKET, SO_BROADCAST,&flag,sizeof(flag))))
  {
    printf("FAILED : %s, err=%d",sr!=0?"SO_BROADCAST":"SO_REUSEADDR", errno);break;
  }
  
  //Привязка: Bind a socket to a specific IP + port
  if (bind(sock, (struct sockaddr *)&destAddr, sizeof(destAddr)) != 0) {printf("UDP Socket unable to bind: errno %d", errno);esp_restart();}
          
  //printf("********************** UDP Socket binded ****************************\n");
  //=======================================================>>
  if (false == (pvParameters == NULL || info->ip_info.ip.addr != (uint32_t)0)) printf("UDP EXIT\n");
  int e9=0;

  while (pvParameters == NULL || info->ip_info.ip.addr != (uint32_t)0) //пока нет параметров или есть ip
  {
    bool rstseq=false;int new_mode=stream_mode;

    struct sockaddr_storage source_addr; // Large enough for both IPv4 or IPv6
    socklen_t socklen = sizeof(source_addr);
    int len = recvfrom(sock, rxbuf, rxsize - 1, 0, (struct sockaddr *)&source_addr, &socklen);
    
    // Error occurred during receiving
    if (len < 0)
    {
      er=errno;
      if(!e9)printf("recvfrom failed: errno %d\n", er);
      if(er==9)e9=1;
      break;
    }
    // Data received
    else
    {
      e9=0;
      // Get the sender's ip address as string
      if (source_addr.ss_family == PF_INET)
      {
        inet_ntoa_r(((struct sockaddr_in *)&source_addr)->sin_addr, addr_str, sizeof(addr_str) - 1);
      }
      else if (source_addr.ss_family == PF_INET6)
      {
        // inet6_ntoa_r(((struct sockaddr_in6 *)&source_addr)->sin6_addr, addr_str, sizeof(addr_str) - 1);
        #ifdef DBG_UDP
        printf("source_addr.ss_family == PF_INET6 \n");
        #endif
      }

      rxbuf[len] = 0; // Null-terminate whatever we received and treat like a string...
      //#ifdef DBG_UDP
      //printf("udp: Received %d bytes from %s:", len, addr_str);printf("%s\n", rxbuf);
      //#endif

      memset(txbuf, 0, txsize);
      dobreak=0;//1 - break, 2-reboot
      char* pdev=NULL;bool dv=false;string so="";bool rdev=false;

      if (strstr(rxbuf, WIFI_SERVER_HOSTNAME) || (pdev=strstr(rxbuf, device))){//prefix LSCAN_2 or device name 

        if (strstr(rxbuf, "get_name") || (pdev && strstr(rxbuf, "sta_get_ip")))
        {
          if (strlen(txbuf) != 0){sprintf(&txbuf[strlen(txbuf)], "&");}sprintf(&txbuf[strlen(txbuf)], "device=%s&",device);dv=true;
          
          if (sta_got_ip == NULL || direct_connected) { 
            //sprintf(&txbuf[strlen(txbuf)], "ip=%s&gateway=%s&mask=%s&apip=%s", ip32_to_string(0).c_str(),ip32_to_string(0).c_str(), ip32_to_string(0).c_str(),WIFI_SERVER_IP);
            sprintf(&txbuf[strlen(txbuf)], "ip=%s&gateway=%s&mask=%s&apip=%s",WIFI_SERVER_IP,WIFI_SERVER_GW,WIFI_SERVER_MASK,WIFI_SERVER_IP);
          }
          else {
            sprintf(&txbuf[strlen(txbuf)], "ip=%s&gateway=%s&mask=%s&apip=%s",ip4_to_string(sta_got_ip->ip_info.ip).c_str(),
                    ip4_to_string(sta_got_ip->ip_info.gw).c_str(), ip4_to_string(sta_got_ip->ip_info.netmask).c_str(),WIFI_SERVER_IP);
          }

          if(pdev)sprintf(&txbuf[strlen(txbuf)], "&sta_ssid=%s&sta_pass=%s",  dev.sta_ssid, dev.sta_pass);
            else { uint64_t chipid = getEfuseMac(); uint32_t d = (uint32_t)(chipid&0xFF);delay(d); }
        }
        
        //device------------------>>
        if (pdev){
          if(!client_connection_time && (stream_mode&MODE_UDP))client_connection_time=esp_timer_get_time();
          if(!dv){if (strlen(txbuf) != 0){sprintf(&txbuf[strlen(txbuf)], "&");} sprintf(&txbuf[strlen(txbuf)], "device=%s",device);}
          // if (strstr(rxbuf, "ping")) {
          //   if (strlen(txbuf) != 0)sprintf(&txbuf[strlen(txbuf)], "&");
          //   if (sta_got_ip == NULL)  sprintf(&txbuf[strlen(txbuf)], "ip=%s",ip32_to_string(0).c_str());
          //     else  sprintf(&txbuf[strlen(txbuf)], "ip=%s", ip32_to_string(sta_got_ip->ip_info.ip).c_str());
          // }
          if (strstr(rxbuf, "get_sta_ssid")) {
            cdelim(txbuf);
            sprintf(txbuf, "sta_ssid=%s&sta_pass=%s", dev.sta_ssid, dev.sta_pass);
          }
          if (strstr(rxbuf, "sta_ssid=")) {
            char *ssid = getTagValue(rxbuf, "sta_ssid");
            if(ssid)strcpy(dev.sta_ssid , ssid);
            cdelim(txbuf);
            sprintf(&txbuf[strlen(txbuf)], ssid); free(ssid);
            so+=string("NEW STA SSID=")+dev.sta_ssid+" ";
          }
          if (strstr(rxbuf, "sta_password=")) {
            char *pass = getTagValue(rxbuf, "sta_password");
            if(pass)strcpy(dev.sta_pass , pass);
            cdelim(txbuf);
            sprintf(&txbuf[strlen(txbuf)], pass); free(pass);
            so+=string("NEW STA PASS=")+dev.sta_pass+" ";
          }
          if (strstr(rxbuf, "save_devdata")) {
            save_dev_flash();
            cdelim(txbuf);
            sprintf(&txbuf[strlen(txbuf)], "save_devdata=ok"); 
             so+="-> save ";
          }
          if(strstr(rxbuf, "reset_device")!=NULL){
            cdelim(txbuf);
            sprintf(&txbuf[strlen(txbuf)],"reset_device=ok");//if connected
            rdev=true;
            so+="-> reset device ";
          }
          if (strstr(rxbuf, "reconnect")) {
            reconnect = true;
            cdelim(txbuf);
            sprintf(&txbuf[strlen(txbuf)], "reconnect=ok"); dobreak=1;
            so+="-> reconnect ";
          }
          if (strstr(rxbuf, "reboot") || strstr(rxbuf, "restart")) {
            cdelim(txbuf);
            sprintf(&txbuf[strlen(txbuf)], "reboot=ok"); dobreak=2;
            so+="-> reboot ";
          }
          if(strstr(rxbuf, "ping")!=NULL){
            cdelim(txbuf);
            if(client_connection_time)sprintf(&txbuf[strlen(txbuf)],"ping=%s&connected=%lld",device,client_connection_time);//if connected
              else sprintf(&txbuf[strlen(txbuf)],"ping=%s",device);
            //printf("PING(UDP): %s\n",txbuf);
          }
          //---command---->>
          //Получить свободное место. _________________________________________________________________________________________________
          if(strstr(rxbuf, "get_freesize")) {
            int freeFIFO=*(&StreamBufFree);
            cdelim(txbuf);
            sprintf(&txbuf[strlen(txbuf)],"freesize=%d",freeFIFO);
            so+=string("freesize=")+to_string(freeFIFO)+" \n";
          }
          //Получить размер блока. ___________________________________________________________________________________________________
          if(strstr(rxbuf, "get_stream_block_size")) {
            cdelim(txbuf);
            sprintf(&txbuf[strlen(txbuf)],"stream_block_size=%d",stream_block_size);
            so+=string("stream_block_size=")+to_string(stream_block_size)+"\n";
          }
          //Задать размер блока. ___________________________________________________________________________________________________
          if(strstr(rxbuf, "stream_block_size=")) {
            string v=getTagStringValue(rxbuf, "stream_block_size");
            cdelim(txbuf);
            if(v=="undefined"){
              so+=string(" !!! stream_block_size cmd error (undefined)\n");
              sprintf(&txbuf[strlen(txbuf)],"error stream_block_size=%d",stream_block_size);
            }
            else{
              stop_spi=true;
              int sb=atoi(v.c_str());
              if(!sb){
                so+=string(" !!! stream_block_size error (size==0)\n");
                sprintf(&txbuf[strlen(txbuf)],"error stream_block_size=%d",stream_block_size);
                stop_spi=false;
              }
              uint32_t m=0;
              if(stream_mode&(MODE_FIXED_DMA | MODE_STREAM)){
                device_spi_dma_reset();delay(15);
              }
              spi_stream_mode=stream_mode=0;
              m=sendCmd(WCMD_STREAM_BLOCKSIZE,(uint32_t)sb);
              if(m!=27503)so+=string(" !!! stream_block_size sendCmd return ")+to_string(m)+"\n";
                v="";if(m!=0xffffffff){string ss=((char*)&m);if(ss=="ok")v="ok";}

              if(v!="ok"){
                sprintf(txbuf,"error stream_block_size=%d",stream_block_size);
              }
              else{ //OK
                stream_block_size=sb;
                sprintf(txbuf,"stream_block_size=%d",stream_block_size);
                reinit_stream_buffer=true;
              }
            }
          }
          //переключить режим flags:MODE_FIXED_DMA|MODE_FIFO|MODE_CSUMM|MODE_NO_ACK|MODE_STREAM|MODE_UDP ______________________________
          if((strstr(rxbuf, "stream_mode="))!=NULL){
            string v=getTagStringValue(rxbuf, "stream_mode");
            if(v=="undefined"){
              cdelim(txbuf);
              sprintf(&txbuf[strlen(txbuf)],"error stream_mode=%d",stream_mode);
              so+=string("ERROR (undefined value): stream_mode=")+to_string(stream_mode)+"\n";
            }
            else{
              //printf(" SET stream_mode => %s\n",v.c_str());
              stop_spi=true;
              int sm=atoi(v.c_str());uint32_t m=0;
              if(stream_mode&(MODE_FIXED_DMA | MODE_STREAM)){
                device_spi_dma_reset();
              }
              spi_cs_low();
              spi_stream_mode=stream_mode=0;
              //------------
              if(sm&(MODE_FIXED_DMA | MODE_STREAM)){//+FIXED DMA BLOCK LEN
                m=sendCmd(WCMD_STREAM,(uint32_t)sm);
                if(m!=27503)so+=string("stream_mode sendCmd return ")+to_string(m)+"\n";
                v="";if(m!=0xffffffff){string ss=((char*)&m);if(ss=="ok")v="ok";else sm=-1;}
              }
              else v="ok";

              if(!(sm&MODE_STREAM) && (sm&MODE_FIFO) && v=="ok"){//+FIFO
                if(!spiTask)xTaskCreate(fifo_task,"fifo_spi_task",4096+device_SPI_buf_size, NULL, 5, &spiTask); // SPI FIFO buffer transfering task
                stop_spi=false;
              }
              //------------
              if(v!="ok"){
                cdelim(txbuf);
                sprintf(&txbuf[strlen(txbuf)],"error stream_mode=%d",stream_mode);
                rstseq=true;spi_stream_mode=stream_mode=0;
              }
              else{ //OK
                new_mode=sm;
                cdelim(txbuf);
                sprintf(&txbuf[strlen(txbuf)],"stream_mode=%d",new_mode);
                if(!client_connection_time && (stream_mode&MODE_UDP))client_connection_time=esp_timer_get_time();
              }
              
              if(new_mode!=stream_mode){
                if(((new_mode&MODE_UDP) != (stream_mode&MODE_UDP)) && socket_stream)restart_stream_socket=true;
                spi_stream_mode=stream_mode=sm=new_mode;
              }

              so+=string("=> STREAM MODE:")+to_string(stream_mode)+string("\n");

              if(rstseq){
                so+=string(" !!! stream_mode cmd error (ret=")+to_string(m)+string("),reset device stream...\n");
              }
              // if(restart_stream_socket){
              //   if(socket_stream>0){
              //     so+=string("STREAM socket ")+to_string(socket_stream)+string(" close...\n");
              //     shutdown(socket_stream, 2); close(socket_stream);socket_stream=-1;
              //   }
              // }
            }
          }
          // //пинг ____________________________________________________________________________________________________________________
          // if((p=strstr(rxbuf, "ping"))!=NULL){
          //   rxbuf[0]=0;
          //   if(stream_client_connected)sprintf(&txbuf[strlen(txbuf)],"ping=%s&connected=%lld",device,stream_client_connected);//if connected
          //     else sprintf(&txbuf[strlen(txbuf)],"ping=%s",device);
          // }
        }

      }//prefix

      #ifdef DBG_UDP
      uint16_t _port=htons(addr_broad_response.sin_port);
      #endif

      if(strlen(txbuf)){
        int err = 0;
        if(direct_connected){ //подключено напрямую к этой точке доступа, поэтому отсылаем напрямую
          ((struct sockaddr_in *)&source_addr)->sin_port = htons(WIFI_SERVER_UDP_RESPONSE_PORT); 
          #ifdef DBG_UDP
          _port=htons(((struct sockaddr_in*)&source_addr)->sin_port);
          #endif
          err = sendto(sock, txbuf, strlen(txbuf), 0, (struct sockaddr *)&source_addr, sizeof(struct sockaddr));
          if (err < 0) {printf("sendto error (DIRECT) : errno %d\n", errno); break;}
        }
        //отсылка через BROADCAST маршрутизатора/точки доступа на случай, если отключение клиента ещё не обнаружено
        err = sendto(sock, txbuf, strlen(txbuf), 0, (struct sockaddr *)&addr_broad_response, sizeof(sockaddr));
        if (err < 0) {printf("sendto error (BROADCAST) : errno %d\n", errno); break;}
        if(so!="")printf("%s\n",so.c_str());
        //здесь уже удачно
        #ifdef DBG_UDP
        printf("sending%s %d bytes to %s:%d : \"%s\"\n",(direct_connected?" (DIRECT+BROADCAST)":" (BROADCAST)"), err,addr_str,(int)_port,txbuf);
        #endif
        if(rdev)device_spi_get_ready();
      }
      #ifdef DBG_UDP
      else{
        printf("EMPTY TXBUF for sending%s to %s:%d\n",(direct_connected?" (DIRECT+BROADCAST)":" (BROADCAST)"), addr_str,(int)_port);
      }
      #endif
      //-----------------------  
      if(rstseq){
        device_spi_dma_reset();
        device_spi_get_ready();
        spi_cs_low();delay(10);
      }

      // if(0){//restart_stream_socket && socket_stream>0){
      //   unsigned long block = 1;int er=0;
      //   if (0 != ioctlsocket(socket_stream, FIONBIO, &block)){er=errno;printf("FAILED : FIONBIO = %d (%s)\n",er,esp_err_to_name(er));}
      //   int ss=socket_stream;int c=10;
      //   do{
      //     delay(100);ss=socket_stream;
      //   }while(ss<=0 && --c>0);
      // }
      //-----------------------        
      if(dobreak)break;

      if(rstseq){
        printf("reset sequence...\n");
        device_spi_dma_reset();
        spi_stream_mode=stream_mode=0;
        device_spi_get_ready();
        spi_cs_low();delay(10);
      }

    }//data received 
  }//while 1

  if (sock != -1) { printf("UDP socket %d closed.\n", sock); shutdown(sock, 2); close(sock); }
  free(rxbuf); free(txbuf);
  if(dobreak==2)esp_restart();
  if (reconnect){ disable_sta = true; esp_wifi_disconnect(); reconnect = false; delay(100); sta_connect(2000); }
  };//while 1

  vTaskDelete(NULL);
}
//=========================================================================================================//
bool save_dev_flash()
{
  // Open
  #ifdef DBG_UDP
  printf("Opening NVS for save ... ");
  #endif
  nvs_handle_t my_handle;
  esp_err_t err = nvs_open("storage", NVS_READWRITE, &my_handle);
  if (err != ESP_OK)
  {
    printf("Error (%s) opening NVS handle!\n", esp_err_to_name(err));
  }
  else
  {
    err = nvs_set_blob(my_handle, "dev_data", &dev, sizeof(dev_data));
    if (err)
    {
      printf("Error (%s) saving!\n", esp_err_to_name(err));
    }
    else
    {
      #ifdef DBG_UDP
      printf("NVS data saved success. Updating NVS ... ");
      #endif
      err = nvs_commit(my_handle);
      if(err != ESP_OK)printf("Failed updating NVS!\n");
      #ifdef DBG_STREAM 
      else printf("Done\n");
      #endif
    }
    // Close
    nvs_close(my_handle);
  }

  return err ? false : true;
}

bool read_dev_flash()
{
  // Open
  #ifdef DBG_STREAM
  printf("Opening NVS for read ... ");
  #endif
  nvs_handle_t my_handle;
  esp_err_t err = nvs_open("storage", NVS_READWRITE, &my_handle);
  if (err != ESP_OK)
  {
    printf("Error (%s) opening NVS handle!\n", esp_err_to_name(err));
  }                 
  else 
  {
    dev_data d;
    memset((void *)&d, 0, sizeof(dev_data));
    size_t required_size = sizeof(dev_data);
    err = nvs_get_blob(my_handle, "dev_data", (void *)&d, &required_size);
    if (err) { printf("Error (%s) reading!\n", esp_err_to_name(err)); }
    else
    {
      memcpy(&dev, &d, sizeof(dev_data));
      #ifdef DBG_STREAM
      printf("NVS data readed success.\n");
      #endif
    }
    // Close
    nvs_close(my_handle);
  }
  return err ? false : true;
}
//------------------------------------------------------------------------------------------------------
//аргумент может быть длиной до 24 бит включительно
uint32_t sendCmd(uint8_t cmd,uint32_t arg24bit){
  uint32_t c=arg24bit<<8|cmd;esp_task_wdt_reset();int v=0;char bf[65]={0};
  int e=spitx((uint8_t*)&c,4,&v);
  if(e){
    char bf2[5]={0,0,0,0,0};memcpy(bf2,(char*)&v,4);
    printf(" !!! sendCmd tx error=%s [\"%s\"]\n",string(esp_err_to_name_r(e,bf,64)).c_str(),bf2);
    return 0xffffffff;
  }
  return v;
}
//------------------------------------------------------------------------------------------------------
int getBufferSize(){
  uint32_t v=sendCmd(WCMD_BUFSIZE,0);
  if(v==0xffffffff){printf(" !!! getBufferSize cmd error\n");return -1;}
  return (int)(v);
}
//------------------------------------------------------------------------------------------------------
int getStreamBlockSize(){
  uint32_t v=sendCmd(WCMD_STREAM_BLOCKSIZE,0);
  if(v==0xffffffff){printf(" !!! getStreamBlockSize cmd error\n");return -1;}
  return (int)(v);
}
//------------------------------------------------------------------------------------------------------
int getFreeSize(){
  uint32_t v=sendCmd(WCMD_FREE,0);
  if(v==0xffffffff){printf(" !!! getFreeSize cmd error\n");return -1;}
  return (int)(v);
}
//------------------------------------------------------------------------------------------------------
//вернёт freeAfter предположительное оставшееся свободное место после отправки (будет увеличиваться)
esp_err_t prepareDeviceDMA(int& len, int* freeAfter){
  uint32_t v=sendCmd(WCMD_PREP_DMA,len);
  if(v==0xffffffff){printf(" !!! prepareDeviceDMA cmd error\n");return ESP_FAIL;}
  int freesz=v>>16;v&=0xffff;
  len=v;
  #ifdef DBG_SPI
  printf(" > prepareDeviceDMA : length=%d, return free=%d\n",(int)v,freesz);
  #endif
  if(freeAfter)*freeAfter=freesz;
  return ESP_OK;//0
}
//------------------------------------------------------------------------------------------------------
//переслать большой буфер.Устройство вернёт свободное место в spiDeviceFreeBufSize
esp_err_t send_spi_buf(char* buf,int buflen,int& spiDeviceReturnFreeSize){
  esp_err_t e=ESP_OK;int v=0;char bf[65]={0};
  int len=buflen;
  int frsz;uint8_t* pt=(uint8_t*)buf;
  int l=len;
  int ce=0;//counter prepareDeviceDMA

  #ifdef DBG_CHECK_SPI_STREAM 
  //=============ПРОВЕРКА НА ОШИБКИ ДАННЫХ=============
  check_stream_buf(buf,buflen,spiDeviceReturnFreeSize);
  printf(" CHECK_SPI_STREAM : buf OK, DMA length=%d\n",buflen);
  //===================================================
  #endif  

  static int sz=0;

  if( spi_stream_mode & (MODE_STREAM|MODE_FIXED_DMA) ){
   
    int len=buflen;char* p=buf;
    while(len){

      int l=len>stream_block_size?stream_block_size:len;

      if(sz || l<stream_block_size){ //from block/to block
        int l1=stream_block_size-sz;
        if(l1<l)l=l1;
        if(l){
          memcpy(stream_block+sz,p,l);
          sz+=l;
        }

        if(sz==stream_block_size){
          sz=0;
          e=spixfer(stream_block,stream_block_size,NULL);
          if(e){printf(" !!! send_spi_buf DMA BLOCK error=%s\n",esp_err_to_name_r(e,bf,64));return e;}
        }

        p+=l;len-=l;
        continue;
      }
      //from buf
      e=spixfer((uint8_t*)p,l,NULL);
      if(e){printf(" !!! send_spi_buf DMA STREAM len(%d) error=%s\n",l,esp_err_to_name_r(e,bf,64));break;}
      len-=l;p+=l;
    }
    return e;
  }
  else if(0){//spi_stream_mode & MODE_FIXED_DMA){
    
    spiDeviceReturnFreeSize=device_SPI_buf_size?device_SPI_buf_size:stream_block_size;
    while(len){ 
      #ifdef DBG_SPI_SENDBUF  
      printf("===> spi_stream_mode len=%d ===>\n",len);  
      #endif
      l=len;
      if(sz){
        int f=stream_block_size-sz;if(l>f)l=f;
        if(l){
          memcpy(stream_block+sz,pt,l);sz+=l;
          #ifdef DBG_SPI_SENDBUF  
          printf(" ++ copyed l=%d sz=%d ===>\n",l,sz);  
          #endif
        }
      }
      else{ //empty
        l=len;
        if(l>stream_block_size)l=stream_block_size;
        else if(l<stream_block_size){
          if(!stream_block && !(stream_block=(uint8_t*)malloc(stream_block_size))){printf(" !!! send_spi_buf DMA STREAM RAM error,no memory free !!!\n");return ESP_ERR_NO_MEM; }
          memcpy(stream_block,pt,l);sz=l;
        }
      }
      
      if(sz){
        if(sz>=stream_block_size){
          #ifdef DBG_SPI_SENDBUF  
          printf(" out sz: l=%d sz=%d ===>\n",l,sz);  
          #endif
          e=spixfer(stream_block,stream_block_size,NULL);//отправка блока DMA, буфер устанавливает устройство
          if(e){printf(" !!! send_spi_buf DMA STREAM len(%d) error=%s\n",sz,string(esp_err_to_name_r(e,bf,64)).c_str());return e;}
          sz=0;
        }
      }
      else if(l){
        #ifdef DBG_SPI_SENDBUF  
        printf(" out l: l=%d sz=%d ===>\n",l,sz);  
        #endif    
        e=spixfer(pt,l,NULL);//отправка длины меньше блока DMA, буфер устанавливает устройство
        if(e){printf(" !!! send_spi_buf DMA STREAM(%d < block %d) error=%s\n",l,stream_block_size,string(esp_err_to_name_r(e,bf,64)).c_str());return e;}
      }
      else {
        printf(" !!! send_spi_buf DMA STREAM !sz && !l error, buflen=%d\n",buflen);
        delay(1000);
        return ESP_FAIL;
      }

      pt+=l;len-=l;
      #ifdef DBG_SPI_SENDBUF  
      printf(" END: len=%d l=%d sz=%d ===>\n",len,l,sz);  
      #endif
    }
    #ifdef DBG_SPI_SENDBUF  
    printf("SPI SENDBUF END: len=%d l=%d sz=%d\n",len,l,sz);  
    #endif
  }
  //================================================================================
  else { // DMA BLOCK LEN NOT FIXED 
    //каждая транзакция должна начинаться с подготовки DMA
    while(len){ 
      l=len;
      if(l>buscfg.max_transfer_sz)l=buscfg.max_transfer_sz;

      if(ESP_OK!=(e=prepareDeviceDMA(l, &frsz ))){spiDeviceReturnFreeSize=0;return e;}
      if(l<=0){
        if(l<0){
          printf(" !!! SPI ERROR : send_spi_buf return incorrect free DMA length=%d\n",l);
          return ESP_ERR_INVALID_ARG;
        }
        ce++;//counter prepareDeviceDMA
        if(ce>=64){
          errors|=ERR_SPI_ERROR;
          printf(" !!! SPI ERROR : ERR_SPI_ERROR, stop...\n");
          return ESP_FAIL;
        }
        continue;
      }
      ce=0;
      if(l>buscfg.max_transfer_sz){
        //printf(" !!! SPI ERROR : send_spi_buf return very big free DMA length=%d\n",l);
        //return ESP_ERR_INVALID_ARG;
        l=buscfg.max_transfer_sz;
      }
      e=spitx(pt,l,&v,4);//отправка длины DMA, буфер устанавливает устройство
      if(e){printf(" !!! send_spi_buf DMA len(%d) error=%s\n",l,string(esp_err_to_name_r(e,bf,64)).c_str());spiDeviceReturnFreeSize=0;return e;}
      pt+=l;len-=l;
      frsz=v>>16;v&=0xffff;
      #ifdef DBG_SPI
      printf(" > send_spi_buf return free=%d, loaded=%d\n",frsz,(int)v);
      #endif
      if((int)v>device_SPI_buf_size){
        errors|=ERR_SPI_ERROR;spiDeviceReturnFreeSize=0;
        printf(" !!! ERROR DMA loaded (%d) > device_bufsize (%d) !!!\n",(int)v,device_SPI_buf_size);
        return ESP_FAIL;
        //vTaskDelay(1000);
      }
      else spiDeviceReturnFreeSize=frsz;
    }//while len
  }//!stream
        
  return ESP_OK;//0
}
//------------------------------------------------------------------------------------------------------
void restart_stream_mode(){
  device_spi_dma_reset();
  device_spi_get_ready();
  if(stream_mode&MODE_FIXED_DMA){//+FIXED DMA BLOCK LEN
    uint32_t m=sendCmd(WCMD_STREAM,0);
    if(m!=27503){
      printf("restart_stream_mode ERROR : stream_mode sendCmd return %u, reboot...\n",m);
      esp_restart();
    }
  }
}
//------------------------------------------------------------------------------------------------------
// * * * * * * * * * * * * * * * * * * * SPI FIFO TASK * * * * * * * * * * * * * * * * * * * * * * * * *
//------------------------------------------------------------------------------------------------------
static void IRAM_ATTR fifo_task(void *pvParameters){
  bool emulateDevice=false;//эмулировать отправку на устройство;
  while(pwr==NULL){ delay(100);}//ждать установки параметров буфера

  // #ifndef SPI_NO_COPY
  // transbuf=(char*)malloc(2*device_SPI_buf_size);//transbuf
  // if(!transbuf){printf(" ! ! ! fifo_task STOPPED : error !transbuf \n"); while(1){delay(100);} }
  // int tlen=0;//transfer len
  // #endif

  printf("<==== created task for SPI%s FIFO buffer ====>\n",emulateDevice?" (EMULATE)":"");  delay(100);

  int freesize=0;//свободно в буфере устройства
  int freeFIFO=0;

  int ec=0;

  while(1){
    
    if(errors){
      print("!!! fifo_task stop: errors=%d\n",errors);
      restart_stream_mode();
    }

    bool b0=false;
    while(stop_spi){ if(!b0)printf(" ------- fifo_task: STOPPED ------- \n"); b0=true;delay(100); }
    EventBits_t bts=xEventGroupWaitBits( xEventGroupFIFO, 0b1, pdTRUE, pdFALSE, portMAX_DELAY );
    #if defined(DBG_CHECK_STREAM) || defined(DBG_STREAM)
    printf(" * new event bits=%d *\n",(int)bts);
    #endif
    if(bts!=0b1){
      #if defined(DBG_CHECK_STREAM) || defined(DBG_STREAM)
      print(" !!! fail event bits (%d) !!!\n",(int)bts);
      #endif
      continue;//ожидаем события xClearOnExit, xWaitForAllBits  
    }
    
    //esp_task_wdt_reset();
    freeFIFO=*(&StreamBufFree);
    if(freeFIFO==/*device_SPI_buf_size*/TCP_FIFO_buf_size){/*taskYIELD();*/Sleep(1);continue;}//пока нет для отправки 
    // while(0)//(freeFIFO=StreamBufFree)==device_SPI_buf_size)
    // {//пока нет для отправки 
    //   if(freeFIFO!=device_SPI_buf_size)break;
    //   vTaskDelay(1);//==usleep(1*1000); 
    //   //за время 1 мс (1 тик) указатель LScan успеет переместиться максимум на 50000тчк.с/1000*8=400 байт (не сцать!)
    //   //usleep(1);
    //   //vTaskSwitchContext();
    //   //taskYIELD();//отдать время для наполнения !!! - нихуя не работает
    // }

    // while(0){//!freesize){
    //   freesize=emulateDevice?device_SPI_buf_size/8:getFreeSize();
    //   if(freesize)break;
    //   //vTaskDelay(1);//опять не ссым на 400 байт
    // }//пока в устройстве нет места

    #if defined(DBG_CHECK_STREAM) || defined(DBG_STREAM)
    int bef;int64_t t=esp_timer_get_time();
    bef=freeFIFO;
    bool pl=(prd<=pwr);
    #endif

    int l=TCP_FIFO_buf_size-freeFIFO;
    if(l<=0){
      printf(" ------- fifo_task: STOPPED TCP_FIFO_buf_size-freeFIFO(%d)<=0------- \n",l);
      while(60){ delay(1000);}
    }

    l&=(int)~7;//сколько данных
    if(!l)continue;

    int se=0;//check send spi error

    if(prd+l>=spibuf_out){
      int r=prd+l-spibuf_out;
      int ll=spibuf_out-prd;

      if(ll<=0){
        printf(" ------- fifo_task: STOPPED ll(%d)<=0 TCP_FIFO_buf_size=%d,freeFIFO=%d ------- \n",ll,TCP_FIFO_buf_size,freeFIFO);
        while(60){ delay(1000);}
      }
     
      if(!emulateDevice){
        if(0!=(se=send_spi_buf((char*)prd,ll,freesize))){
          ec++;
          printf("===>>> FIFO task :: send_spi_buf(>=spibuf_out) return ERROR %d\n",se);
        }
        else ec=0;

        if(r){
          if(0!=(se=send_spi_buf(streamBuffer,r,freesize))){
            ec++;
            printf("===>>> FIFO task :: send_spi_buf (r) return ERROR %d\n",se);
          }
          else ec=0;
        }
      }
      
      prd=streamBuffer+r;
    }
    else{
      if(!emulateDevice){
        if(0!=(se=send_spi_buf((char*)prd,l,freesize))){
          ec++;
          printf("===>>> FIFO task :: send_spi_buf(<spibuf_out) return ERROR %d\n",se);
        }
        else ec=0;
      }
      prd+=l;
    }

    if(ec>=3){
      printf(" :: errors=%d, restart stream...\n",ec);
      restart_stream_mode();
      ec=0;
    }

    //portENTER_CRITICAL(&mux); 
    freeFIFO+=l;
    *(&StreamBufFree)=l;
    //portEXIT_CRITICAL(&mux); 

    bool bl=false;
    while(freeFIFO<0){if(!bl)printf("===>>> ERROR :: freeFIFO<0: %d\n",freeFIFO); bl=true;delay(100);}//CRITICAL
    #ifdef DBG_STREAM
    printf(" - [%lld] : free : %d -> %d , load=%d\n",t,bef,freeFIFO,l);
    #endif
  }
}
//------------------------------------------------------------------------------------------------------
bool init_stream_buffer(){
  static int old_stream_block_size=0;
  stop_spi=true;
  uint32_t time_start=xTaskGetTickCount();
  spi_cs_low();
  device_SPI_buf_size=0;do{device_SPI_buf_size=getBufferSize();delay(10);}while (device_SPI_buf_size<=0 && xTaskGetTickCount()-time_start<200);
  if(!device_SPI_buf_size){printf("*** Error getting buffer size, restart...\n");esp_restart();}
  stream_block_size=0;do{stream_block_size=getStreamBlockSize();delay(10);}while (stream_block_size<=0 && xTaskGetTickCount()-time_start<200);
  spi_cs_high();

  if(!stream_block_size)stream_block_size=DMA_BLOCK;
  if(stream_block_size>4095)stream_block_size=4095;
  if(old_stream_block_size!=stream_block_size){
    if(!(stream_block=(uint8_t*)(stream_block?realloc(stream_block,stream_block_size):malloc(stream_block_size)))){
      printf("*** Error stream_block DMA STREAM RAM error,no memory free, restart...\n");
      esp_restart();
    }
    old_stream_block_size=stream_block_size;
  }

  bool minbuf_det=false;
  portENTER_CRITICAL(&mux); 
    xEventGroupFIFO=xEventGroupCreate();
    TCP_FIFO_buf_size=2*device_SPI_buf_size;//1 out of size for receive
    if(TCP_FIFO_buf_size<10240){
      TCP_FIFO_buf_size=10240+device_SPI_buf_size;
      minbuf_det=true;
    }
    if(streamBuffer)free(streamBuffer);
    streamBuffer=(char*)malloc(TCP_FIFO_buf_size+8);
    TCP_FIFO_buf_size-=device_SPI_buf_size;
    StreamBufFree=TCP_FIFO_buf_size;
    spibuf_out=streamBuffer+TCP_FIFO_buf_size;
    prd=streamBuffer;pwr=streamBuffer;
  portEXIT_CRITICAL(&mux);

  if(!streamBuffer){
    printf("*** Error create stream buffer, restart...\n");esp_restart();
  }
  if(minbuf_det)printf("TCP_FIFO_buf_size=%d, min stream buffer=10240 bytes\n",TCP_FIFO_buf_size);
    else printf("Allocated stream buffer=%d, DMA block=%d\n",TCP_FIFO_buf_size,stream_block_size);
  stop_spi=false;
  return true;
}
//------------------------------------------------------------------------------------------------------
// * * * * * * * * * * * * * * * * * * * * SOCKET TCP/UDP * * * * * * * * * * * * * * * * * * * * * * * 
//------------------------------------------------------------------------------------------------------
static void stream_task(void *pvParameters)
{
  static TickType_t time_start = xTaskGetTickCount();
  char *device = (char *)ap_config.ap.ssid;
  int directErrors=0;

  char addr_str[128]; // char array to store client IP
  int bytes_received; // immediate bytes received

  int rxsize = 1472; //+1 term
  int txsize = 1472; //+1 term

  char *rxbuf = (char *)malloc(rxsize + 1); memset(rxbuf, 0, rxsize + 1);
  char *txbuf = (char *)malloc(txsize + 1); memset(txbuf, 0, txsize + 1);
  int er = 0;
  restart_stream_socket=false;
  //---------getting buffer size and allocate buffer-----------------------------------------
  if(!streamBuffer || reinit_stream_buffer){//если буфер не установлен
    reinit_stream_buffer=!init_stream_buffer();
    vTaskDelay(1);
  }
  //-----------------------------------------------------------------------------------------
  sockaddr_in destAddr;memset(&(destAddr.sin_zero), '\0', 8);
  /*--------------------*/
  if(stream_mode&MODE_UDP){ //IP_REASSEMBLY Enable this option (config) allows reassemblying incoming fragmented IP4 packets.
    socket_stream=create_socket(INADDR_ANY, WIFI_SERVER_PORT, SOCK_DGRAM, IPPROTO_IP, addr_str, &destAddr);
  }
  else socket_stream = create_socket(INADDR_ANY, WIFI_SERVER_PORT, SOCK_STREAM, IPPROTO_TCP, addr_str, &destAddr);
  
  if(socket_stream<0){
    printf("STREAM Socket (%s) unable to create: %s\n",stream_mode&MODE_UDP?"UDP":"TCP",esp_err_to_name(errno)); 
    esp_restart();
  }

  int flag = 1;
  if (0!=setsockopt(socket_stream, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag)))
  {
    printf("STREAM unable to SO_REUSEADDR: %s\n",  esp_err_to_name(errno)); esp_restart();
  }
  
  //Привязка: Bind a socket to a specific IP + port
  errno=0;
  if (0!=bind(socket_stream, (sockaddr *)&destAddr, sizeof(destAddr))) {
    printf("STREAM Socket %d unable to bind: %s\n",socket_stream, esp_err_to_name(errno)); esp_restart();
  }
  
  if(stream_mode&MODE_UDP){
    int oval = TCP_FIFO_buf_size;
    if(setsockopt(socket_stream, SOL_SOCKET,SO_RCVBUF, (char *)&oval, sizeof(int)) != 0) printf("SO_RCVBUF STREAM fail !!!\n");//lwip_setsockopt
  }
  else if (listen(socket_stream, (int)5) != 0){
    printf("LISTEN ERROR: errno %s\n", esp_err_to_name(errno)); esp_restart();
  }
  else printf("********************** Socket listening  ****************************\n");

  #ifdef DBG_STREAM
  int e9=0;
  #endif
  //==========================SOCKET LISTENING=============================>>
  while (!restart_stream_socket) 
  {
    bool connected = false; // for detect lost connection

    ledOff(GPIOLED0); esp_task_wdt_reset();

    struct sockaddr_in sourceAddr; // Large enough for IPv4
    int addrLen = sizeof(sourceAddr);
    errno = 0;

    int client_socket =0;

    if(stream_mode&MODE_UDP){ //UDP MODE-----------------------------
      printf("*************************** UDP MODE ********************************\n");
      //spi_cs_high();delay(15);
      //spi_cs_low();delay(15);//device_spi_get_ready();
    }
    else{                     //TCP MODE-----------------------------
      unsigned long block = 1;int er=0;
      if (0 != ioctlsocket(socket_stream, FIONBIO, &block)){er=errno;printf("FAILED : FIONBIO = %d (%s)\n",er,esp_err_to_name(er));}
      //EAGAIN или EWOULDBLOCK
      /* Accept connection to incoming client */
      while(!restart_stream_socket){
        if ((client_socket = accept(socket_stream, (struct sockaddr *)&sourceAddr, (socklen_t*)&addrLen)) < 0)
        {
          int er = errno;
          if(er==EAGAIN || er==EWOULDBLOCK){
            vTaskDelay(1);
            continue;
          }
          if (connected)
          {
            printf("LOST connection : errno %d (%s)\n", er, esp_err_to_name(er));// (errno == 23 ? ": Too many open files in system" : ""));
          }
          else
          {
            inet_ntoa_r(sourceAddr.sin_addr, addr_str, sizeof(addr_str) - 1);
            #ifdef DBG_STREAM
            if(!e9)printf("Unable to accept connection:socket_stream=%d, ip=%s , errno %d %s\n",socket_stream, addr_str, er,esp_err_to_name(er));
            // (er == 23 ? ": Too many open files in system" : ""));
            if(er==9)e9=1;
            #endif
          }
          
          // if (er == 9)break; // bad file, create new socket
          if (er == 23) esp_restart();
          vTaskDelay(1);
          continue; //not accepted
        }
        else
        {
          #ifdef DBG_STREAM
          e9=0;
          #endif
          connected = true;
          esp_task_wdt_reset();
          block = 0;er=0;//to blocking mode
          if (0 != ioctlsocket(socket_stream, FIONBIO, &block)){er=errno;printf("FAILED accept FIONBIO = %d (%s)\n",er,esp_err_to_name(er));}
          break;
          //#ifdef DBG_TCPprintf("Socket accepted\n");
        }
      };//while accept
      if(restart_stream_socket)break;
      //-----------ok--------------
      if(!client_connection_time)client_connection_time=esp_timer_get_time();
      int ndval = 1;if(setsockopt(client_socket, IPPROTO_TCP, TCP_NODELAY, (char *)&ndval, sizeof(int)) != ESP_OK) printf("TCP_NODELAY (client) fail !!!\n");
      ndval=device_SPI_buf_size;
      if(setsockopt(client_socket, SOL_SOCKET,SO_RCVBUF, (char *)&ndval, sizeof(int)) != 0) printf("SO_RCVBUF (client) fail !!!\n");//lwip_setsockopt
    }
    
    rxbuf[0] = 0;

    /**
     ERRORS : e:\dev\.espressif\tools\xtensa-esp32-elf\esp-2021r2-patch3-8.4.0\xtensa-esp32-elf\xtensa-esp32-elf\include\sys\errno.h
     e:\dev\esp-idf\components\lwip\lwip\src\include\lwip\errno.h
    */
    int left=0;//осталось принять бинарных
    int rcvd=0;//принято
    /*
    fd_set rfds, wfds, efds; FD_ZERO(&rfds);FD_SET(client_socket, &rfds);
    struct timeval tv;tv.tv_sec = 0; tv.tv_usec = 1000;//1 ms
    unsigned long block = 1;
    if (0 != ioctlsocket(socket_stream, FIONBIO, &block)){er=errno; printf("FAILED : FIONBIO %d (%s)\n",er,esp_err_to_name(er));}
    */
    spi_cs_low();

    while (!restart_stream_socket) //==============RECV LOOP================>>
    {
      esp_task_wdt_reset();
      if(client_connection_time && pdTICKS_TO_MS(xTaskGetTickCount() - time_start) > 1000)
      {
        time_start = xTaskGetTickCount();
        toggleLED(GPIOLED0);
      }

      if(errors){
        printf("STREAM [1460]: errors=%d, pause 5000, stop->reboot...\n",errors);
        vTaskDelay(5000);
        esp_restart();
      }

      //------------
      int freeFIFO=StreamBufFree;int right=0;
      
      if(left){
        right=left;
        if(right>freeFIFO)right=freeFIFO;//в любом случае больше записать нельзя даже с выходом справа, т.к. перекроется указатель чтения
        right&=(int)~7;
      }
      //------------
      memset(rxbuf,0,rxsize);
      //==============================================
      // int retval = lwip_select(client_socket+1, &rfds,0,0,&tv);
      // printf("select = %d =\n",retval);
      // if(retval==0)continue;//нет событий
      // Теперь FD_ISSET(0, &rfds) вернет истинное значение.
      //EBADF 9 В одном из наборов находится неверный файловый описатель.
      //EINTR 4 Был получен незаблокированный сигнал.
      //EINVAL 22/1 n отрицательно или значение, содержащееся внутри timeout, некорректно.
      //ENOMEM 12/1 Функция select не может выделить объем памяти для внутренних таблиц.

      //выбираем куда писать. Если идут данные-пишем по указателю FIFO
      char* pwrPtr=(char*)pwr;
       if(stream_mode&MODE_UDP){ //UDP MODE-----------------------------
        //recvfrom получает датаграмму и сохраняет исходный адрес.
        if(stream_mode & MODE_STREAM){
          //printf("receive udp stream...\n");
          bytes_received = recvfrom(socket_stream, streamBuffer+rcvd, /*TCP_FIFO_buf_size*/stream_block_size-rcvd+((stream_mode&MODE_CSUMM)?8:0), 0, (struct sockaddr *)&sourceAddr, (socklen_t*)&addrLen);
        }
        else {
          //printf("receive udp...\n");
          bytes_received = recvfrom(socket_stream, left?pwrPtr+rcvd:rxbuf, left?right:rxsize, 0, (struct sockaddr *)&sourceAddr, (socklen_t*)&addrLen);
        }
      }
      else{                     //TCP MODE-----------------------------
        if(!client_socket)break;
        if(stream_mode & MODE_STREAM){
          bytes_received = recv(client_socket, streamBuffer+rcvd, /*TCP_FIFO_buf_size*/stream_block_size-rcvd+((stream_mode&MODE_CSUMM)?8:0), 0);
        }
        else {
          bytes_received = recv(client_socket, left?pwrPtr+rcvd:rxbuf, left?right:rxsize, 0);
        }
      }
      //printf("bytes_received=%d\n",bytes_received);
      
      // if(!left){
      //   unsigned long block = 1;
      //   if (0 != ioctlsocket(socket_stream, FIONBIO, &block)){er=errno; printf("FAILED : FIONBIO %d (%s)\n",er,esp_err_to_name(er));}
      //   retval = lwip_select(client_socket+1, &rfds,0,0,&tv);
      //   //printf("select err %d (%s)\n",retval,esp_err_to_name(retval));
      //   block =0;ioctlsocket(socket_stream, FIONBIO, &block);
      //   //struct lwip_sock *sock=get_socket(client_socket);
      //   txbuf[0]=0;
      //   //int br=recv(client_socket,txbuf, rxsize, 0);
      //   //if(br>0)txbuf[br]=0;//term
      //   //printf("recv br=%d rx=[%s]\n",br,txbuf);
      // }
      #if defined(DBG_STREAM) || defined (DBG_TCP_REQ)
        int64_t t=esp_timer_get_time();
        //printf("===> [%lld] left=%d , right=%d , free=%d received=%d\n",esp_timer_get_time(),left,right,freeFIFO,bytes_received);
      #endif
      //------------------------------------------------
      //  При приеме произошла ошибка
      if (bytes_received < 0 || restart_stream_socket) //происходит при отключении от точки доступа
      {
        er = errno;
        if (er == ECONNRESET || er == EBADF || er == ENOTCONN || restart_stream_socket){
          printf("<===== STA DISCONNECTED (%d) =====>\n",er); 
          spi_stream_mode=stream_mode=0;left=right=0;client_connection_time=0;restart_stream_socket=true;
          device_spi_dma_reset();delay(15);device_spi_get_ready();
          if(client_socket>0){shutdown(client_socket, 2); close(client_socket);}
          if(restart_stream_socket && socket_stream>0){shutdown(socket_stream, 2); close(socket_stream);socket_stream=-1;}
          break; 
        }
        if (er != EAGAIN)printf("===>>> ERROR: Received Data=%d , err.code=%d\n", bytes_received, er);
        /*vTaskDelay(100 / portTICK_PERIOD_MS);*/ continue;
      }
      else if (bytes_received == 0){ 
        if (errno != EAGAIN){
           printf("<===== STA DISCONNECTED (no data),errno %d =====>\n",errno); //if application close
          spi_stream_mode=stream_mode=0;left=right=0;client_connection_time=0;restart_stream_socket=true;
          device_spi_dma_reset();delay(15);device_spi_get_ready();
          if(client_socket>0){shutdown(client_socket, 2); close(client_socket);}
          break; 
        }
        printf("<============ End data ============>\n");
      }

      if(restart_stream_socket)break;
      if(!client_connection_time)client_connection_time=esp_timer_get_time();

      // Data received
      esp_task_wdt_reset();//memset(txbuf, 0, txsize);
      
      #if defined(DBG_CHECK_STREAM) || defined(DBG_STREAM) || defined(DBG_TEST_STREAM)
      int bef=freeFIFO;
      #endif

      //STREAM MODE-------------------------------------------------------------------------->>>
      if(stream_mode & MODE_STREAM){
        rcvd+=bytes_received;
        //if((stream_mode&MODE_FIXED_DMA) && rcvd<stream_block_size)continue;
        if(rcvd<stream_block_size)continue; //для STREAM устанавливается DMA !!!

        bool summ_correct=true;
        if((stream_mode&MODE_CSUMM)){
          rcvd-=8;uint64_t* sum=(uint64_t*)(streamBuffer+rcvd);uint64_t summ=0;
          for(size_t i=0;i<rcvd;i+=4){summ+=*((uint32_t*)&streamBuffer[i]);}
          if(summ != *sum)summ_correct=false;
          if(summ_correct){
            sprintf(txbuf,"ok");
            if(((stream_mode & MODE_UDP) ? sendto(socket_stream, txbuf, 2, 0, (sockaddr*)&sourceAddr, sizeof(sourceAddr)):send(client_socket, txbuf, 2, 0))<0)
              printf("TCP send \"ok\" (stream direct) ERROR=%d\n", errno);
          }
          else{  
            sprintf(txbuf,"error");
            if(((stream_mode & MODE_UDP) ? sendto(socket_stream, txbuf, strlen(txbuf), 0, (sockaddr*)&sourceAddr, sizeof(sourceAddr)):send(client_socket, txbuf, strlen(txbuf), 0))<0)
              printf("TCP send \"error\" (stream direct) ERROR=%d\n", errno);
            printf("!!! TCP: SUMM ERROR\n");
          }
        }
        else if(!(stream_mode&MODE_NO_ACK)){ //NO SUMM
          sprintf(txbuf,"ok");
          if(((stream_mode & MODE_UDP) ? sendto(socket_stream, txbuf, 2, 0, (sockaddr*)&sourceAddr, sizeof(sourceAddr)):send(client_socket, txbuf, 2, 0))<0)
              printf("TCP send \"ok\" (stream direct) ERROR=%d\n", errno);
        }

        if(summ_correct){
          int fr=0;int re=0;
          if(0!=(re=send_spi_buf(streamBuffer,rcvd,fr))){
            printf("STREAM: send spi (direct stream) ERROR=%d\n",re);//SEND DIRECT
            //---------------------
            ++directErrors;
            if(directErrors%3==0){
              if(directErrors>3){
                printf("ERROR: send_spi_buf cycles=%d,reboot...\n",directErrors);
                reset_device();esp_restart();
              }
              else {
                printf("ERROR: send_spi_buf cycles=%d,reset spi bus...\n",directErrors);
                device_spi_dma_reset();delay(50);
              }
            }
            //---------------------
          }
          else directErrors=0;
        }

        rcvd=0;
        continue;
      }
      else if(left){// LOAD------------------------------------------------------------------>>>
        
        bool summ_correct=true;

        if(!(stream_mode & MODE_FIFO))freeFIFO=TCP_FIFO_buf_size;else freeFIFO=StreamBufFree;
        
        left-=bytes_received;
        rcvd+=bytes_received;

        #ifdef DBG_CHECK_STREAM 
          bef=freeFIFO;
        #endif  

        #ifdef DBG_TEST_STREAM
          StreamBufFree=TCP_FIFO_buf_size;//device_SPI_buf_size;
          pwr=streamBuffer;
        #else
        
          if(left==0 && rcvd){
            
            if((stream_mode&MODE_CSUMM)){
              rcvd-=8;uint64_t* sum=(uint64_t*)(pwr+rcvd);uint64_t summ=0;
              for(size_t i=0;i<rcvd;i+=4){summ+=*((uint32_t*)&pwr[i]);}
              if(summ != *sum)summ_correct=false;
            }
            if(summ_correct){
              //=============ПРОВЕРКА НА ОШИБКИ ДАННЫХ=============
              #ifdef DBG_CHECK_STREAM 
                check_stream_buf((char*)pwr,rcvd,bef);
              #endif  

              if(!(stream_mode & MODE_FIFO)){// not FIFO
                int fr=0;
                int re=0;
                //printf("STREAM send direct...\n");//SEND DIRECT
                if(0!=(re=send_spi_buf(pwrPtr,rcvd,fr))){
                  printf("STREAM send spi (direct) ERROR=%d\n",re);//SEND DIRECT
                  //---------------------
                  ++directErrors;
                  if(directErrors%3==0){
                    if(directErrors>3){
                      printf("ERROR: send_spi_buf cycles=%d,reboot...\n",directErrors);
                      reset_device();
                      esp_restart();
                    }
                    else {
                      printf("ERROR: send_spi_buf cycles=%d,reset spi bus...\n",directErrors);
                      device_spi_dma_reset();delay(50);
                    }
                  }
                  //---------------------
                }
                else directErrors=0;
                //else printf("spi tx ok\n");
                pwr=streamBuffer;
              }
              else{  
                if(pwr+rcvd>=spibuf_out){
                  int r=pwr+rcvd-spibuf_out;
                  if(r)memcpy(streamBuffer,spibuf_out,r);
                  pwr=streamBuffer+r;
                }
                else{
                  pwr+=rcvd;
                }
                //portENTER_CRITICAL(&mux); 
                freeFIFO=(StreamBufFree-=rcvd);
                //portEXIT_CRITICAL(&mux);
              }// FIFO
            }//summ correct
            else {
              printf("!!! STREAM: SUMM ERROR\n");
            }
          }//left 0 && rcvd

        #endif //#ifdef DBG_TEST_STREAM

        bool bl=false;
        while(left<0){ stop_spi=true;if(!bl)printf("===>>> ERROR STREAM :: left<0: %d , bytes_received=%d , stop all...\n",left,bytes_received); bl=true;vTaskDelay(1); }

        #ifdef DBG_STREAM
          if(left)printf(" + [%lld] : free : %d , received=%d (total=%d) , left=%d , right=%d\n",t,freeFIFO,bytes_received,rcvd,left,right);
          else printf(" + [%lld] : free : %d -> %d , received=%d (total=%d) , left=%d , right=%d\n",t,bef,freeFIFO,bytes_received,rcvd,left,right);
        #endif

        if(left==0){
          if ((stream_mode | MODE_CSUMM) || !(stream_mode & MODE_NO_ACK)){
            if(summ_correct)sprintf(txbuf,"received=%d&freesize=%d",rcvd,freeFIFO);else sprintf(txbuf,"summ=error");
            rcvd=0;
            int ss=(stream_mode & MODE_UDP) ? sendto(socket_stream, txbuf, strlen(txbuf), 0, (sockaddr*)&sourceAddr, sizeof(sourceAddr)):send(client_socket, txbuf, strlen(txbuf), 0);
            if(ss<0)printf("STREAM send (stream) ERROR=%d\n", errno);
            #ifdef DBG_STREAM
            //else printf("   [%lld] stream tx: \"%s\"\n",t,txbuf);
            #endif
          }

          if((stream_mode&MODE_FIFO)){// if FIFO
            xEventGroupSetBits(xEventGroupFIFO, 0b1);
            //vTaskSwitchContext();
            vPortYield();//отдать время для наполнения !!!
          }
        }
        continue;//продолжать читать
      }
      //---command---->>
      rxbuf[bytes_received] = 0; char* p=NULL;
      //команда записи потока.Здесь нужно отдать столько, сколько можно записать за раз ___________________________________________
      if((p=strstr(rxbuf, "stream="))!=NULL){
        
        if(stream_mode&MODE_FIXED_DMA){
          left=stream_block_size;
        }
        else{
          string v=getTagStringValue(rxbuf, "stream");
          if(v=="undefined"){
            printf("STREAM CRIT ERROR !v: rxbuf:\"%s\"\n",rxbuf);
            break;
            //while(1)vTaskDelay(1000);
          }
          left=atoi(v.c_str());
          rcvd=0;
          #if defined(DBG_STREAM) || defined (DBG_TCP_REQ)
          int req=left;
          #endif
          rxbuf[0]=0;
          if(!(stream_mode&MODE_FIFO)){// no FIFO
            freeFIFO=device_SPI_buf_size;
            if(left>freeFIFO)left=freeFIFO;
          }
          else{ // WITH FIFO
            freeFIFO=StreamBufFree;
            if(left>freeFIFO)left=freeFIFO;
          }
          left&=(int)~7;//- проверка длины проверяется значением right перед чтением
        }
        
        sprintf(txbuf,"ready_stream=%d%s",left,(stream_mode&MODE_CSUMM)?"&summ=8":"");
        if(left && (stream_mode&MODE_CSUMM))left+=8;

        int sr=(stream_mode & MODE_UDP) ? sendto(socket_stream, txbuf, strlen(txbuf), 0, (sockaddr*)&sourceAddr, sizeof(sourceAddr)):send(client_socket, txbuf, strlen(txbuf), 0);
        if(sr<=0)printf("STREAM send (cmd stream) ERROR=%d\n", errno);

        #if defined(DBG_STREAM) || defined (DBG_TCP_REQ)
        else printf("_____ [%lld] \"%s\" request=%d free=%d _____\n",t,txbuf,req,freeFIFO);
        #endif

        continue;
      }
      //Получить свободное место. _________________________________________________________________________________________________
      if(strstr(rxbuf, "get_freesize")) {
        rxbuf[0]=0;
        freeFIFO=*(&StreamBufFree);
        sprintf(txbuf,"freesize=%d",freeFIFO);
        if(((stream_mode & MODE_UDP) ? sendto(socket_stream, txbuf, strlen(txbuf), 0, (sockaddr*)&sourceAddr, sizeof(sourceAddr)):send(client_socket, txbuf, strlen(txbuf), 0))<0)
          printf("STREAM send (cmd get_freesize)) ERROR=%d\n", errno);
        #ifdef DBG_STREAM
        else printf("[%lld] freesize=%d\n",t,freeFIFO);
        #endif
        continue;
      }
      //Получить размер блока. ___________________________________________________________________________________________________
      if(strstr(rxbuf, "get_stream_block_size")) {
        rxbuf[0]=0;
        sprintf(txbuf,"stream_block_size=%d",stream_block_size);
        if(((stream_mode & MODE_UDP) ? sendto(socket_stream, txbuf, strlen(txbuf), 0, (sockaddr*)&sourceAddr, sizeof(sourceAddr)):send(client_socket, txbuf, strlen(txbuf), 0))<0)
          printf("STREAM send (cmd stream_block_size)) ERROR=%d\n", errno);
        #ifdef DBG_STREAM
        else printf("[%lld] stream_block_size=%d\n",t,stream_block_size);
        #endif
        continue;
      }
      //Задать размер блока. ___________________________________________________________________________________________________
      if(strstr(rxbuf, "stream_block_size=")) {
        string v=getTagStringValue(rxbuf, "stream_block_size");
        if(v=="undefined"){
          printf(" !!! stream_block_size cmd error (undefined)\n");
          sprintf(txbuf,"error stream_block_size=%d",stream_block_size);
          if(((stream_mode & MODE_UDP) ? sendto(socket_stream, txbuf, strlen(txbuf), 0, (sockaddr*)&sourceAddr, sizeof(sourceAddr)):
            send(client_socket, txbuf, strlen(txbuf), 0))<0)printf("STREAM send (cmd stream_block_size)) ERROR=%d\n", errno);
          continue;
        }
        stop_spi=true;
        int sb=atoi(v.c_str());
        if(!sb){
          printf(" !!! stream_block_size error (size==0)\n");
          sprintf(&txbuf[strlen(txbuf)],"error stream_block_size=%d",stream_block_size);
          if(((stream_mode & MODE_UDP) ? sendto(socket_stream, txbuf, strlen(txbuf), 0, (sockaddr*)&sourceAddr, sizeof(sourceAddr)):
            send(client_socket, txbuf, strlen(txbuf), 0))<0)printf("STREAM send (cmd stream_block_size)) ERROR=%d\n", errno);
          stop_spi=false;
          continue;
        }
        uint32_t m=0;
        if(stream_mode&(MODE_FIXED_DMA | MODE_STREAM)){
          device_spi_dma_reset();delay(15);
        }
        spi_stream_mode=stream_mode=0;
        m=sendCmd(WCMD_STREAM_BLOCKSIZE,(uint32_t)sb);
        if(m!=27503)printf("stream_block_size sendCmd return %u\n",m);
          v="";if(m!=0xffffffff){string ss=((char*)&m);if(ss=="ok")v="ok";}

        if(v!="ok"){
          sprintf(txbuf,"error stream_block_size=%d",stream_block_size);
        }
        else{ //OK
          stream_block_size=sb;
          sprintf(txbuf,"stream_block_size=%d",stream_block_size);
          reinit_stream_buffer=true;
        }

        if(((stream_mode & MODE_UDP) ? sendto(socket_stream, txbuf, strlen(txbuf), 0, (sockaddr*)&sourceAddr, sizeof(sourceAddr)):
            send(client_socket, txbuf, strlen(txbuf), 0))<0)printf("STREAM send (cmd stream_block_size)) ERROR=%d\n", errno);

        #ifdef DBG_STREAM
        else printf("[%lld] stream_block_size=%d\n",t,stream_block_size);
        #endif
        continue;
      }
      //переключить режим flags:MODE_FIXED_DMA|MODE_FIFO|MODE_CSUMM|MODE_NO_ACK|MODE_STREAM|MODE_UDP ______________________________
      if((p=strstr(rxbuf, "stream_mode="))!=NULL){
        string v=getTagStringValue(rxbuf, "stream_mode");
        if(v=="undefined"){
          printf(" !!! stream_mode cmd error (undefined)\n");
          sprintf(txbuf,"error stream_mode=%d",stream_mode);
          if(((stream_mode & MODE_UDP) ? sendto(socket_stream, txbuf, strlen(txbuf), 0, (sockaddr*)&sourceAddr, sizeof(sourceAddr)):
            send(client_socket, txbuf, strlen(txbuf), 0))<0)printf("STREAM send (cmd stream_mode)) ERROR=%d\n", errno);
          continue;
        }
        //else printf(" SET stream_mode => %s\n",v.c_str());
        stop_spi=true;
        int sm=atoi(v.c_str());uint32_t m=0;
        if(stream_mode&(MODE_FIXED_DMA | MODE_STREAM)){
          device_spi_dma_reset();
        }
        spi_cs_low();
        spi_stream_mode=stream_mode=0;
        //------------
        if(sm&(MODE_FIXED_DMA | MODE_STREAM)){//+FIXED DMA BLOCK LEN
          m=sendCmd(WCMD_STREAM,(uint32_t)sm);
          if(m!=27503)printf("stream_mode sendCmd return %u\n",m);
          v="";if(m!=0xffffffff){string ss=((char*)&m);if(ss=="ok")v="ok";else sm=-1;}
        }
        else v="ok";

        if(!(sm&MODE_STREAM) && (sm&MODE_FIFO) && v=="ok"){//+FIFO
          if(!spiTask)xTaskCreate(fifo_task,"fifo_spi_task",4096+device_SPI_buf_size, NULL, 5, &spiTask); // SPI FIFO buffer transfering task
          stop_spi=false;
        }
        //------------
        bool rstseq=false;int new_mode=stream_mode;
        if(v!="ok"){
          sprintf(txbuf,"error stream_mode=%d",stream_mode);
          rstseq=true;
        }
        else{ //OK
          new_mode=sm;
          sprintf(txbuf,"stream_mode=%d",new_mode);
          if(!client_connection_time && (stream_mode&MODE_UDP))client_connection_time=esp_timer_get_time();
        }

        if((sm=((stream_mode & MODE_UDP) ? sendto(socket_stream, txbuf, strlen(txbuf), 0, (sockaddr*)&sourceAddr, sizeof(sourceAddr)):
          send(client_socket, txbuf, strlen(txbuf), 0)))<0)printf("STREAM send (cmd stream_mode)) ERROR=%d\n", errno);
        
        if(new_mode!=stream_mode){
          if(((new_mode&MODE_UDP) != (stream_mode&MODE_UDP)) && socket_stream)
          {
            if(client_socket>0){shutdown(client_socket, 2); close(client_socket);client_socket=-1;}
            printf("STREAM socket %d close !!!\n", socket_stream); shutdown(socket_stream, 2); close(socket_stream);socket_stream=-1;
            restart_stream_socket=true;
          }
          spi_stream_mode=stream_mode=sm=new_mode;
        }

        printf("=> STREAM MODE: %s\n",(to_string(stream_mode)).c_str());

        if(rstseq){
          printf(" !!! stream_mode cmd error (ret=%d,string value=%s),reset device stream...\n",m,v.c_str());
          device_spi_dma_reset();
          spi_stream_mode=stream_mode=0;
          device_spi_get_ready();
          spi_cs_low();delay(10);
        }
        if(restart_stream_socket)break;
        continue;
      }
      //пинг ____________________________________________________________________________________________________________________
      if((p=strstr(rxbuf, "ping"))!=NULL){
        rxbuf[0]=0;
        if(client_connection_time)sprintf(txbuf,"ping=%s&connected=%lld",device,client_connection_time);//if connected
          else sprintf(txbuf,"ping=%s",device);
        if(((stream_mode & MODE_UDP) ? sendto(socket_stream, txbuf, strlen(txbuf), 0, (sockaddr*)&sourceAddr, sizeof(sourceAddr)):send(client_socket, txbuf, strlen(txbuf), 0))<0)printf("STREAM send (cmd ping)) ERROR=%d\n", errno);
        //printf("PING(TCP): %s\n",txbuf);
        continue;
      }
      //неизвестная команда
      sprintf(txbuf, "error:unknown command \"%s\"\n",rxbuf);
      rxbuf[0]=0;
      if(((stream_mode & MODE_UDP) ? sendto(socket_stream, txbuf, strlen(txbuf), 0, (sockaddr*)&sourceAddr, sizeof(sourceAddr)):send(client_socket, txbuf, strlen(txbuf), 0))<0)printf("STREAM send ERROR=%d\n", errno);

    } //чтение while 1

    if(restart_stream_socket)break;

  } // soket listening

  client_connection_time=0;

  if (socket_stream>0)
  {
    printf("<-----------------[STREAM socket %d closing]----------------->\n", socket_stream); shutdown(socket_stream, 2); close(socket_stream);socket_stream=-1;taskYIELD();
  }

  free(rxbuf); free(txbuf);
}
