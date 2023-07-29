#include "spi_drv.hpp"
//------------------------------------------------------------------------------------------------------
volatile uint32_t queue_cntr = 0; // queue counter
volatile uint32_t sampleLen = 64; //длина пакета передачи/приема
volatile uint32_t CRC_retry = 2;  //повторить передачу CRC_retry раз при ошибке CRC
//-----------------
static xQueueHandle rdySem; // The semaphore indicating the slave is ready to receive stuff.
volatile spi_device_handle_t spi = NULL;
//------------------------------------------------------------------
INTERRUPT_ATTR void scan_keys();
IRAM_ATTR void transaction_post_cb(spi_transaction_t *trans);
IRAM_ATTR void transaction_post_sync_cb(spi_transaction_t *trans);
//------------------------------------------------------------------
static char buf65[65] = {0};
static char _temp_buf65[65] = {0};
volatile esp_err_t ite = 0;

volatile int spi_stream_mode=0;
//____________________________________________________________________________________________________________//
//вернет тектовое описание ошибки
string get_errstring(esp_err_t error) { return string(esp_err_to_name_r(error, buf65, 64)); }
//____________________________________________________________________________________________________________//
spi_bus_config_t buscfg{
    .mosi_io_num = SPI_MOSI, ///< GPIO pin for Master Out Slave In (=spi_d) signal, or -1 if not used.
    .miso_io_num = SPI_MISO, ///< GPIO pin for Master In Slave Out (=spi_q) signal, or -1 if not used.
    .sclk_io_num = SPI_SCK,  ///< GPIO pin for SPI Clock signal, or -1 if not used.
    .quadwp_io_num = -1,
    .quadhd_io_num = -1,
    .data4_io_num = -1, ///< GPIO pin for spi data4 signal in octal mode, or -1 if not used.
    .data5_io_num = -1, ///< GPIO pin for spi data5 signal in octal mode, or -1 if not used.
    .data6_io_num = -1, ///< GPIO pin for spi data6 signal in octal mode, or -1 if not used.
    .data7_io_num = -1, ///< GPIO pin for spi data7 signal in octal mode, or -1 if not used.
    .max_transfer_sz = SPI_DMA_MODE>0?10240:SOC_SPI_MAXIMUM_BUFFER_SIZE, //в байтах
    .flags = SPICOMMON_BUSFLAG_MASTER,// | SPICOMMON_BUSFLAG_MOSI,// SPICOMMON_BUSFLAG_IOMUX_PINS, // SPICOMMON_BUSFLAG_GPIO_PINS//|SPICOMMON_BUSFLAG_SCLK|SPICOMMON_BUSFLAG_MISO|SPICOMMON_BUSFLAG_MOSI|SPICOMMON_BUSFLAG_DUAL//(
    .intr_flags = 0};
// CPOL = 0 — исходное состояние сигнала синхронизации -- низкий уровень;
// CPOL = 1 — исходное состояние сигнала синхронизации -- высокий уровень;
// CPHA = 0 — выборка данных производится по переднему фронту(переключению) сигнала синхронизации. То есть по переключению из основного в противоположное ему;
// CPHA = 1 — выборка данных производится по заднему фронту(переключению)сигнала синхронизации. То есть по переключению обратно к основному из противоположного;
spi_device_interface_config_t dev_cfg_master{
    .command_bits = 0, .address_bits = 0, .dummy_bits = 0,
    .mode = 0,                  // SPI mode, r(CPOL, CPHA) : 0: (0, 0),  1: (0, 1), 2: (1, 0), 3: (1, 1)
    .duty_cycle_pos = 0,        // 128,       // Duty cycle of positive clock, in 1/256th increments (128 = 50%/50% duty). Setting this to 0 (=not setting it) is equivalent to setting this to 128.
    .cs_ena_pretrans = 0,       // Amount of SPI bit-cycles the cs should be activated before the transmission (0-16). This only works on half-duplex transactions.
    .cs_ena_posttrans = 0,      // 2,       // Amount of SPI bit-cycles the cs should stay active after the transmission (0-16)
    .clock_speed_hz = SPI_FREQ, // SPI_MASTER_FREQ_20M,             ///Clock speed, divisors of 80MHz, in Hz. See ``SPI_MASTER_FREQ_*``.
    .input_delay_ns = 0,        // 1,        //задержка готовности SLAVE MISO, необходима между SCLK и MISO ( 0 - default )
#ifdef SPI_CS
    .spics_io_num = SPI_CS, // CS GPIO pin for this device, or -1 if not used
#else
    .spics_io_num = -1, // CS GPIO pin for this device, or -1 if not used
#endif
    .flags = 0,//FULL DUPLEX ONLY !!! SPI_DEVICE_HALFDUPLEX,//|SPI_DEVICE_NO_DUMMY, // 0,             //Bitwise OR of SPI_DEVICE_* flags
    .queue_size = MAX_QUEUE,        //сколько транзакций может быть поставлено в очередь с помощью spi_device_queue_trans, но не завершено с помощью spi_device_get_trans_result)
    .pre_cb =   NULL,                // IT pre-callback
    .post_cb =  NULL                 // IT post-callback
};
//------------------------------------------------------------------------------------------------------
// * * * * * * * * * * * * * * * * * * * INTERRUPT * * * * * * * * * * * * * * * * * * * * * * * * * * *
//------------------------------------------------------------------------------------------------------
/*
This ISR is called when the handshake line goes high.
*/
static void IRAM_ATTR gpio_handshake_isr_handler(void *arg)
{
  //Иногда из-за помех или звонка или чего-то еще мы получаем два прерывания друг за другом.
  // Cмотрим время между прерываниями и отклоняем любое прерывание слишком близко к другому.
  // static uint32_t lasthandshaketime;
  //uint32_t currtime = esp_cpu_get_ccount();
  // uint32_t diff=currtime-lasthandshaketime;
  // if (diff<240000) return; //240000 - ignore everything <1ms after an earlier irq
  // lasthandshaketime=currtime;

  // Give the semaphore.
  BaseType_t mustYield = false;
  xSemaphoreGiveFromISR(rdySem, &mustYield);
  if (mustYield) portYIELD_FROM_ISR();//отдать время другому потоку
}
//____________________________________________________________________________________________________________//
//прерывание обратного вызова для завершения транзакции из очереди
IRAM_ATTR void transaction_post_cb(spi_transaction_t *t)
{
  if (!spi)
  {
    ite = ESP_FAIL;
    return;
  }
  ite = spi_device_get_trans_result(spi, &t, 0);
  queue_cntr--;
  free(t);
}
//--------------------------------------------------------
//прерывание обратного вызова для завершения синхронной транзакции
IRAM_ATTR void transaction_post_sync_cb(spi_transaction_t *t)
{
  if (!spi)
  {
    ite = ESP_FAIL;
    return;
  }
  ite = spi_device_get_trans_result(spi, &t, 0);
}
//____________________________________________________________________________________________________________//
//асинхронная передача
esp_err_t asyncTXRX(uint8_t *txbuf, uint32_t txlen, uint8_t *rxbuf, uint32_t rxlen)
{

  esp_err_t e = ESP_OK;

  if (0)
  { // queue_cntr>=MAX_QUEUE){ //проверка, сколько в очереди
    TickType_t c = xTaskGetTickCount();
    TickType_t cn;
    while (queue_cntr >= MAX_QUEUE && (cn = xTaskGetTickCount()) - c <= TICKS_WAIT)
      ;
    if (queue_cntr >= MAX_QUEUE && (cn = xTaskGetTickCount()) - c > TICKS_WAIT)
    {
      printf("asyncTXRX ERROR (wait query): ESP_ERR_TIMEOUT, queue=%d\n", queue_cntr);
      return ESP_ERR_TIMEOUT;
    }
  }

  spi_transaction_t *qt = (spi_transaction_t *)malloc(sizeof(spi_transaction_t));
  qt->flags = 0; // normal spi mode
  qt->cmd = 0;
  qt->addr = 0;
  qt->length = txlen * 8;   // in bits
  qt->rxlength = rxlen * 8; // in bits
  qt->tx_buffer = txbuf;
  qt->rx_buffer = rxbuf;

  dev_cfg_master.post_cb = transaction_post_cb;
  if (!spi)
  {
    e = spi_bus_add_device(SPIH, &dev_cfg_master, (spi_device_handle_t *)&spi);
    if (e)
      printf("asyncTXRX ERROR (add spi dev): %s, queue=%d\n", string(esp_err_to_name_r(e, buf65, 64)).c_str(), queue_cntr);
    return e;
  }

  queue_cntr++;
  e = spi_device_queue_trans(spi, qt, TICKS_WAIT);

  if (e)
  {
    --queue_cntr;
    // println("asyncTXRX ERROR : "+string(esp_err_to_name_r(e,buf65,64))+", queue="+string(queue_cntr)+"\n");
  }
  else
    printf(" * * * asyncTXRX SUCCESS, queue=%d\n", queue_cntr);
  return e;
}
//____________________________________________________________________________________________________________//
//синхронная передача любой размер. rxlen-длина приемного буфера.передача/приём по очереди(полудуплекс)
esp_err_t syncTXRXlong(uint8_t *txbuf, uint32_t txlen, uint8_t *rxbuf, uint32_t rxlen, uint32_t *pRxTotal, TickType_t wait)
{

  esp_err_t e = ESP_OK;
  ite = e;

  if (rxbuf && rxlen)
    memset(rxbuf, 0, rxlen);

  int total = txlen;
  int len = 0;
  int packet = 0;
  for (uint8_t *p = txbuf; txbuf && p < txbuf + txlen;)
  {
    len = total > sampleLen ? sampleLen : total;
    total -= len;
    esp_err_t te = 0;
    // if(rxbuf && rxlen && !total)//last
    //   e=syncTXRX(p,len,rxbuf,rxlen,pRxTotal, wait);
    // else
    e = syncTXRX(p, len, 0, 0, NULL, wait);
    p += len;
    packet++;

    if (te)
    {
      // printf(" * ERROR : %s\n",get_errstring(e).c_str());
      if (!e)
        e = te;
    }
  }

  if (rxbuf && rxlen)
  {
    uint32_t count = 0;
    total = rxlen;
    int len = 0;
    for (uint8_t *p = rxbuf; p < rxbuf + rxlen;)
    {
      len = total > sampleLen ? sampleLen : total;
      esp_err_t te = 0;
      te = syncTXRX(0, 0, p, len, &count, wait);
      if (!count)
        break;
      if (pRxTotal)
        *pRxTotal += count;
      total -= count;
      p += count;
      count = 0;
      packet++;

      if (te)
      {
        // printf(" * ERROR : %s\n",get_errstring(e).c_str());
        if (!e)
          e = te;
      }
    }

    // e=syncTXRX(0,0,rxbuf,rxlen, wait);//только читать
    // packet++;
  }

  return e;
  /*
  uint32_t len=txlen;uint32_t rlen=rxlen;
  uint32_t _txlen=txlen;
  uint32_t _rxlen=rxbuf?rxlen:0;

  if(rlen>len){
    //print("syncTX WARNING: txbuf(%d)<rxbuf(%d), make rxlen=txlen.",len,rlen);
    _rxlen=rlen=len;
  }

  for(int i=0;i<len;i+=sampleLen){
    esp_task_wdt_reset();
    //printf("syncTX start txlen=%d rxlen=%d i=%d\n",txlen,rxlen,i);
    e=syncTXRX(_txlen?&txbuf[i]:NULL, _txlen>sampleLen?sampleLen:_txlen, _rxlen && i<rlen?&rxbuf[i]:NULL, _rxlen>sampleLen?sampleLen:_rxlen, wait);
    //printf("syncTX end\n");
    if(e || ite)break;
    _txlen-=(sampleLen>_txlen?_txlen:sampleLen);
    _rxlen-=(sampleLen>_rxlen?_rxlen:sampleLen);
  }
  //printf("syncTX off\n");
  if(!e && ite)e=ite;
 //printf("syncTX return\n");
  return e;*/
}
//____________________________________________________________________________________________________________//
//синхронная передача до 64 байтов. rxlen-длина приемного буфера
esp_err_t syncTXRX(uint8_t *txbuf, uint32_t txlen, uint8_t *rxbuf, uint32_t rxlen, uint32_t *pRxTotal, TickType_t wait)
{
  esp_err_t e = ESP_OK;
  static bool acquired = false;
  int qcnt = queue_cntr;

  transaction_cb_t pt = dev_cfg_master.post_cb; // save queue callback

  if (spi && !acquired && qcnt)
  {
    e = spi_device_acquire_bus(spi, portMAX_DELAY); //ограниченное время не поддерживается
    if (e)
    {
      printf("syncTXRX ERROR (acquire bus): %s, queue=%d\n", string(esp_err_to_name_r(e, buf65, 64)).c_str(), queue_cntr);
      return e;
    }
    acquired = true;
  }

  dev_cfg_master.post_cb = NULL;

  if (!spi)
  {
    e = spi_bus_add_device(SPIH, &dev_cfg_master, (spi_device_handle_t *)&spi);
    if (e)
      printf("syncTXRX ERROR (add spi dev): %s, queue=%d\n", string(esp_err_to_name_r(e, buf65, 64)).c_str(), queue_cntr);
    return e;
  }

  // SPI_FREQ/1000;//тактов в 1 мс
  // uint32_t tt=len*8*1000/SPI_FREQ+1;
  // print("syncTXRX LEN (tx "+string(txlen)+" / rx "+string(rxlen)+")="+string(len)+", WAIT= "+string(tt)+"\n");

  if (rxbuf && rxlen)
    memset(rxbuf, 0, rxlen);

  // uint8_t* extRXBuf=NULL;
  // uint8_t* _txb=txbuf;
  // uint32_t _rlen=rxlen;
  // uint32_t _tlen=txlen;
  // uint8_t* _rxb=rxbuf;
  // if(!rxbuf || !rxlen){
  //   extRXBuf=(uint8_t*)malloc(sampleLen);memset(extRXBuf,0,sampleLen);
  //   _rxb=extRXBuf;_rlen=sampleLen;
  // }
  // uint8_t* extTXBuf=NULL;
  // if(txlen<sampleLen){
  //   extTXBuf=(uint8_t*)malloc(sampleLen);memset(extTXBuf,0,sampleLen);memcpy(extTXBuf,txbuf,txlen);
  //   //print(" -- tx copy(len="+string(txlen)+")= \""+string((char*)extTXBuf)+"\" (new len="+string(sampleLen)+")\n");
  //   _txb=extTXBuf;
  //   _tlen=sampleLen;
  // }

  // Wait for slave to be ready for next byte before sending
  if (xSemaphoreTake(rdySem, wait) == pdTRUE)
  { // portMAX_DELAY); //Wait until slave is ready

    spi_transaction_t t = {
        .flags = 0, // normal spi mode
        .cmd = 0,
        .addr = 0,
        // .length=_tlen*8,//in bits !!!
        // .rxlength=_rlen*8,//in bits
        // .tx_buffer=_txb,
        // .rx_buffer=_rxb
        .length = txlen * 8,                          // in bits !!! длина передающего буфера
        .rxlength = (rxbuf && rxlen) ? rxlen * 8 : 0, // sampleLen*8,//in bits длина приемного буфера,не может быть больше передающего. (default 0==length)
        .user=NULL,
        .tx_buffer = (txlen ? txbuf : NULL),
        .rx_buffer = ((rxbuf && rxlen) ? rxbuf : NULL)};

    uint8_t *pTx = NULL;
    uint8_t *pRx = NULL;

    if (t.tx_buffer && t.length < sampleLen * 8)
    {
      t.tx_buffer = pTx = (uint8_t *)malloc(sampleLen);
      if (!pTx)
      {
        printf("syncTXRX CRITICAL ERROR: !malloc pTx buff\n");
        delay(10000);
        esp_restart();
        while (1)
          ;
      }
      t.length = sampleLen * 8;
      memset(pTx, 0, sampleLen);
      memcpy(pTx, txbuf, txlen);
    }

    if (t.rx_buffer && t.rxlength < sampleLen * 8)
    {
      t.rx_buffer = pRx = (uint8_t *)malloc(sampleLen);
      if (!pRx)
      {
        printf("syncTXRX CRITICAL ERROR: !malloc pRx buff\n");
        delay(10000);
        esp_restart();
        while (1)
          ;
      }
      t.rxlength = sampleLen * 8;
      memset(pRx, 0, sampleLen);
    }

    //printf("syncTXRX :  sampleLen=%d\n", (int)sampleLen);
    // if(!t.rx_buffer || t.rxlength<sampleLen*8){t.rx_buffer=pRx=(uint8_t*)malloc(sampleLen);t.rxlength=sampleLen*8;memset(t.rx_buffer,0,sampleLen);}

    // uint freeRAM = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    // printf("free RAM is %d.", freeRAM);
     //println("syncTXRX :RX BUF="+string(t.rx_buffer)+" , RX LEN="+string(t.rxlength)+" ,TX BUF="+string((int)t.tx_buffer)+" , TX len="+string(t.length));
    // delay(2000);
    // print("TX64 1\n");
    if (!spi || ite == ESP_FAIL)
    {
      printf("-----!!!-----syncTXRX ERROR : SPI null\n");
      e = ESP_FAIL;
      spi = NULL;
    }
    else
    {
      e = spi_device_transmit(spi, &t);
    }

    //повторение, если включено
    // if(!e && t.rx_buffer){
    //   ((char*)t.rx_buffer)[t.rxlength-1]=0;//terminator !!!
    //   if(strstr ((char*)t.rx_buffer,"CRC_ERROR")){
    //     e=ESP_ERR_INVALID_CRC;
    //     if(CRC_retry){
    //       uint32_t rep=CRC_retry;
    //       while(rep){
    //         println("syncTXRX : CRC_ERROR");
    //         rep--;
    //         memset(t.rx_buffer,0,t.rxlength);
    //         e=spi_device_transmit(spi, &t);
    //         if(e)break;
    //         else if(strstr((char*)t.rx_buffer,"CRC_ERROR")){
    //           e=ESP_ERR_INVALID_CRC;
    //           //println("syncTXRX : CRC_ERROR");
    //         }
    //       }
    //     }
    //   }
    // }
    // println("syncTXRX :RX LEN="+string(t.rxlength));

    if (pRx)
    {
      if (pRxTotal)
        *pRxTotal += strlen((char *)pRx);
      size_t rxl = strlen((char *)t.rx_buffer);
      if (rxl > rxlen)
        rxl = rxlen;
      if (rxlen && rxbuf && rxl)
        memcpy(rxbuf, t.rx_buffer, rxl); // rxlen);
      free(pRx);
    }

    if (pTx)
      free(pTx);
  }
  else
  {
    e = ESP_ERR_TIMEOUT;
    // print("syncTXRX ERROR : SEMAPHORE BUSY\n");
  }

  if (acquired && spi)
  {
    dev_cfg_master.post_cb = pt; // restore queue callback
    spi_device_release_bus(spi);
    acquired = false;
  }

  // if(e!=BUSY){
  //   if(e)print("syncTXRX ERROR : "+string(esp_err_to_name_r(e,buf65,64))+", queue="+string(queue_cntr)+"\n");
  //   else print(".");//syncTXRX OK\n");
  // }
  // print("TX64 e\n");
  return e;
}
//____________________________________________________________________________________________________________//
//инициализация
esp_err_t spi_master_config(bool waitREADYstring, bool need_install_GPIO_isr_service)
{
  esp_err_t e;
  printf("-config pin %d as SPI_SCK output.\n", SPI_SCK);
  //gpio_reset_pin((gpio_num_t)SPI_SCK);
  gpioInit(SPI_SCK, GPIO_MODE_OUTPUT);
  gpio_set_level((gpio_num_t)SPI_SCK, 0);
  printf("-config pin %d as SPI_MISO input.\n", SPI_MISO);
 // gpio_reset_pin((gpio_num_t)SPI_MISO);
  gpioInit(SPI_MISO, GPIO_MODE_INPUT,(gpio_pullup_t)0,(gpio_pulldown_t)1);
  printf("-config pin %d as SPI_MOSI output.\n", SPI_MOSI);
  //gpio_reset_pin((gpio_num_t)SPI_MOSI);
  //gpioInit(SPI_MOSI, GPIO_MODE_INPUT_OUTPUT_OD,(gpio_pullup_t)0,(gpio_pulldown_t)0);//GPIO_MODE_OUTPUT);
  gpioInit(SPI_MOSI, GPIO_MODE_OUTPUT,(gpio_pullup_t)0,(gpio_pulldown_t)0);
  gpio_set_level((gpio_num_t)SPI_MOSI, 0);

#ifdef SPI_CS
  printf("-config pin %d as SPI_CS.\n", SPI_CS);
  //gpio_reset_pin((gpio_num_t)SPI_CS);
  gpioInit(SPI_CS, GPIO_MODE_OUTPUT);
  gpio_set_level((gpio_num_t)SPI_CS, 1);
#endif

#ifdef SPI_SOFT_CS
  printf("-config pin %d as SPI_SOFT_CS output.\n", SPI_SOFT_CS);
  //gpio_reset_pin((gpio_num_t)SPI_SOFT_CS);
  gpioInit(SPI_SOFT_CS, GPIO_MODE_OUTPUT);
  gpio_set_level((gpio_num_t)SPI_SOFT_CS, 1);
#endif

  // GPIO config for the handshake line.
  printf("-config pin %d as GPIO_HANDSHAKE input.\n", GPIO_HANDSHAKE);
  gpio_config_t io_conf = {
      .pin_bit_mask = (1 << GPIO_HANDSHAKE),
      .mode = GPIO_MODE_INPUT,
      //.pull_up_en = GPIO_PULLUP_ENABLE,
      .pull_up_en=GPIO_PULLUP_DISABLE,
      //.pull_down_en=GPIO_PULLDOWN_DISABLE,
      .pull_down_en=GPIO_PULLDOWN_ENABLE,
      .intr_type = GPIO_INTR_POSEDGE,
  };

  // Create the semaphore.
  rdySem = xSemaphoreCreateBinary();

  // Set up handshake line interrupt.
  gpio_config(&io_conf);
  if (need_install_GPIO_isr_service) gpio_install_isr_service(0);

  gpio_set_intr_type((gpio_num_t)GPIO_HANDSHAKE, GPIO_INTR_POSEDGE);
  gpio_isr_handler_add((gpio_num_t)GPIO_HANDSHAKE, gpio_handshake_isr_handler, NULL);

  e = spi_bus_initialize(SPIH, &buscfg, SPI_DMA_MODE);
  if (e)
  {
    print("spi_bus_initialize ERROR : %s\n", esp_err_to_name_r(e, buf65, 64));
    delay(1000);
    println("REBOOT...");
    esp_restart();
  }

  printf("\nSPI STARTING, chip ID=%s ===>\n", get_chip_id().c_str());
  //if (waitREADYstring)  e = reinitSlaveOverCS();
  device_spi_get_ready();
  return e;
}
//____________________________________________________________________________________________________________//
void reset_device(){//reboot over cs
  spi_cs_low();delay(5);
  spi_cs_high();delay(5);spi_cs_low();delay(5);
  spi_cs_high();delay(5);spi_cs_low();delay(5);
  spi_cs_high();delay(50);
}
void device_spi_dma_reset(){
  spi_cs_low();delay(5);
  spi_cs_high();delay(5);spi_cs_low();delay(5);
  spi_cs_high();delay(50);
}
esp_err_t device_spi_get_ready(int cyclesMax){
  esp_err_t e=ESP_OK;int v;int n=0;char bf[65]={0};int cyc=cyclesMax;
  if(!cyc)cyc=3;

  // if(cyclesMax>0){
  //   cyc=cyclesMax;
  //   printf("device_spi_get_ready: reboot device over cs...\n");
  //   //spi_cs_high(); delay(500); spi_cs_low(); delay(100);
  //   spi_cs_high(); delay(50); spi_cs_low(); delay(50);
  // }
  // else {
    //spi_cs_high(); delay(100); spi_cs_low(); delay(100);
    //spi_cs_low();delay(10);//spi_cs_high(); delay(10); spi_cs_low(); delay(50);
  //   cyc=10;
  // }

  do{ //test
    spi_cs_low();
    int ct=100;
    while(!gpio_get_level((gpio_num_t)GPIO_HANDSHAKE) && --ct>0){
      delay(10);
    }

    int c=8;//WCMD_PING 
    v=0;
    e=spitx((uint8_t*)&c,4,&v);
    if(e || v==0 || string((char*)&v)!="RDY"){
      n++;
      if(e){
        printf("get status spi [%d] error=%s\n",n,esp_err_to_name_r(e,bf,64));
      }

      if(n%cyc==0){ 
        if(n>cyc){
          printf("ERROR: spitx=%s (%hu):(cycles=%d),reboot...\n",(char*)&v,v,n);
          reset_device();
          esp_restart();
        }
        else{
          printf("ERROR: spitx=%s (%hu):(cycles=%d),reset spi bus...\n",(char*)&v,v,n);
          device_spi_dma_reset();delay(50);
        }
      }
    }
    else {
      printf(">>>[%d] spitx=%s (%hu)\n",n,(char*)&v,v);
      n=0;
      //printf("gpio_handshake_isr_handler=%X\n",(uint)&gpio_handshake_isr_handler);
    }
    spi_cs_high(); 
    delay(10);
  }while(string((char*)&v)!="RDY");
  return ESP_OK;
}
//____________________________________________________________________________________________________________//
esp_err_t reinitSlaveOverCS()
{
  esp_err_t e;
  int attempts = 20;
  do
  {

    memset(buf65, 0, 65);
    sprintf(_temp_buf65, "READY");
    // syncTXRX((uint8_t*)_temp_buf65,64,(uint8_t*)buf65,64,TICKS_WAIT);//SYNC
    for (int i = 0; i < 3; i++)
    {
      e = syncTXRX((uint8_t *)_temp_buf65, 64, (uint8_t *)buf65, 64);
      if (e)
      {
        printf("-spi_master_config transaction ERROR : %s\n", get_errstring(e).c_str());
        delay(10);
      }
      else
      {
        if (!strstr((char *)buf65, "READY"))
        {
          printf("-spi_master_config ERROR : slave device is not ready (%s)\n", buf65);
          e = ESP_FAIL;
          delay(10);
        }
        else
        {
          printf("-MASTER : slave device READY (%s)\n", buf65);
          return 0;
        }
      }
    }

    if (e)
    {
      printf("-ready slave state ERROR : %s, reinit slave CS...\n", get_errstring(e).c_str());
      spi_cs_high();
      delay(50);
      spi_cs_low();
      delay(50);
    }

  } while (e && --attempts);

  return e;
}
//____________________________________________________________________________________________________________//
//синхронная передача 
esp_err_t spitx(uint8_t* txbuf,uint32_t txlen,int* iv,TickType_t wait){
  esp_err_t e =0;//spixfer(txbuf,txlen,NULL,wait);
  //sampleLen=4;
  //int v=0;
  //uint8_t bf[64]={8,1,2,3};uint8_t bf2[64];memset(bf2,0,64);
  e=//syncTXRX(bf,64,bf2,64);//
  spixfer(txbuf,txlen);//,(uint8_t*)&v,4);
  if(e)return e;
  if(spi_stream_mode)return e;
  e =//syncTXRX(bf,64,bf2,64);//
  //syncTXRX((uint8_t*)&v,4,(uint8_t*)iv,4);//
  spixfer(NULL,0,iv,wait);
  return e;
}

esp_err_t spixfer(uint8_t* txbuf,uint32_t txlen,int* iv,TickType_t wait)
{
  esp_err_t e = ESP_OK;//static bool ack=false;

  //dev_cfg_master.pre_cb = NULL;//cb_cs_low;
  //dev_cfg_master.post_cb = NULL;//cb_cs_post;

  if (!spi)
  {
    e = spi_bus_add_device(SPIH, &dev_cfg_master, (spi_device_handle_t *)&spi);
    if (e){
      printf("spixfer ERROR (add spi dev): %s\n", string(esp_err_to_name_r(e, buf65, 64)).c_str());
      spi_cs_high();
      return e;
    }
  }

  //printf("-waiting semaphore,stream_mode=%s...\n",spi_stream_mode?"true":"false");
  int r=0,w=0;//tbuf=melloc(txlen);
  spi_transaction_t t = {
    .flags = 0, // normal spi mode
    .cmd = 0,
    .addr = 0,
    .length = txlen?txlen * 8:32,   // in bits 
    .rxlength = (size_t)(spi_stream_mode?0:32),
    .user=0,
    .tx_buffer = txlen?txbuf:(uint8_t*)&w,
    .rx_buffer = spi_stream_mode?NULL:(iv?(uint8_t*)iv:(uint8_t*)&r)
  };

  if(0){//txlen>4){
    t.rx_buffer=(uint8_t*)malloc(txlen);
    t.rxlength=txlen;
  }

  BaseType_t wt=/*gpio_get_level((gpio_num_t)GPIO_HANDSHAKE)?true:*/xSemaphoreTake(rdySem, wait);//portMAX_DELAY
  // Wait for slave to be ready for next byte before sending
  if(wt == pdTRUE)//Wait until slave is ready
  { 
    
    if (!spi || ite == ESP_FAIL)
    {
      printf("-----!!!-----spixfer ERROR : SPI null\n");
      e = ESP_FAIL;
      spi = NULL;
    }
    else{
      e = spi_device_transmit(spi, &t);  
    }
  }
  else
  {
    e = ESP_ERR_TIMEOUT;
    print("spixfer wait Ready ERROR : TIMEOUT\n");
  }
  
  //if(txlen>4){
  //  free(t.rx_buffer);
  //}
  //spi_cs_high();

  return e;
}
//____________________________________________________________________________________________________________//