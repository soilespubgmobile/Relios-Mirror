/*
 * RELIOS: who may talk to the receiver (ReliosOBS's addition to UxPlay's library, LGPL-2.1 like it).
 *
 * The receiver only answers devices on the local network: private IPv4 (10/8, 172.16/12, 192.168/16),
 * IPv4 link-local (169.254/16), IPv6 unique local (fc00::/7) and link-local (fe80::/10) addresses.
 * Loopback is refused too, except in the receiver's own tests (relios_guard_allow_loopback).
 */
#ifndef RELIOS_GUARD_H
#define RELIOS_GUARD_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void relios_guard_allow_loopback(bool allow);

/* addr: the 4 (IPv4) or 16 (IPv6) address bytes, as netutils_get_address() returns them
 * (an IPv4-mapped IPv6 address comes as its 4 IPv4 bytes) */
bool relios_guard_local_address(const unsigned char *addr, int len);

/* the same host, by IP address only (ports may differ); sockaddr_a/b are struct sockaddr_storage */
bool relios_guard_same_host(void *sockaddr_a, void *sockaddr_b);

#ifdef __cplusplus
}
#endif

#endif
