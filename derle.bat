@echo off
rem Relios Mirror'i bu bilgisayarda derler. Once MSYS2 kurulu olmali: https://www.msys2.org
rem (varsayilan klasor: C:\msys64). Sonuc: dist\ReliosMirror klasoru.
setlocal
set "MSYS=C:\msys64"
if not exist "%MSYS%\usr\bin\bash.exe" (
  echo MSYS2 bulunamadi: %MSYS%
  echo https://www.msys2.org adresinden kurup bu dosyayi yeniden calistirin.
  pause
  exit /b 1
)
set MSYSTEM=UCRT64
set CHERE_INVOKING=1
cd /d "%~dp0"
"%MSYS%\usr\bin\bash.exe" -lc "pacman -S --needed --noconfirm mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-pkgconf mingw-w64-ucrt-x86_64-openssl mingw-w64-ucrt-x86_64-ffmpeg mingw-w64-ucrt-x86_64-SDL2 && bash paketle.sh"
if errorlevel 1 (
  echo.
  echo Derleme basarisiz oldu. Yukaridaki hata satirlarini kopyalayip gonderin.
) else (
  echo.
  echo Tamam: dist\ReliosMirror\ReliosMirror.exe
)
pause
