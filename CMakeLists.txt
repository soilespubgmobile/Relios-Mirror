# Relios Mirror: the iPad's AirPlay screen mirroring in a window of its own (no OBS).
#
#   ReliosMirror(.exe)             the window: decodes (FFmpeg), shows and plays (SDL2), records
#   relios-airplay-receiver(.exe)    ReliosOBS's receiver, unchanged (receiver/): the network side
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
  add_executable(ReliosMirror WIN32 viewer/main.c viewer/proc_win.c viewer/proc.h)
  target_link_libraries(ReliosMirror PRIVATE bcrypt advapi32 shell32 user32)
else()
  add_executable(ReliosMirror viewer/main.c viewer/proc_posix.c viewer/proc.h)
  target_link_libraries(ReliosMirror PRIVATE m)
endif()
target_include_directories(ReliosMirror PRIVATE receiver)
if(TARGET SDL2::SDL2main)
  target_link_libraries(ReliosMirror PRIVATE SDL2::SDL2main)
endif()
target_link_libraries(ReliosMirror PRIVATE PkgConfig::SDL2TTF SDL2::SDL2 PkgConfig::FFMPEG)
# (FFmpeg's and SDL's headers differ a little between versions: a pointer's "const" is not an error here)
target_compile_options(ReliosMirror PRIVATE -Wall -Wno-error=incompatible-pointer-types -Wno-deprecated-declarations)
