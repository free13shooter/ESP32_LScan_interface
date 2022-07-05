#include "AP_server.h"

//---------------------------------------------------------------------------------------------------------//
WiFiServer *server = NULL;
IPAddress server_ip(0, 0, 0, 0);
string *ssid;

static const char *TAG = "LScan";

static esp_netif_t *esp_netifs[ESP_IF_MAX] = {NULL, NULL, NULL};
//------------
int client_socket;
int ip_protocol;
int socket_id;
int bind_err;
int listen_error;

// FreeRTOS event group to signal when we are connected & ready to make a request
static EventGroupHandle_t wifi_event_group;

const int IPV4_GOTIP_BIT = BIT0;

static void tcp_server_task(void *pvParameters);
static void WiFiServer_task(void *pvParameters);
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
static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
  if (event_id == WIFI_EVENT_AP_STACONNECTED)
  {
    wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *)event_data;
    ESP_LOGW(TAG, "station " MACSTR " join, AID=%d", MAC2STR(event->mac), event->aid);
    xEventGroupSetBits(wifi_event_group, IPV4_GOTIP_BIT);
  }
  else if (event_id == WIFI_EVENT_AP_STADISCONNECTED)
  {
    wifi_event_ap_stadisconnected_t *event = (wifi_event_ap_stadisconnected_t *)event_data;
    ESP_LOGW(TAG, "station " MACSTR " leave, AID=%d", MAC2STR(event->mac), event->aid);
    // On disconnet, close TCP socket client
    if (client_socket != -1)
    {
      ESP_LOGE(TAG, "Shutting down socket and restarting...");
      shutdown(client_socket, 0);
      close(client_socket);
    }
  }
}
//---------------------------------------------------------------------------------------------------------//
static void IP_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
  if (event_base == IP_EVENT && event_id == IP_EVENT_AP_STAIPASSIGNED)
  {
    ip_event_ap_staipassigned_t *event = (ip_event_ap_staipassigned_t *)event_data;
    ESP_LOGW(TAG, "ASSIGNED IP:" IPSTR, IP2STR(&event->ip));
  }
}
//=========================================================================================================//
//=========================================================================================================//
esp_err_t wifi_init_softap(void)
{
  printf("\n_________________________________________________________\n *    *    *\nbegin AP setup...\n");
  esp_wifi_stop();
  //---------------------------------------
  if (!server_ip.fromString(WIFI_SERVER_IP))
  {
    ESP_LOGE(TAG, "ERROR IP::fromString(%s), init failed.\n", WIFI_SERVER_IP);
    return ESP_ERR_ESP_NETIF_INIT_FAILED;
  }
  wifi_event_group = xEventGroupCreate(); // Create listener thread
  ESP_ERROR_CHECK(esp_netif_init());      // Initialize the underlying TCP/IP lwIP stack
  // ESP_ERROR_CHECK(esp_event_loop_init(event_handler, NULL) ); //Start event_handler loop
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  esp_netif_t *wifiAP = esp_netifs[WIFI_IF_AP] = esp_netif_create_default_wifi_ap();
  esp_netif_ip_info_t ipInfo;
  IP4_ADDR(&ipInfo.ip, server_ip[0], server_ip[1], server_ip[2], server_ip[3]);
  IP4_ADDR(&ipInfo.gw, 0, 0, 0, 0); // не рекламировать как маршрутизатор-шлюз
  IP4_ADDR(&ipInfo.netmask, 255, 255, 255, 0);
  esp_netif_dhcps_stop(wifiAP);
  esp_netif_set_ip_info(wifiAP, &ipInfo);
  esp_netif_dhcps_start(wifiAP);

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));
  //---------------------
  esp_event_handler_instance_t instance_any_id;
  esp_event_handler_instance_t instance_got_ip;
  ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &instance_any_id));
  // ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_AP_STAIPASSIGNED, &IP_event_handler, NULL, &instance_got_ip));

  wifi_config_t wifi_config;
  memset(&wifi_config, 0, sizeof(wifi_config));
  wifi_config.ap.channel = WIFI_CHANNEL;
  wifi_config.ap.max_connection = WIFI_MAX_CLIENTS;
  wifi_config.ap.authmode = WIFI_AUTH_MODE;

  sprintf((char *)wifi_config.ap.ssid, (string("LASER_SCAN_2_") + get_chip_id()).c_str()); // print High 2 bytes
  sprintf((char *)wifi_config.ap.password, WIFI_PASS);                               // print High 2 bytes
  wifi_config.ap.ssid_len = (uint8_t)strlen((char *)wifi_config.ap.ssid);

  if (wifi_config.ap.ssid_len == 0)
    wifi_config.ap.authmode = WIFI_AUTH_OPEN;

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
  ESP_ERROR_CHECK(esp_wifi_start());
  //==========
  if (esp_wifi_set_ps(WIFI_PS_NONE) != ESP_OK) // set sleep false
  {
    ESP_LOGE(TAG, "esp_wifi_set_ps WIFI_PS_NONE failed!");
  }
  esp_netif_ip_info_t ip;
  if (esp_netif_get_ip_info(get_esp_interface_netif(ESP_IF_WIFI_AP), &ip) != ESP_OK)
  {
    ESP_LOGE(TAG, "Netif Get IP Failed!\n");
  }
  else
  {
    IPAddress myIP = ip.ip.addr;
    printf("\n");
    ESP_LOGW(TAG, "AP \"%s\" IP address: %d.%d.%d.%d\n", wifi_config.ap.ssid, (int)myIP[0], (int)myIP[1], (int)myIP[2], (int)myIP[3]);
  }
  //==========
  return ESP_OK;
}
//=========================================================================================================//
esp_err_t WiFi_AP_start()
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
  e = wifi_init_softap();
  if (e)
    return e;
  // xTaskCreate(tcp_server_task, "tcp_server", 4096, NULL, 5, NULL);
  //xTaskCreate(WiFiServer_task, "tcp_server", 4096, NULL, 5, NULL);
  //WiFiServer_task(NULL);
  tcp_server_task(NULL);
  return ESP_OK;
}
//------------------------------------------------------------------------------------------------------
// * * * * * * * * * * * * * * * * * * * * SOCKET LISTENING * * * * * * * * * * * * * * * * * * * * * *
//------------------------------------------------------------------------------------------------------
static void tcp_server_task(void *pvParameters)
{
  
  // char rx_buffer[129]; // char array to store received data
  // char tx_buffer[129]; // char array to store received data
  char addr_str[128];  // char array to store client IP
  int bytes_received;  // immediate bytes received
  int addr_family;     // Ipv4 address protocol variable

  int rxsize=1472;//+1 term
  int txsize=1472;//+1 term
  char* rxbuf=(char*)malloc(rxsize+1);memset(rxbuf,0,rxsize+1);char* txbuf=(char*)malloc(txsize+1);memset(txbuf,0,txsize+1);
 

  while (1)
  {
    struct sockaddr_in destAddr;
    destAddr.sin_addr.s_addr = htonl(INADDR_ANY); // Change hostname to network byte order
    destAddr.sin_family = AF_INET;                // Define address family as Ipv4
    destAddr.sin_port = htons(80);                // htons(PORT); 	//Define PORT
    addr_family = AF_INET;                        // Define address family as Ipv4
    ip_protocol = IPPROTO_TCP;                    // Define protocol as TCP
    inet_ntoa_r(destAddr.sin_addr, addr_str, sizeof(addr_str) - 1);

    /* Create TCP socket*/
    socket_id = socket(addr_family, SOCK_STREAM, ip_protocol);
    if (socket_id < 0)
    {
      ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
      break;
    }
    ESP_LOGI(TAG, "Socket created");

    //Привязка: Bind a socket to a specific IP + port
    bind_err = bind(socket_id, (struct sockaddr *)&destAddr, sizeof(destAddr));
    if (bind_err != 0)
    {
      ESP_LOGE(TAG, "Socket unable to bind: errno %d", errno);
      break;
    }
    ESP_LOGI(TAG, "Socket binded");

    /* Begin listening for clients on socket */
    listen_error = listen(socket_id, 3);
    if (listen_error != 0)
    {
      ESP_LOGE(TAG, "Error occured during listen: errno %d", errno);
      break;
    }
    ESP_LOGI(TAG, "Socket listening");

    while (1)
    {
      struct sockaddr_in sourceAddr; // Large enough for IPv4
      uint addrLen = sizeof(sourceAddr);
      /* Accept connection to incoming client */
      client_socket = accept(socket_id, (struct sockaddr *)&sourceAddr, &addrLen);
      if (client_socket < 0)
      {
        ESP_LOGE(TAG, "Unable to accept connection: errno %d", errno);
        break;
      }
      ESP_LOGI(TAG, "Socket accepted");

      int val = 1;
      if(setsockopt(client_socket, SOL_SOCKET, SO_KEEPALIVE, (char*)&val, sizeof(int)) == ESP_OK) {
        if(setsockopt(client_socket, IPPROTO_TCP, TCP_NODELAY, (char*)&val, sizeof(int)) != ESP_OK)
            ESP_LOGE("server", "TCP_NODELAY fail !!!");
      }
      else ESP_LOGE("server", "SO_KEEPALIVE fail !!!");


      // Optionally set O_NONBLOCK
      // If O_NONBLOCK is set then recv() will return, otherwise it will stall until data is received or the connection is lost.
      // fcntl(client_socket,F_SETFL,O_NONBLOCK);

      // Clear rx_buffer, and fill with zero's
      // bzero(rx_buffer, sizeof(rx_buffer));
      bzero(rxbuf, rxsize+1);
      vTaskDelay(100 / portTICK_PERIOD_MS);
      int rec=0;
      int ctr=100;
      bool w=true;
      while (1)
      {
        //if(w)ESP_LOGI(TAG, "Waiting received for data");
        bytes_received = recv(client_socket, rxbuf, rxsize, 0);
        //if(w)ESP_LOGI(TAG, "Received Data=%d",bytes_received);
        w=false;
        // Error occured during receiving
        if (bytes_received < 0)
        {
          // if(ctr<=0){
          //   ESP_LOGI(TAG, "Waiting for data , received=%d",bytes_received);
          //   ctr=100;
          // }
          // else ctr--;
          vTaskDelay(100 / portTICK_PERIOD_MS);
        }
        // Connection closed
        else if (bytes_received == 0)
        {
          ESP_LOGI(TAG, "Connection closed");
          w=true;
          break;
        }
        // Data received
        else
        {
          //Получить IP-адрес отправителя в виде строки
          // if (sourceAddr.sin_family == PF_INET)
          // {
          //   inet_ntoa_r(((struct sockaddr_in *)&sourceAddr)->sin_addr.s_addr, addr_str, sizeof(addr_str) - 1);
          // }

          //rx_buffer[bytes_received] = 0; // Завершаем нулем все, что мы получили, и обрабатываем как строку
          rxbuf[bytes_received] = 0; // Завершаем нулем все, что мы получили, и обрабатываем как строку
          
          //ESP_LOGI(TAG, "Received %d bytes from %s:", bytes_received, addr_str);
          //ESP_LOGI(TAG, "%s", rx_buffer);

          //esp_task_wdt_reset();
          //END RX,NEED ANSWER
          if(1){//bytes_received<sizeof(rx_buffer) - 1){
            w=true;
            esp_err_t e=syncTX((uint8_t*)rxbuf,bytes_received,(uint8_t*)txbuf,txsize);
            //tx_buffer[128]=0;
          //esp_err_t e=syncTX((uint8_t*)spi_txbuf,readed,(uint8_t*)spi_rxbuf,rxsize);
          //ESP_LOGI(TAG, "txbuf=\"%s\"", txbuf);
          //int txlen=strlen(txbuf);
          int txlen=strlen(txbuf);
          int snd=0;
          if(txlen)//client.write(txbuf,txlen);
            snd=send(client_socket, (uint8_t*)txbuf,  txlen, 0);

            //int err;
            //int snd=send(client_socket, "200", strlen("200"), 0);
            //ESP_LOGW(TAG, "send=%d", snd);
            // if(err){
            //   ESP_LOGE(TAG, "send ERROR %d", err);
            // }
          }

          // char* ssid  = getTagValue(rx_buffer, "ssid");
          // char* password  = getTagValue(rx_buffer, "password");

          // if(ssid && password)
          // {
          // 	ESP_LOGI(TAG, "SSID: %s", ssid);
          // 	ESP_LOGI(TAG, "PASSWORD: %s", password);
          // 	wifi_init_sta(ssid, password);
          // }

          // Clear rx_buffer, and fill with zero's
          bzero(rxbuf, rxsize+1);
        }
      }
    }
  }
  vTaskDelete(NULL);
}
//=========================================================================================================//
char *getTagValue(char *a_tag_list, char *a_tag)
{
  /* 'strtok' modifies the string. */
  char *tag_list_copy = (char*)malloc(strlen(a_tag_list) + 1);
  char *result = 0;
  char *s;

  strcpy(tag_list_copy, a_tag_list); // original to copy

  s = strtok(tag_list_copy, "&"); // Use delimiter "&"
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
        result = (char*)malloc(strlen(equals_sign) + 1);
        strcpy(result, equals_sign);
      }
    }
    s = strtok(0, "&");
  }
  free(tag_list_copy);
  return result;
}
//=========================================================================================================//
static void WiFiServer_task(void *pvParameters){
  ESP_LOGW(TAG, "start server...");
  server=new WiFiServer(server_ip, 80, 1);
  server->begin();
  WiFiClient client;
  int rxsize=128;//+1 term
  int txsize=128;//+1 term
  char* rxbuf=(char*)malloc(rxsize+1);memset(rxbuf,0,rxsize+1);char* txbuf=(char*)malloc(txsize+1);memset(txbuf,0,txsize+1);
  char* spi_rxbuf=(char*)malloc(rxsize+1);memset(spi_rxbuf,0,rxsize+1);// rx from device
  char* spi_txbuf=(char*)malloc(txsize+1);memset(spi_txbuf,0,rxsize+1);// tx to device

  if(!rxbuf || !txbuf){
    ESP_LOGE(TAG, "!rxbuf || !txbuf");
    while(1){Sleep(100);}
  }
  size_t rxavail=0;
  //while(1);
  while(1){
    esp_task_wdt_reset();
    Sleep(10);
    bool set_opt=false;
    while((client=server->available())){
      if(!set_opt)client.setNoDelay(false);//не объединять мелкие пакеты
      set_opt = true;
      IPAddress remoteIP=client.remoteIP();
      int readed=0;
      memset(txbuf,0,txsize+1);
      while((rxavail=client.available())){
        readed=client.read((uint8_t*)rxbuf,rxsize);
        rxbuf[readed]=0;//terminator
        //ESP_LOGW(TAG, "Received %d bytes from %s:", readed, remoteIP.toString().c_str());
        //ESP_LOGI(TAG, "%s", rxbuf);
        //txbuf=*((char*)"200 OK\r\n");
        if(readed){
          memcpy(spi_txbuf,rxbuf,readed);
          //esp_err_t e=syncTX((uint8_t*)rxbuf,readed,(uint8_t*)txbuf,txsize-1);
          //esp_err_t e=syncTX((uint8_t*)spi_txbuf,readed,(uint8_t*)spi_rxbuf,rxsize);
          //ESP_LOGI(TAG, "txbuf=\"%s\"", txbuf);
          //int txlen=strlen(txbuf);
          int txlen=strlen(spi_rxbuf);
          //if(txlen)client.write(txbuf,txlen);
        }

      }
      // if(client.connected()){
      //   client.write("200 OK\r\n");
      // }
    }
    //no clients
  }
}

