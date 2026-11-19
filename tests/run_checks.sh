#!/bin/sh
# Relios Mirror: all the checks that run without an iPad, on Linux (gcc, cmake, ninja, OpenSSL 3 headers;
# python3 with "cryptography" for the receiver's tests; valgrind if present).
#
#   sh tests/run_checks.sh [quick|full]
#
# quick (the default) runs every check with fewer fuzzing inputs; full runs millions of them (several minutes).
# Builds go to $OUT (default /tmp/relios-checks). Exits non-zero at the first failure.
set -eu
cd "$(dirname "$0")/.."
MODE=${1:-quick}
OUT=${OUT:-/tmp/relios-checks}
mkdir -p "$OUT"
if [ "$MODE" = full ]; then FUZZ=2500000; RFUZZ=1000000; else FUZZ=100000; RFUZZ=50000; fi
SAN="-O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer"
step() { printf '\n=== %s\n' "$*"; }
# runs a check; its last lines are shown; a failure stops everything (with its whole output)
run() {
	if "$@" >"$OUT/last.log" 2>&1; then
		tail -n 2 "$OUT/last.log"
	else
		cat "$OUT/last.log"
		echo "FAILED: $*"
		exit 1
	fi
}

step "USB protocol: unit tests (AddressSanitizer + UBSan)"
gcc -std=c11 -Wall -Wextra -Werror $SAN -Iviewer tests/qtproto_test.c viewer/qtproto.c -o "$OUT/qtproto_test"
run "$OUT/qtproto_test" tests/fixtures/qvh

if command -v valgrind >/dev/null 2>&1; then
	step "USB protocol: unit tests (valgrind)"
	gcc -std=c11 -O1 -g -Iviewer tests/qtproto_test.c viewer/qtproto.c -o "$OUT/qtproto_test_plain"
	run valgrind -q --error-exitcode=9 --leak-check=full "$OUT/qtproto_test_plain" tests/fixtures/qvh
fi

step "USB protocol: fuzzing ($FUZZ inputs)"
gcc -std=c11 -Wall -Wextra -Werror $SAN -Iviewer tests/qtproto_fuzz.c viewer/qtproto.c -o "$OUT/qtproto_fuzz"
run "$OUT/qtproto_fuzz" tests/fixtures/qvh "$FUZZ" 1

step "USB device instances the driver installer accepts"
gcc -std=c11 -Wall -Wextra -Werror $SAN -Iviewer tests/usbids_test.c -o "$OUT/usbids_test"
run "$OUT/usbids_test"

step "Picture queue: threads (ThreadSanitizer), memory (AddressSanitizer), without a thread"
gcc -std=gnu11 -O1 -g -Wall -Wextra -Werror -fsanitize=thread -Itests/sdl_shim -Iviewer tests/usbqueue_test.c \
	viewer/usbqueue.c -lpthread -o "$OUT/usbqueue_tsan"
run "$OUT/usbqueue_tsan"
gcc -std=gnu11 -Wall -Wextra -Werror $SAN -Itests/sdl_shim -Iviewer tests/usbqueue_test.c viewer/usbqueue.c \
	-lpthread -o "$OUT/usbqueue_asan"
run "$OUT/usbqueue_asan"
run "$OUT/usbqueue_asan" nothread

step "Wi-Fi receiver: the parsers reachable before pairing, fuzzed ($RFUZZ inputs each)"
PL=receiver/libplist
UX=receiver/uxplay/lib
mkdir -p "$OUT/plist"
# (NDEBUG as in the program's Release build: libplist's asserts are not compiled in there)
for f in base64 bplist bytearray common hashtable jplist jsmn oplist out-default out-limd out-plutil plist ptrarray \
	time64 xplist; do
	gcc -std=gnu11 $SAN -DNDEBUG -D_GNU_SOURCE -DHAVE_CONFIG_H -DLIBPLIST_STATIC -I$PL -I$PL/src -I$PL/libcnary/include \
		-I$PL/include -c $PL/src/$f.c -o "$OUT/plist/$f.o"
done
for f in node node_list; do
	gcc -std=gnu11 $SAN -DNDEBUG -D_GNU_SOURCE -DHAVE_CONFIG_H -DLIBPLIST_STATIC -I$PL -I$PL/src -I$PL/libcnary/include \
		-I$PL/include -c $PL/libcnary/$f.c -o "$OUT/plist/$f.o"
done
gcc -std=gnu11 $SAN -DNDEBUG -D_GNU_SOURCE -DNOHOLD -DPLIST_210 -DPLIST_230 -DLIBPLIST_STATIC -I$UX -I$UX/llhttp -I$UX/mdnsd \
	-I$PL/include tests/receiver_parsers_fuzz.c $UX/http_request.c $UX/llhttp/api.c $UX/llhttp/http.c \
	$UX/llhttp/llhttp.c "$OUT"/plist/*.o -lpthread -lm -o "$OUT/receiver_parsers_fuzz"
for m in mdns http plist; do run "$OUT/receiver_parsers_fuzz" $m "$RFUZZ" 1; done

step "Wi-Fi receiver: built with AddressSanitizer + UBSan, its own tests"
cmake -S receiver -B "$OUT/receiver" -G Ninja -DCMAKE_BUILD_TYPE=Debug -DRELIOS_AIRPLAY_SANITIZE=ON \
	-DRELIOS_AIRPLAY_STATIC=OFF >/dev/null
cmake --build "$OUT/receiver" >/dev/null
if python3 -c 'import cryptography' 2>/dev/null; then
	run python3 tests/receiver_test.py "$OUT/receiver/relios-airplay-receiver" "$OUT/work"
	run python3 tests/receiver_security_test.py "$OUT/receiver/relios-airplay-receiver" "$OUT/work"
	if [ "$MODE" = full ]; then LIVE=120; else LIVE=30; fi
	run python3 tests/receiver_live_fuzz.py "$OUT/receiver/relios-airplay-receiver" "$OUT/work" "$LIVE"
	step "Wi-Fi receiver: as released (NDEBUG) with AddressSanitizer + UBSan, fuzzed over the network"
	cmake -S receiver -B "$OUT/receiver-release" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
		-DRELIOS_AIRPLAY_SANITIZE=ON -DRELIOS_AIRPLAY_STATIC=OFF >/dev/null
	cmake --build "$OUT/receiver-release" >/dev/null
	run python3 tests/receiver_live_fuzz.py "$OUT/receiver-release/relios-airplay-receiver" "$OUT/work" "$LIVE"
else
	echo "(python3 \"cryptography\" missing: the receiver's tests were skipped)"
fi

step "all checks passed"
