/* ReliosOBS: libplist's build settings for the receiver (MinGW-w64 and the Linux check build) */
#define PACKAGE_VERSION "2.7.0-3cfe224"
#define PACKAGE_STRING "libplist 2.7.0-3cfe224"
/* (MinGW-w64's string.h declares strndup too) */
#define HAVE_STRNDUP 1
#ifndef _WIN32
#define HAVE_GMTIME_R 1
#define HAVE_LOCALTIME_R 1
#define HAVE_TIMEGM 1
#define HAVE_MEMMEM 1
#define HAVE_STRPTIME 1
#endif
