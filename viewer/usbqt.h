/*
 * Relios Mirror: wired (USB) mirroring of an iPad / iPhone on Windows. See usbqt.c.
 */
#ifndef RELIOS_USBQT_H
#define RELIOS_USBQT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum usbqt_state {
	USBQT_OFF = 0,
	USBQT_NO_DEVICE,     /* no iPad / iPhone on USB */
	USBQT_NO_DRIVER,     /* one is on USB, without the USB driver (Settings > USB > Install) */
	USBQT_DRIVER_FAILED, /* the driver is installed for it, but Windows reports a problem with the device */
	USBQT_REPLUG,        /* the driver is installed, not loaded yet: unplug and plug the device */
	USBQT_STARTING,      /* switching the device to screen capture */
	USBQT_LOCKED,        /* no answer: the device should be unlocked and trust this computer */
	USBQT_STREAMING,
	USBQT_ERROR,         /* a session failed; it is tried again */
};

struct usbqt_callbacks {
	/* a picture, Annex B, followed by 64 zero bytes; the size the device reports (0 if it did not) */
	void (*video)(const uint8_t *annexb, size_t len, bool h265, int width, int height);
	/* sound: 16-bit little-endian, interleaved */
	void (*audio)(const uint8_t *pcm, int frames, int channels, int rate);
	void (*state)(enum usbqt_state state);
	void (*log)(const char *text);
};

/* true: running. false: could not start, or the previous run is still giving the device back (try again later) */
bool usbqt_start(const struct usbqt_callbacks *cb);
/* wait: until the device is given back (at most ~12 s); otherwise that happens in the background */
void usbqt_stop(bool wait);
bool usbqt_running(void);
bool usbqt_streaming(void);

struct usbqt_device {
	char instance[200]; /* USB\VID_05AC&PID_xxxx\<serial> */
	char name[128];
	bool filtered;      /* "libusb0" is among its upper filters */
	unsigned problem;   /* the problem Windows reports for it (Device Manager's code; 0: none) */
};

/* the first iPad / iPhone on USB in Windows' device list (no USB traffic) */
bool usbqt_find_device(struct usbqt_device *out);

/* whether that process is one of this program's short USB helpers (never to be counted or ended as a copy) */
bool usbqt_is_helper(unsigned long pid);

/* the helper: "--usb-switch activate|restore <serial> <product id, hex>"; returns its exit code */
int usbqt_switch_main(int argc, char **argv);

/* the exit codes of the driver installer, relios-usb-kurucu.exe (usbdriver.c): "install|remove <device instance>" */
enum {
	USBQT_DRIVER_DONE = 0,
	USBQT_DRIVER_ARGUMENTS = 1,
	USBQT_DRIVER_NO_DEVICE = 2,
	USBQT_DRIVER_NO_FILES = 3,      /* the program's usb folder lacks the driver's files */
	USBQT_DRIVER_BAD_FILES = 4,     /* their fingerprints are not the expected ones: nothing was installed */
	USBQT_DRIVER_OTHER_VERSION = 5, /* another libusb0 is installed: left as it is, nothing was installed */
	USBQT_DRIVER_COPY = 6,
	USBQT_DRIVER_SERVICE = 7,
	USBQT_DRIVER_REGISTRY = 8,
	USBQT_DRIVER_DEVICE_PROBLEM = 9, /* Windows already reports a problem with the device: nothing was installed */
	USBQT_DRIVER_NOT_LOADED = 10,   /* Windows did not start the device with the driver: everything was undone */
	USBQT_DRIVER_KEPT = 11,         /* removed from the device; the driver stays (another device uses it, or it was
					 * not installed by Relios Mirror) */
	USBQT_DRIVER_REBOOT = 12,       /* removed; the driver's files go at the next restart of Windows */
	USBQT_DRIVER_RESTART_FIRST = 13, /* a removal is still waiting for a restart of Windows: nothing was done */
};

#endif
