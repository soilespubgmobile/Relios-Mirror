#!/usr/bin/env bash
# Builds Relios Mirror and gathers the program with the DLLs it needs in dist/ReliosMirror.
# Run in MSYS2's UCRT64 shell (derle.bat and the GitHub workflow do).
set -euo pipefail
cd "$(dirname "$0")"

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build

out=dist/ReliosMirror
rm -rf dist
mkdir -p "$out"
cp build/bin/ReliosMirror.exe build/bin/relios-airplay-receiver.exe "$out/"

prefix="${MINGW_PREFIX:-/ucrt64}"
copy_dlls() {
	ldd "$1" | awk '$2 == "=>" { print $3 }' | while read -r dll; do
		case "$dll" in
		"$prefix"/bin/*) cp -n "$dll" "$out/" ;;
		esac
	done
}
copy_dlls "$out/ReliosMirror.exe"
copy_dlls "$out/relios-airplay-receiver.exe"
# (where SDL2 is the layer over SDL3, SDL3 is loaded while running: ldd does not show it)
if [ -f "$prefix/bin/SDL3.dll" ]; then
	cp -n "$prefix/bin/SDL3.dll" "$out/"
	copy_dlls "$prefix/bin/SDL3.dll"
fi

cp BENIOKU.txt LICENSE assets/logo.bmp "$out/"
# our own virtual camera (built by the workflow's "softcam" step; without it the program has no virtual camera)
cam=$(find softcam -name softcam.dll -path '*x64*' 2>/dev/null | head -1 || true)
if [ -n "$cam" ]; then
	cp "$cam" "$out/softcam.dll"
	echo "Sanal kamera: $cam ($(stat -c %s "$cam") bayt)"
	# (for the build log: what the camera DLL needs from Windows, and what it offers)
	objdump -p "$out/softcam.dll" | grep -E "DLL Name:" | sed 's/^[[:space:]]*/  ihtiyac: /' || true
	objdump -p "$out/softcam.dll" | grep -E "\] (sc[A-Z][A-Za-z]+|Dll[A-Za-z]+)$" | sed 's/^[[:space:]]*/  sunulan: /' || true
else
	echo "Sanal kamera: softcam.dll YOK"
fi
# the one-file version: the launcher + the folder as .tgz + "RLSM" + size (16 hex digits) + build stamp (14 digits)
mkdir -p dist/tek-exe
tar -czf build/payload.tgz -C "$out" .
single=dist/tek-exe/ReliosMirror.exe
cat build/bin/launcher.exe build/payload.tgz > "$single"
printf 'RLSM%016x%s' "$(stat -c %s build/payload.tgz)" "$(date +%Y%m%d%H%M%S)" >> "$single"
echo "Tek dosya: $single ($(du -sh "$single" | cut -f1))"

echo
echo "Hazir: $out ($(ls "$out" | wc -l) dosya, $(du -sh "$out" | cut -f1))"
