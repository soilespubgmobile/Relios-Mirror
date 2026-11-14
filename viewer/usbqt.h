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
	USBQT_NO_APPLE,      /* Apple's own USB support (Apple Devices or iTunes) is missing, or does not run the device */
	USBQT_APPLE_STOPPED, /* Apple's service (Apple Mobile Device Service) is installed but stopped */
	USBQT_NEED_WINUSB,   /* the capture interface needs Microsoft's WinUSB (Settings > USB); the device is held */
	USBQT_STARTING,      /* switching the device to screen capture */
	USBQT_LOCKED,        /* no answer: the device should be unlocked and trust this computer (given back meanwhile) */
	USBQT_STREAMING,
	USBQT_ERROR,         /* a session failed; it is tried again */
	USBQT_FAILED,        /* failed again and again: tried again only when asked, or when plugged in anew */
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
/* "try again now": ends a wait between attempts (after "Trust" was answered on the device, say) */
void usbqt_retry(void);
/* in USBQT_NEED_WINUSB: the capture interface's device (USB\VID_05AC&PID_xxxx&MI_nn\...) to give WinUSB */
bool usbqt_capture_device(char *out, size_t size);

struct usbqt_device {
	char instance[200]; /* USB\VID_05AC&PID_xxxx\<serial> */
	char name[128];
	char service[48];   /* its driver in Windows: "usbccgp" (Windows' composite driver) when Apple's driver runs it */
	char lower[96];     /* its lower filters (Apple's own sit there), for the log */
	bool filtered;      /* "libusb0" is among its upper filters */
	unsigned problem;   /* the problem Windows reports for it (Device Manager's code; 0: none) */
};

/* the first iPad / iPhone on USB in Windows' device list (no USB traffic) */
bool usbqt_find_device(struct usbqt_device *out);

/* Apple's own support on this computer (Apple Devices from the Microsoft Store, or iTunes): its service, which pairs
 * with the device ("Trust This Computer"). Its driver is what makes Windows use the device's screen capture
 * configuration after the switch; without both, wired mirroring cannot work. (Windows' service list only.) */
enum usbqt_apple {
	USBQT_APPLE_OK,
	USBQT_APPLE_MISSING,
	USBQT_APPLE_NOT_RUNNING, /* installed, set to start with Windows (or switched off), but not running */
};
enum usbqt_apple usbqt_apple_service(void);
/* whether Apple's driver runs the device: Windows' composite driver under Apple's filters (as iPhoneMirror checks).
 * Without it Windows runs the device as a camera for photos only. (An unknown driver is not held against it.) */
bool usbqt_apple_driver(const struct usbqt_device *d);

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
	USBQT_DRIVER_NO_APPLE = 14,      /* Apple's driver does not run the device (install: refused, nothing done) */
	USBQT_DRIVER_REGISTER = 15,      /* the virtual camera was put in place, but Windows did not take it (regsvr32) */
	USBQT_DRIVER_NOT_CAPTURE = 16,   /* winusb-install: that device is not a screen-capture interface (nothing done) */
	USBQT_DRIVER_WINUSB = 17,        /* winusb-install: Windows did not take WinUSB for it (put back as it was) */
	/* winusb-install, the same, by step (log): +1 class, +2 WinUSB's list, +3 not in it, +4 installing, +5 start */
	USBQT_DRIVER_WINUSB_STEP = 170,
};

/* "winusb-install <interface device>": Microsoft's own WinUSB (Windows' winusb.inf, signed by Microsoft) is made
 * the driver of the device Windows' composite driver opens for the iPad's screen-capture interface alone
 * (USB\VID_05AC&PID_xxxx&MI_nn\..., class FF/2A), marked with this interface GUID; Apple's driver keeps the rest of
 * the iPad (pairing). "remove" takes such marked devices away again (Windows then gives them their old driver). */
#define USBQT_CAPTURE_GUID_W L"{5E0A7C3D-2B91-4F68-A1D4-93C7E2B8F610}"
#define USBQT_CAPTURE_GUID_INIT {0x5e0a7c3d, 0x2b91, 0x4f68, {0xa1, 0xd4, 0x93, 0xc7, 0xe2, 0xb8, 0xf6, 0x10}}

/* The installer also puts the virtual camera in place and takes it away ("camera-install" / "camera-remove", no
 * device): the camera's DLL next to it is accepted only with the fingerprint the build wrote into the installer.
 * Exit codes as above: DONE, NO_FILES (no camera in this build), BAD_FILES, COPY, REGISTER, REBOOT (removal). */

#endif
