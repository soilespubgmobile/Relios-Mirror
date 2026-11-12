/*
 * ReliosOBS iPad mirroring: the messages between the receiver program (relios-airplay-receiver.exe) and
 * the OBS source (relios-airplay.dll). Shared by both sides; plain C, no dependencies.
 *
 * The receiver writes messages to its standard output: an 8-byte header and a payload. OBS writes text
 * commands to the receiver's standard input, one per line. The receiver's log lines go to its standard
 * error, as text.
 *
 * Header (little-endian): 'R', 'A', type, 0, payload length (uint32).
 */
#pragma once

#include <stdint.h>

#define RA_MAGIC0 'R'
#define RA_MAGIC1 'A'
#define RA_HEADER_SIZE 8
#define RA_MAX_PAYLOAD (16u * 1024u * 1024u)

enum ra_message_type {
	RA_MSG_STATUS = 1,       /* text: "event" or "event key=value ..." (see below) */
	RA_MSG_PIN = 2,          /* text: the 4-digit PIN to show; empty: hide it */
	RA_MSG_REGISTER = 3,     /* text: "<public key>\t<device id>\t<name>" of a newly paired device */
	RA_MSG_VIDEO_FORMAT = 4, /* struct ra_video_format */
	RA_MSG_VIDEO = 5,        /* struct ra_video_header + Annex B H.264 / H.265 data */
	RA_MSG_AUDIO = 6,        /* struct ra_audio_header + one encoded audio frame */
};

/* Status events: listening | client name=<..> model=<..> | trusted new=<0|1> | mirroring on|off |
 * pairing_failed count=<n> | pairing_off | ended | integrity level=<low|medium|..> | error text=<..> |
 * hidden <0|1> (Relios Mirror: the name was taken off the network while a device mirrors / is announced again) */

#pragma pack(push, 1)
struct ra_video_format {
	uint8_t codec; /* 1 = H.264, 2 = H.265 */
	uint8_t reserved[3];
	uint16_t width_source;
	uint16_t height_source;
	uint16_t x_offset;
	uint16_t y_offset;
	uint16_t width;
	uint16_t height;
};

struct ra_video_header {
	uint64_t ntp_time_remote; /* the iPad's clock, nanoseconds */
	uint64_t ntp_time_local;  /* the same moment on this computer's wall clock (Unix time, nanoseconds) */
	uint64_t received_local;  /* when the receiver got it, wall clock (Unix time, nanoseconds) */
	uint8_t is_h265;
	uint8_t reserved[3];
	int32_t nal_count;
};

struct ra_audio_header {
	uint64_t ntp_time_remote;
	uint64_t ntp_time_local;
	uint64_t received_local;
	uint32_t rtp_time;
	uint16_t seqnum;
	uint8_t ct; /* compression type: 2 = ALAC, 4 = AAC-LC, 8 = AAC-ELD */
	uint8_t reserved;
};
#pragma pack(pop)
