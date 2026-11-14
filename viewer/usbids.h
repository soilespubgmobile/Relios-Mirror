/*
 * Relios Mirror: which USB devices wired mirroring may touch (shared by usbqt.c and usbdriver.c).
 */
#ifndef RELIOS_USBIDS_H
#define RELIOS_USBIDS_H

#include <ctype.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/* the product IDs of iPhones, iPads and iPods (Apple's driver list; as in iPhoneMirror). Not Apple TV, Watch,
 * HomePod, adapters, nor a device in recovery or DFU mode */
static inline bool apple_mobile_product(unsigned pid)
{
	static const unsigned short ids[] = {0x1290, 0x1291, 0x1292, 0x1293, 0x1294, 0x1297, 0x1299, 0x129A,
					     0x129C, 0x129D, 0x129E, 0x129F, 0x12A0, 0x12A1, 0x12A2, 0x12A3,
					     0x12A4, 0x12A5, 0x12A6, 0x12A8, 0x12A9, 0x12AA, 0x12AB, 0x12AC};
	for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
		if (ids[i] == pid)
			return true;
	}
	return false;
}

/* an iPad's / iPhone's own device instance (upper case ASCII): USB\VID_05AC&PID_xxxx\<serial>, the serial made of
 * letters, digits and dashes (not one of its interfaces: those have &MI_xx) */
static inline bool valid_instance(const char *a)
{
	size_t n = strlen(a);
	if (strncmp(a, "USB\\VID_05AC&PID_", 17) || n < 22 + 7 || n > 22 + 40 || a[21] != '\\')
		return false;
	for (int i = 17; i < 21; i++) {
		if (!isxdigit((unsigned char)a[i]))
			return false;
	}
	char hex[5] = {a[17], a[18], a[19], a[20], 0};
	if (!apple_mobile_product((unsigned)strtoul(hex, NULL, 16)) || !isalnum((unsigned char)a[22]))
		return false;
	for (const char *p = a + 22; *p; p++) {
		if (!isalnum((unsigned char)*p) && *p != '-')
			return false;
	}
	return true;
}


/* The device Windows' composite driver makes for one interface of an iPad / iPhone:
 * USB\VID_05AC&PID_xxxx&MI_nn\<id> (the id Windows gives it: hex digits and '&') */
static inline bool valid_interface_instance(const char *a)
{
	size_t n = strlen(a);
	if (strncmp(a, "USB\\VID_05AC&PID_", 17) || n < 27 + 3 || n > 27 + 40 || strncmp(a + 21, "&MI_", 4) ||
	    a[27] != '\\')
		return false;
	for (int i = 17; i < 27; i++) {
		if (i >= 21 && i < 25)
			continue;
		if (!isxdigit((unsigned char)a[i]))
			return false;
	}
	char hex[5] = {a[17], a[18], a[19], a[20], 0};
	if (!apple_mobile_product((unsigned)strtoul(hex, NULL, 16)))
		return false;
	for (const char *p = a + 28; *p; p++) {
		if (!isxdigit((unsigned char)*p) && *p != '&')
			return false;
	}
	return true;
}

#endif
