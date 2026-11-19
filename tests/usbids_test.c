/*
 * Tests which USB device instances wired mirroring may touch (viewer/usbids.h): the program passes them to the
 * driver installer, which runs as administrator, so only an iPad's / iPhone's own instance (or one of its interfaces)
 * may pass. Known good and bad ones, then random strings.
 *
 *   cc -std=c11 -Wall -Wextra -fsanitize=address,undefined -Iviewer tests/usbids_test.c -o usbids_test
 */
#include "usbids.h"

#include <stdint.h>
#include <stdio.h>

static int failures, checks;

static void expect(bool got, bool want, const char *what, const char *s)
{
	checks++;
	if (got != want) {
		failures++;
		printf("FAIL: %s %s \"%s\"\n", what, want ? "refused" : "accepted", s);
	}
}

int main(void)
{
	/* an iPad's own instance: USB\VID_05AC&PID_xxxx\<serial> */
	static const char *const good[] = {
		"USB\\VID_05AC&PID_12AB\\00008112000A1B2C3D4E5F60",
		"USB\\VID_05AC&PID_12A8\\0123456",
		"USB\\VID_05AC&PID_1290\\ABCDEF0123456789ABCDEF0123456789ABCDEF01",
		"USB\\VID_05AC&PID_12AB\\0000-8112-000A",
	};
	static const char *const bad[] = {
		"",
		"USB\\VID_05AC&PID_12AB\\",
		"USB\\VID_05AC&PID_12AB\\012345",                                     /* serial too short */
		"USB\\VID_05AC&PID_12AB\\ABCDEF0123456789ABCDEF0123456789ABCDEF012",   /* too long */
		"USB\\VID_05AD&PID_12AB\\00008112000A1B2C",                            /* not Apple */
		"USB\\VID_05AC&PID_1281\\00008112000A1B2C",                            /* recovery mode */
		"USB\\VID_05AC&PID_12AB&MI_02\\6&1323831A&2&0002",                     /* an interface */
		"USB\\VID_05AC&PID_12AB\\0000..\\..\\WINDOWS",                         /* path characters */
		"USB\\VID_05AC&PID_12AB\\-00081120",                                   /* starts with a dash */
		"USB\\VID_05AC&PID_12AB\\00008112 000A",                               /* a space */
		"USB\\VID_05AC&PID_12AB\\00008112000A\"",                              /* a quote */
		"USB\\VID_05AC&PID_12GB\\00008112000A1B2C",                            /* not hex */
		"usb\\vid_05ac&pid_12ab\\00008112000a1b2c",                            /* (callers make it upper case) */
		"USB\\VID_05AC&PID_12AB\\00008112000a1b2c",                            /* lower case */
		"USB\\VID_05AC&PID_12ab\\00008112000A1B2C",
		"USB/VID_05AC&PID_12AB/00008112000A1B2C",
		"PCI\\VID_05AC&PID_12AB\\00008112000A1B2C",
	};
	for (size_t i = 0; i < sizeof(good) / sizeof(good[0]); i++)
		expect(valid_instance(good[i]), true, "device", good[i]);
	for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
		expect(valid_instance(bad[i]), false, "device", bad[i]);

	/* one of its interfaces: USB\VID_05AC&PID_xxxx&MI_nn\<Windows' id: hex and '&'> */
	static const char *const good_if[] = {
		"USB\\VID_05AC&PID_12AB&MI_02\\6&1323831A&2&0002",
		"USB\\VID_05AC&PID_12A8&MI_01\\7&ABC",
	};
	static const char *const bad_if[] = {
		"USB\\VID_05AC&PID_12AB&MI_02\\6",                        /* too short */
		"USB\\VID_05AC&PID_12AB&MI_02\\6&1323831a&2&0002",        /* lower case */
		"USB\\VID_05AC&PID_12AB\\00008112000A1B2C",               /* the device itself */
		"USB\\VID_05AC&PID_12AB&MI_0G\\6&1323831A&2&0002",        /* not hex */
		"USB\\VID_05AC&PID_12AB&MI_02\\6&1323831A&2&0002\\..\\X", /* path characters */
		"USB\\VID_05AC&PID_12AB&MI_02\\6&1323831A&2&0002 ",       /* a space */
		"USB\\VID_05AC&PID_1281&MI_02\\6&1323831A&2&0002",        /* recovery mode */
		"USB\\VID_046D&PID_12AB&MI_02\\6&1323831A&2&0002",        /* not Apple */
		"USB\\VID_05AC&PID_12AB&MI_02\\6&1323831A&2&0002&0002&0002&0002&0002&00", /* too long */
	};
	for (size_t i = 0; i < sizeof(good_if) / sizeof(good_if[0]); i++)
		expect(valid_interface_instance(good_if[i]), true, "interface", good_if[i]);
	for (size_t i = 0; i < sizeof(bad_if) / sizeof(bad_if[0]); i++)
		expect(valid_interface_instance(bad_if[i]), false, "interface", bad_if[i]);

	/* random strings around the valid shape: nothing that passes may hold a character outside the allowed ones */
	uint64_t r = 88172645463325252ull;
	char s[96];
	for (int it = 0; it < 2000000; it++) {
		const char *base = (it & 1) ? good[it % 4] : good_if[it % 2];
		size_t n = strlen(base);
		memcpy(s, base, n + 1);
		int changes = 1 + (int)(r % 3);
		for (int c = 0; c < changes; c++) {
			r ^= r << 13, r ^= r >> 7, r ^= r << 17;
			size_t at = (size_t)(r % (n + 1));
			if (at < n)
				s[at] = (char)(r >> 32);
			else if (n + 1 < sizeof(s)) {
				s[n] = (char)(r >> 40);
				s[++n] = '\0';
			}
		}
		s[sizeof(s) - 1] = '\0';
		bool dev = valid_instance(s), itf = valid_interface_instance(s);
		checks++;
		if (dev && itf) {
			failures++;
			printf("FAIL: both a device and an interface: \"%s\"\n", s);
		}
		if (dev || itf) {
			for (const char *p = s; *p; p++) {
				unsigned char ch = (unsigned char)*p;
				bool allowed = (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') || ch == '\\' || ch == '&' ||
					       ch == '_' || ch == '-';
				if (!allowed) {
					failures++;
					printf("FAIL: accepted with character 0x%02x\n", ch);
					break;
				}
			}
		}
	}
	printf("%d check(s), %d failure(s)\n", checks, failures);
	return failures ? 1 : 0;
}
