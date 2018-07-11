#ifndef _DTUN_UTILS_H_
#define _DTUN_UTILS_H_

#include "DTun/Types.h"
#include "udt.h"

namespace DTun
{
    bool isDebuggerPresent();

    std::string ipToString(UInt32 ipAddress);

    bool stringToIp(const std::string& str, UInt32& ipAddress);

    std::string portToString(UInt16 port);

    std::string ipPortToString(UInt32 ipAddress, UInt16 port);

    void closeSysSocketChecked(SYSSOCKET sock);
    void closeUDTSocketChecked(UDTSOCKET sock);
}

#endif
