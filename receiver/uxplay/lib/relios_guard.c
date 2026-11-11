/*
 * RELIOS: who may talk to the receiver (see relios_guard.h). LGPL-2.1, like the rest of this library.
 */
#include <string.h>

#include "compat.h"
#include "netutils.h"
#include "relios_guard.h"

static bool allow_loopback = false;

void relios_guard_allow_loopback(bool allow)
{
    allow_loopback = allow;
}

bool relios_guard_local_address(const unsigned char *addr, int len)
{
    if (!addr) {
        return false;
    }
    if (len == 4) {
        if (addr[0] == 10) {
            return true;                                    /* 10.0.0.0/8 */
        }
        if (addr[0] == 172 && (addr[1] & 0xf0) == 16) {
            return true;                                    /* 172.16.0.0/12 */
        }
        if (addr[0] == 192 && addr[1] == 168) {
            return true;                                    /* 192.168.0.0/16 */
        }
        if (addr[0] == 169 && addr[1] == 254) {
            return true;                                    /* 169.254.0.0/16 */
        }
        if (addr[0] == 127) {
            return allow_loopback;
        }
        return false;
    }
    if (len == 16) {
        static const unsigned char loopback6[16] = { 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,1 };
        if ((addr[0] & 0xfe) == 0xfc) {
            return true;                                    /* fc00::/7 */
        }
        if (addr[0] == 0xfe && (addr[1] & 0xc0) == 0x80) {
            return true;                                    /* fe80::/10 */
        }
        if (!memcmp(addr, loopback6, 16)) {
            return allow_loopback;
        }
        return false;
    }
    return false;
}

bool relios_guard_same_host(void *sockaddr_a, void *sockaddr_b)
{
    int len_a = 0, len_b = 0;
    unsigned int zone_a = 0, zone_b = 0;
    if (!sockaddr_a || !sockaddr_b) {
        return false;
    }
    const unsigned char *a = netutils_get_address(sockaddr_a, &len_a, &zone_a, NULL);
    const unsigned char *b = netutils_get_address(sockaddr_b, &len_b, &zone_b, NULL);
    if (!a || !b || len_a != len_b || len_a == 0) {
        return false;
    }
    return memcmp(a, b, (size_t) len_a) == 0;
}
