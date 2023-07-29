#include "IPAddress.hpp"
//__________________________________________________________________________________________________//

IPAddress::IPAddress()
{
    _address.dword = 0;
}

IPAddress::IPAddress(uint8_t first_octet, uint8_t second_octet, uint8_t third_octet, uint8_t fourth_octet)
{
    _address.bytes[0] = first_octet;
    _address.bytes[1] = second_octet;
    _address.bytes[2] = third_octet;
    _address.bytes[3] = fourth_octet;
}

IPAddress::IPAddress(uint32_t address)
{
    _address.dword = address;
}

IPAddress::IPAddress(const uint8_t *address)
{
    memcpy(_address.bytes, address, sizeof(_address.bytes));
}

IPAddress& IPAddress::operator=(const uint8_t *address)
{
    memcpy(_address.bytes, address, sizeof(_address.bytes));
    return *this;
}

IPAddress& IPAddress::operator=(uint32_t address)
{
    _address.dword = address;
    return *this;
}

bool IPAddress::operator==(const uint8_t* addr) const
{
    return memcmp(addr, _address.bytes, sizeof(_address.bytes)) == 0;
}

size_t IPAddress::print() const
{
    size_t n = 0;
    for(int i = 0; i < 3; i++) {
        n += printf("%d",(int)_address.bytes[i]);
        n += printf(".");
    }
    n += printf("%d",(int)_address.bytes[3]);
    return n;
}

string IPAddress::toString() const
{
    char szRet[16];
    sprintf(szRet,"%u.%u.%u.%u", _address.bytes[0], _address.bytes[1], _address.bytes[2], _address.bytes[3]);
    return string(szRet);
}

bool IPAddress::fromString(const char *address)
{
    // TODO: add support for "a", "a.b", "a.b.c" formats

    uint16_t acc = 0; // Accumulator
    uint8_t dots = 0;

    while (*address)
    {
        char c = *address++;
        if (c >= '0' && c <= '9')
        {
            acc = acc * 10 + (c - '0');
            if (acc > 255) {
                // Value out of [0..255] range
                return false;
            }
        }
        else if (c == '.')
        {
            if (dots == 3) {
                // Too much dots (there must be 3 dots)
                return false;
            }
            _address.bytes[dots++] = acc;
            acc = 0;
        }
        else
        {
            // Invalid char
            return false;
        }
    }

    if (dots != 3) {
        // Too few dots (there must be 3 dots)
        return false;
    }
    _address.bytes[3] = acc;
    return true;
}