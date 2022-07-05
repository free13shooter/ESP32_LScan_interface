idf_component_register(SRCS "main.cpp" "functions.cpp" "../spi_drv/spi_drv.cpp" "../WiFi_server/IPAddress.cpp" 
"../WiFi_server/WiFiClient.cpp" "../WiFi_server/WiFiServer.cpp" "../WiFi_server/AP_server.cpp" 
INCLUDE_DIRS "." ".."  "../spi_drv" "../WiFi_server")