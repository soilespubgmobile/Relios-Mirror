/*
 * Fuzzes the parsers of the Wi-Fi receiver that anyone on the local network can reach before pairing:
 *   mdns   the multicast DNS questions (receiver/uxplay/lib/mdnsd/mdnsd.c): any device on the network sends them
 *   http   the RTSP / HTTP requests (http_request.c with llhttp): the first thing a connection sends
 *   plist  binary property lists (libplist's plist_from_bin): request bodies, /info's included
 * Random and damaged inputs, many times over, with AddressSanitizer and UBSan; seeds are made here (valid
 * questions, requests and plists like an iPad's), then damaged.
 *
 *   (see tests/run_checks.sh for the build line)
 *   ./receiver_parsers_fuzz mdns|http|plist <iterations> [seed]
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the mDNS responder, with its static functions (it is compiled here, not linked) */
#include "mdnsd/mdnsd.c"

#include "http_request.h"
#include "plist/plist.h"

static uint64_t rng = 0x9e3779b97f4a7c15ull;

static uint64_t next(void)
{
	rng ^= rng >> 12;
	rng ^= rng << 25;
	rng ^= rng >> 27;
	return rng * 0x2545f4914f6cdd1dull;
}

static uint32_t below(uint32_t n)
{
	return n ? (uint32_t)(next() % n) : 0;
}

static void fail(const char *what)
{
	fprintf(stderr, "FAIL: %s\n", what);
	abort();
}

struct buf {
	uint8_t *p;
	size_t n, cap;
};

static void put(struct buf *b, const void *d, size_t n)
{
	if (b->n + n > b->cap) {
		size_t cap = b->cap ? b->cap : 256;
		while (cap < b->n + n)
			cap *= 2;
		b->p = realloc(b->p, cap);
		if (!b->p)
			fail("out of memory");
		b->cap = cap;
	}
	if (n)
		memcpy(b->p + b->n, d, n);
	b->n += n;
}

static void put8(struct buf *b, unsigned v)
{
	uint8_t c = (uint8_t)v;
	put(b, &c, 1);
}

static void put16be(struct buf *b, unsigned v)
{
	put8(b, v >> 8);
	put8(b, v);
}

static void puts_(struct buf *b, const char *s)
{
	put(b, s, strlen(s));
}

static void put_random(struct buf *b, size_t n)
{
	for (size_t i = 0; i < n; i++)
		put8(b, (unsigned)next());
}

static void mutate(struct buf *b)
{
	if (!b->n)
		return;
	switch (below(7)) {
	case 0:
		b->p[below((uint32_t)b->n)] ^= (uint8_t)(1u << below(8));
		break;
	case 1:
		b->p[below((uint32_t)b->n)] = (uint8_t)(below(2) ? next() : below(2) ? 0 : 0xff);
		break;
	case 2:
		b->n = below((uint32_t)b->n + 1);
		break;
	case 3: {
		size_t from = below((uint32_t)b->n), len = below((uint32_t)(b->n - from) + 1);
		memmove(b->p + from, b->p + from + len, b->n - from - len);
		b->n -= len;
		break;
	}
	case 4: {
		size_t from = below((uint32_t)b->n), len = below((uint32_t)(b->n - from) % 512 + 1);
		uint8_t *copy = malloc(len + 1);
		memcpy(copy, b->p + from, len);
		put(b, copy, len);
		free(copy);
		break;
	}
	case 5:
		if (b->n >= 2) {
			size_t at = below((uint32_t)b->n - 1);
			static const uint16_t odd[] = {0, 1, 0x7f, 0x80, 0xff, 0x100, 0x7fff, 0x8000, 0xc000, 0xc00c, 0xffff};
			uint16_t v = odd[below(sizeof(odd) / sizeof(odd[0]))];
			b->p[at] = (uint8_t)(v >> 8);
			b->p[at + 1] = (uint8_t)v;
		}
		break;
	default: {
		size_t at = below((uint32_t)b->n + 1), len = 1 + below(8);
		uint8_t tmp[8];
		for (size_t i = 0; i < len; i++)
			tmp[i] = (uint8_t)next();
		struct buf rest = {0};
		put(&rest, b->p + at, b->n - at);
		b->n = at;
		put(b, tmp, len);
		put(b, rest.p, rest.n);
		free(rest.p);
		break;
	}
	}
}

/* ------------------------------------------------------------------ mDNS */

static const char *const names[] = {"_services._dns-sd._udp.local", "_airplay._tcp.local", "_raop._tcp.local",
				    "Relios Mirror._airplay._tcp.local", "021122334455@Relios Mirror._raop._tcp.local",
				    "Relios-Mirror.local", "local", "", "a.b.c.d.e.f.g.h.i.j.k"};

static void put_name(struct buf *b, const char *name, size_t start)
{
	/* labels; now and then a compression pointer (forward, backward, to itself, past the end) */
	const char *p = name;
	while (*p) {
		const char *dot = strchr(p, '.');
		size_t len = dot ? (size_t)(dot - p) : strlen(p);
		if (!below(12)) {
			unsigned ptr = below(3) ? 12 + below(40) : below(2) ? (unsigned)b->n : 0x3fff;
			put16be(b, 0xc000 | (ptr & 0x3fff));
			return;
		}
		put8(b, below(30) ? len : below(256));
		put(b, p, len);
		p += len + (dot ? 1 : 0);
	}
	(void)start;
	put8(b, 0);
}

static void make_mdns(struct buf *b)
{
	put16be(b, (unsigned)next());                  /* id */
	put16be(b, below(4) ? 0 : (unsigned)next());   /* flags (a response is ignored) */
	unsigned questions = below(6) ? 1 + below(4) : below(2) ? 0xffff : below(64);
	put16be(b, questions);
	put16be(b, below(3));
	put16be(b, below(3));
	put16be(b, below(3));
	unsigned n = questions > 8 ? 8 : questions;
	for (unsigned i = 0; i < n; i++) {
		if (below(10)) {
			put_name(b, names[below(sizeof(names) / sizeof(names[0]))], 12);
		} else {
			/* a long name: many labels, up to the name buffer's size and past it */
			unsigned labels = 1 + below(80);
			for (unsigned l = 0; l < labels; l++) {
				unsigned len = 1 + below(63);
				put8(b, len);
				put_random(b, len);
			}
			put8(b, 0);
		}
		static const unsigned types[] = {1, 12, 16, 28, 33, 255, 0, 0xffff};
		put16be(b, types[below(sizeof(types) / sizeof(types[0]))]);
		put16be(b, below(2) ? 0x8001 : 1);
	}
	if (!below(4))
		put_random(b, below(64));
}

static void fuzz_mdns(unsigned long long iterations)
{
	mdnsd_t *m = mdnsd_init("Relios-Mirror.local");
	if (!m)
		fail("mdnsd_init");
	mdnsd_service_t airplay, raop;
	memset(&airplay, 0, sizeof(airplay));
	memset(&raop, 0, sizeof(raop));
	snprintf(airplay.type, sizeof(airplay.type), "_airplay._tcp.local");
	snprintf(airplay.name, sizeof(airplay.name), "Relios Mirror._airplay._tcp.local");
	airplay.port = 7000;
	airplay.registered = 1;
	mdnsd_txt_add(&airplay.txt, "deviceid", "02:11:22:33:44:55");
	mdnsd_txt_add(&airplay.txt, "features", "0x5A7FFEE6,0x0");
	snprintf(raop.type, sizeof(raop.type), "_raop._tcp.local");
	snprintf(raop.name, sizeof(raop.name), "021122334455@Relios Mirror._raop._tcp.local");
	raop.port = 7000;
	raop.registered = 1;
	mdnsd_txt_add(&raop.txt, "ch", "2");
	mdnsd_set_services(m, &airplay, &raop);
	struct buf b = {0};
	struct sockaddr_in from;
	memset(&from, 0, sizeof(from));
	from.sin_family = AF_INET;
	for (unsigned long long it = 0; it < iterations; it++) {
		b.n = 0;
		make_mdns(&b);
		unsigned damage = below(3) ? below(4) : below(30);
		for (unsigned i = 0; i < damage; i++)
			mutate(&b);
		if (b.n > MDNS_MAX_PACKET)
			b.n = MDNS_MAX_PACKET; /* (what recvfrom gives at most) */
		/* (in a buffer of exactly its size: a read past the end is caught) */
		uint8_t *exact = malloc(b.n ? b.n : 1);
		memcpy(exact, b.p, b.n);
		from.sin_port = below(2) ? htons(MDNS_PORT) : htons(40000);
		mdns_handle_query(m, exact, b.n, below(2) ? MDNS_FAMILY_IPV4 : MDNS_FAMILY_IPV6, &from, from.sin_port);
		/* the name reader alone, at random places */
		char name[MDNSD_MAX_NAME];
		size_t off = b.n ? below((uint32_t)b.n) : 0;
		mdns_read_name(exact, b.n, &off, name, below(4) ? sizeof(name) : 1 + below(8));
		free(exact);
	}
	free(b.p);
	mdnsd_destroy(m);
	printf("mdns: %llu packets: no fault\n", iterations);
}

/* ------------------------------------------------------------------ RTSP / HTTP requests */

static void make_request(struct buf *b)
{
	static const char *const methods[] = {"GET", "POST", "SETUP", "RECORD", "TEARDOWN", "OPTIONS", "GET_PARAMETER",
					      "SET_PARAMETER", "FLUSH", "PUT", "XYZ", ""};
	static const char *const urls[] = {"/info", "/pair-setup", "/pair-verify", "/fp-setup", "/pair-pin-start",
					   "rtsp://192.168.1.2/123456789", "/feedback", "/audioMode", "*", "",
					   "/play", "/server-info"};
	static const char *const protocols[] = {"RTSP/1.0", "HTTP/1.1", "HTTP/1.0", "RTSP/2.0", "FOO/1.0", ""};
	puts_(b, methods[below(sizeof(methods) / sizeof(methods[0]))]);
	puts_(b, " ");
	puts_(b, urls[below(sizeof(urls) / sizeof(urls[0]))]);
	puts_(b, " ");
	puts_(b, protocols[below(sizeof(protocols) / sizeof(protocols[0]))]);
	puts_(b, "\r\n");
	unsigned headers = below(10);
	size_t body = below(3) ? below(300) : below(2) ? 0 : below(70000);
	bool length_sent = false;
	for (unsigned i = 0; i < headers; i++) {
		static const char *const names_[] = {"CSeq", "Content-Type", "User-Agent", "X-Apple-Session-ID",
						     "Active-Remote", "DACP-ID", "Transfer-Encoding", "Upgrade",
						     "Connection", "Host", ""};
		if (!below(4)) {
			char line[64];
			length_sent = true;
			snprintf(line, sizeof(line), "Content-Length: %s\r\n",
				 below(4) ? "" : below(2) ? "-1" : below(2) ? "99999999999999999999" : "12abc");
			if (below(2))
				snprintf(line, sizeof(line), "Content-Length: %zu\r\n", below(3) ? body : body + below(50));
			puts_(b, line);
			continue;
		}
		puts_(b, names_[below(sizeof(names_) / sizeof(names_[0]))]);
		puts_(b, below(10) ? ": " : ":");
		if (below(8))
			puts_(b, below(2) ? "application/x-apple-binary-plist" : "AirPlay/845.6.1");
		else
			put_random(b, below(200));
		puts_(b, below(10) ? "\r\n" : "\n");
	}
	if (!length_sent && body && below(2)) {
		char line[64];
		snprintf(line, sizeof(line), "Content-Length: %zu\r\n", body);
		puts_(b, line);
	}
	puts_(b, "\r\n");
	put_random(b, body);
}

static void fuzz_http(unsigned long long iterations)
{
	struct buf b = {0};
	for (unsigned long long it = 0; it < iterations; it++) {
		b.n = 0;
		unsigned requests = 1 + below(3);
		for (unsigned r = 0; r < requests; r++)
			make_request(&b);
		unsigned damage = below(3) ? below(3) : below(20);
		for (unsigned i = 0; i < damage; i++)
			mutate(&b);
		http_request_t *req = http_request_init();
		if (!req)
			fail("http_request_init");
		for (size_t pos = 0; pos < b.n;) {
			size_t piece = below(3) ? 1 + below(below(2) ? 16 : 4096) : b.n - pos;
			if (piece > b.n - pos)
				piece = b.n - pos;
			char *exact = malloc(piece);
			memcpy(exact, b.p + pos, piece);
			int r = http_request_add_data(req, exact, piece);
			free(exact);
			pos += piece;
			if (r < 0 || http_request_has_error(req))
				break;
			if (http_request_is_complete(req)) {
				const char *m = http_request_get_method(req);
				const char *u = http_request_get_url(req);
				const char *p = http_request_get_protocol(req);
				const char *h = http_request_get_header(req, below(2) ? "CSeq" : "Content-Type");
				int len = 0;
				const char *d = http_request_get_data(req, &len);
				volatile size_t sink = (m ? strlen(m) : 0) + (u ? strlen(u) : 0) + (p ? strlen(p) : 0) +
						       (h ? strlen(h) : 0);
				(void)sink;
				if (len < 0 || (len && !d))
					fail("odd request body");
				volatile uint8_t x = 0;
				for (int i = 0; i < len; i++)
					x ^= (uint8_t)d[i];
				int fields = 0;
				size_t fl = 0, vl = 0;
				http_request_header_get_size(req, &fields, &fl, &vl);
				char *hs = NULL;
				if (http_request_get_header_string(req, &hs) >= 0)
					free(hs);
				/* (a connection reads its next request with a new one) */
				http_request_destroy(req);
				req = http_request_init();
				if (!req)
					fail("http_request_init");
			}
		}
		http_request_destroy(req);
	}
	free(b.p);
	printf("http: %llu inputs: no fault\n", iterations);
}

/* ------------------------------------------------------------------ binary plists */

static plist_t random_plist(int depth)
{
	switch (depth > 3 ? below(6) : below(8)) {
	case 0:
		return plist_new_bool(below(2));
	case 1:
		return plist_new_uint(next());
	case 2:
		return plist_new_real((double)(int64_t)next() / 1000.0);
	case 3: {
		char s[64];
		size_t n = below(sizeof(s));
		for (size_t i = 0; i < n; i++)
			s[i] = (char)(32 + below(95));
		s[n] = 0;
		return plist_new_string(s);
	}
	case 4: {
		uint8_t d[128];
		size_t n = below(sizeof(d));
		for (size_t i = 0; i < n; i++)
			d[i] = (uint8_t)next();
		return plist_new_data((const char *)d, n);
	}
	case 5:
		return plist_new_unix_date((int64_t)below(2000000000));
	case 6: {
		plist_t a = plist_new_array();
		unsigned n = below(6);
		for (unsigned i = 0; i < n; i++)
			plist_array_append_item(a, random_plist(depth + 1));
		return a;
	}
	default: {
		plist_t d = plist_new_dict();
		static const char *const keys[] = {"streams", "type", "ekey", "eiv", "timingPort", "qualifier",
						   "deviceID", "name", "model", "osVersion", "isScreenMirroringSession",
						   "timestampInfo", "latencyMs", "audioFormat", "ct", "spf", "sr"};
		unsigned n = below(8);
		for (unsigned i = 0; i < n; i++)
			plist_dict_set_item(d, keys[below(sizeof(keys) / sizeof(keys[0]))], random_plist(depth + 1));
		return d;
	}
	}
}

static void walk(plist_t p, int depth)
{
	/* reads every node (types, values, keys), as the handlers do */
	if (!p || depth > 64)
		return;
	plist_type t = plist_get_node_type(p);
	if (t == PLIST_DICT) {
		plist_dict_iter it = NULL;
		plist_dict_new_iter(p, &it);
		if (!it)
			return;
		for (;;) {
			char *key = NULL;
			plist_t v = NULL;
			plist_dict_next_item(p, it, &key, &v);
			if (!v) {
				free(key);
				break;
			}
			walk(v, depth + 1);
			free(key);
		}
		free(it);
	} else if (t == PLIST_ARRAY) {
		uint32_t n = plist_array_get_size(p);
		for (uint32_t i = 0; i < n; i++)
			walk(plist_array_get_item(p, i), depth + 1);
	} else if (t == PLIST_STRING) {
		char *s = NULL;
		plist_get_string_val(p, &s);
		free(s);
	} else if (t == PLIST_DATA) {
		char *d = NULL;
		uint64_t len = 0;
		plist_get_data_val(p, &d, &len);
		free(d);
	} else if (t == PLIST_UINT) {
		uint64_t v;
		plist_get_uint_val(p, &v);
	} else if (t == PLIST_REAL) {
		double v;
		plist_get_real_val(p, &v);
	}
}

static void fuzz_plist(unsigned long long iterations)
{
	struct buf b = {0};
	for (unsigned long long it = 0; it < iterations; it++) {
		b.n = 0;
		plist_t made = random_plist(0);
		char *bin = NULL;
		uint32_t len = 0;
		plist_to_bin(made, &bin, &len);
		plist_free(made);
		if (bin)
			put(&b, bin, len);
		free(bin);
		unsigned damage = below(4) ? 1 + below(3) : below(30);
		for (unsigned i = 0; i < damage; i++)
			mutate(&b);
		char *exact = malloc(b.n ? b.n : 1);
		memcpy(exact, b.p, b.n);
		plist_t p = NULL;
		plist_from_bin(exact, (uint32_t)b.n, &p);
		walk(p, 0);
		if (p)
			plist_free(p);
		free(exact);
	}
	free(b.p);
	printf("plist: %llu inputs: no fault\n", iterations);
}

int main(int argc, char **argv)
{
	if (argc < 3) {
		fprintf(stderr, "usage: %s mdns|http|plist <iterations> [seed]\n", argv[0]);
		return 2;
	}
	unsigned long long iterations = strtoull(argv[2], NULL, 10);
	if (argc > 3)
		rng ^= strtoull(argv[3], NULL, 0) * 0x9e3779b97f4a7c15ull + 1;
	if (!strcmp(argv[1], "mdns"))
		fuzz_mdns(iterations);
	else if (!strcmp(argv[1], "http"))
		fuzz_http(iterations);
	else if (!strcmp(argv[1], "plist"))
		fuzz_plist(iterations);
	else
		return 2;
	return 0;
}
