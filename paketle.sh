#!/usr/bin/env bash
# Builds Relios Mirror and gathers the program with the DLLs it needs in dist/ReliosMirror.
# Run in MSYS2's UCRT64 shell (derle.bat and the GitHub workflow do).
set -euo pipefail
cd "$(dirname "$0")"

# our own virtual camera (built by the workflow's "softcam" step; without it the program has no virtual camera). Its
# fingerprint goes into the installer, which installs nothing else under that name
cam=$(find softcam -name softcam.dll -path '*x64*' 2>/dev/null | head -1 || true)
cam_sha=""
if [ -n "$cam" ]; then
	cam_sha=$(sha256sum "$cam" | cut -d' ' -f1)
fi
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DSOFTCAM_SHA256="$cam_sha"
cmake --build build

out=dist/ReliosMirror
rm -rf dist
mkdir -p "$out"
cp build/bin/ReliosMirror.exe build/bin/relios-airplay-receiver.exe build/bin/relios-usb-kurucu.exe "$out/"
# The USB driver's installer runs with administrator rights from this folder: it may need nothing from the folder.
# Only these may be imported (Windows' KnownDLLs and its C runtime); anything else stops the build.
bad=$(objdump -p "$out/relios-usb-kurucu.exe" | sed -n 's/^[[:space:]]*DLL Name: //p' |
	grep -viE '^(kernel32|advapi32|setupapi|shell32)\.dll$|^api-ms-win-crt-[a-z0-9-]+\.dll$' || true)
if [ -n "$bad" ]; then
	echo "relios-usb-kurucu.exe beklenmeyen DLL istiyor: $bad"
	exit 1
fi
echo "relios-usb-kurucu.exe: $(objdump -p "$out/relios-usb-kurucu.exe" | sed -n 's/^[[:space:]]*DLL Name: //p' | tr '\n' ' ')"
# exploit mitigations: ASLR (also above 4 GB) and DEP on every program, or the build stops
for exe in ReliosMirror.exe relios-airplay-receiver.exe relios-usb-kurucu.exe; do
	flags=$(objdump -p "$out/$exe" | grep -A6 '^DllCharacteristics')
	for want in DYNAMIC_BASE HIGH_ENTROPY_VA NX_COMPAT; do
		if ! echo "$flags" | grep -q "$want"; then
			echo "$exe: $want yok"
			exit 1
		fi
	done
done
echo "ASLR + DEP: tamam"

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
if [ -n "$cam" ]; then
	cp "$cam" "$out/softcam.dll"
	# (the copy must be the very file whose fingerprint the installer carries)
	if [ "$(sha256sum "$out/softcam.dll" | cut -d' ' -f1)" != "$cam_sha" ]; then
		echo "softcam.dll: parmak izi degisti"
		exit 1
	fi
	if ! grep -q "$cam_sha" build/bin/relios-usb-kurucu.exe; then
		echo "relios-usb-kurucu.exe: kameranin parmak izi icinde yok"
		exit 1
	fi
	echo "Sanal kamera parmak izi (kurucuda): $cam_sha"
	# (MIT: the notice goes with the DLL)
	if [ -f softcam/LICENSE ]; then cp softcam/LICENSE "$out/softcam-LICENSE.txt"; fi
	echo "Sanal kamera: $cam ($(stat -c %s "$cam") bayt)"
	# (for the build log: what the camera DLL needs from Windows, and what it offers)
	objdump -p "$out/softcam.dll" | grep -E "DLL Name:" | sed 's/^[[:space:]]*/  ihtiyac: /' || true
	objdump -p "$out/softcam.dll" | grep -E "\] (sc[A-Z][A-Za-z]+|Dll[A-Za-z]+)$" | sed 's/^[[:space:]]*/  sunulan: /' || true
else
	echo "Sanal kamera: softcam.dll YOK"
fi
# the USB driver for wired mirroring (libusb-win32 1.2.6.0: fetched and checked by the workflow's "USB driver" step).
# The program only installs it when "Kur" is pressed, after checking the files' SHA-256 fingerprints again; nothing
# in this folder is ever loaded by the program itself.
if [ -f usb/libusb0.sys ] && [ -f usb/libusb0.dll ]; then
	mkdir -p "$out/usb"
	cp usb/* "$out/usb/"
	cp assets/usb-BENIOKU.txt "$out/usb/BENIOKU-USB.txt"
	echo "USB surucusu: $(cd "$out/usb" && ls | tr '\n' ' ')"
	(cd "$out/usb" && sha256sum libusb0.sys libusb0.dll)
else
	echo "USB surucusu: YOK (kablolu yansitmanin surucusu bu derlemede kurulamaz)"
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
