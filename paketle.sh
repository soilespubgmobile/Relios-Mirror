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

cp BENIOKU.txt LICENSE guvenlik-duvari-izni.bat assets/logo.bmp "$out/"
echo
echo "Hazir: $out ($(ls "$out" | wc -l) dosya, $(du -sh "$out" | cut -f1))"
