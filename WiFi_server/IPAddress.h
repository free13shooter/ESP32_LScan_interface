/*
  Server.h - Server class for Raspberry Pi
  Copyright (c) 2016 Hristo Gochkov  All right reserved.

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 2.1 of the License, or (at your option) any later version.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*/
#ifndef _IPADDRESS_H
#define _IPADDRESS_H

//#include "Arduino.h"
//#include "Server.h"
//#include <server.h>
//#include "WiFiClient.h"
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
#include "lwip/opt.h"
#include "lwip/def.h"
#include "arpa/inet.h"
#include <string>
#include <memory>

using namespace std;

class IPAddress //: public Printable
{
private:
    union {
        uint8_t bytes[4];  // IPv4 address
        uint32_t dword;
    } _address;

    // Access the raw byte array containing the address.  Because this returns a pointer
    // to the internal structure rather than a copy of the address this function should only
    // be used when you know that the usage of the returned uint8_t* will be transient and not
    // stored.
    uint8_t* raw_address()
    {
        return _address.bytes;
    }

public:
    // Constructors
    IPAddress();
    IPAddress(uint8_t first_octet, uint8_t second_octet, uint8_t third_octet, uint8_t fourth_octet);
    IPAddress(uint32_t address);
    IPAddress(const uint8_t *address);
    virtual ~IPAddress() {}

    bool fromString(const char *address);
    bool fromString(const string &address) { return fromString(address.c_str()); }

    // Overloaded cast operator to allow IPAddress objects to be used where a pointer
    // to a four-byte uint8_t array is expected
    operator uint32_t() const
    {
        return _address.dword;
    }
    bool operator==(const IPAddress& addr) const
    {
        return _address.dword == addr._address.dword;
    }
    bool operator==(const uint8_t* addr) const;

    // Overloaded index operator to allow getting and setting individual octets of the address
    uint8_t operator[](int index) const
    {
        return _address.bytes[index];
    }
    uint8_t& operator[](int index)
    {
        return _address.bytes[index];
    }

    // Overloaded copy operators to allow initialisation of IPAddress objects from other types
    IPAddress& operator=(const uint8_t *address);
    IPAddress& operator=(uint32_t address);

    size_t print() const;
    string toString() const;

    friend class WiFiServer;
    friend class Server;
    friend class WiFiClient;
    friend class Client;
};

//const IPAddress IP_ADDRNONE(0,0,0,0);
//const IPAddress INADDR_NONE(0, 0, 0, 0);

#endif /* _IPADDRESS_H */
