# Relios Mirror: the iPad's AirPlay screen mirroring in a window of its own (no OBS).
#
#   ReliosMirror(.exe)             the window: decodes (FFmpeg), shows and plays (SDL2), records; wired (USB)
#                                  mirroring on Windows (viewer/usbqt.c, viewer/qtproto.c)
#   relios-airplay-receiver(.exe)    ReliosOBS's receiver (receiver/; what Relios Mirror changed is marked): the network side
#
# Windows: built in MSYS2's UCRT64 (see paketle.sh, derle.bat and .github/workflows/windows.yml).
cmake_minimum_required(VERSION 3.20)
project(ReliosMirror C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

if(WIN32)
  set(OPENSSL_USE_STATIC_LIBS TRUE) # the receiver is one .exe without DLLs
endif()
add_subdirectory(receiver)

find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavformat libavcodec libswscale libavutil)
find_package(SDL2 REQUIRED CONFIG)
pkg_check_modules(SDL2TTF REQUIRED IMPORTED_TARGET SDL2_ttf)

if(WIN32)
  enable_language(RC)
  add_executable(ReliosMirror WIN32 viewer/main.c viewer/proc_win.c viewer/proc.h viewer/version.h viewer/app.rc
                 viewer/qtproto.c viewer/qtproto.h viewer/usbqt.c viewer/usbqt.h)
  set_source_files_properties(viewer/app.rc PROPERTIES COMPILE_FLAGS "-I${CMAKE_CURRENT_SOURCE_DIR}/viewer")
  # (cfgmgr32, setupapi: the device list and the USB driver for wired mirroring; cfgmgr32 first, so that the CM_
  # functions come from the DLL that documents them)
  target_link_libraries(ReliosMirror PRIVATE bcrypt advapi32 shell32 user32 ole32 cfgmgr32 setupapi)
  # the one-file version: carries the whole folder and unpacks it at the first start (see paketle.sh)
  add_executable(launcher WIN32 viewer/launcher.c viewer/app.rc)
  target_link_options(launcher PRIVATE -static)
  # the USB driver's installer, run with administrator rights: static, and only Windows' own DLLs (cfgmgr32 and
  # bcrypt are not linked: the CM_ functions come from setupapi, bcrypt is loaded from System32; paketle.sh checks)
  add_executable(relios-usb-kurucu WIN32 viewer/usbdriver.c viewer/usbids.h viewer/usbqt.h viewer/usbdriver.rc)
  set_source_files_properties(viewer/usbdriver.rc PROPERTIES COMPILE_FLAGS "-I${CMAKE_CURRENT_SOURCE_DIR}/viewer")
  target_link_libraries(relios-usb-kurucu PRIVATE setupapi advapi32 shell32)
  target_link_options(relios-usb-kurucu PRIVATE -static)
  target_compile_options(relios-usb-kurucu PRIVATE -Wall -Wextra)
else()
  add_executable(ReliosMirror viewer/main.c viewer/proc_posix.c viewer/proc.h viewer/qtproto.c viewer/qtproto.h
                 viewer/usbqt.c viewer/usbqt.h)
  target_link_libraries(ReliosMirror PRIVATE m)
endif()
target_include_directories(ReliosMirror PRIVATE receiver)
if(TARGET SDL2::SDL2main)
  target_link_libraries(ReliosMirror PRIVATE SDL2::SDL2main)
endif()
target_link_libraries(ReliosMirror PRIVATE PkgConfig::SDL2TTF SDL2::SDL2 PkgConfig::FFMPEG)
# (FFmpeg's and SDL's headers differ a little between versions: a pointer's "const" is not an error here)
target_compile_options(ReliosMirror PRIVATE -Wall -Wno-error=incompatible-pointer-types -Wno-deprecated-declarations)
