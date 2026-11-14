# relios-airplay-receiver: the iPad mirroring receiver (trial). A separate program, built with MinGW-w64 (the
# Windows build runs it in MSYS2's UCRT64; OBS itself is built with MSVC). Also builds on Linux, for checks.
#
#   cmake -S relios/airplay -B build-airplay -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-airplay
cmake_minimum_required(VERSION 3.20)
project(relios-airplay-receiver C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS ON)

option(RELIOS_AIRPLAY_STATIC "Link OpenSSL and the C runtime statically (one .exe, no DLLs)" ON)
option(RELIOS_AIRPLAY_SANITIZE "Build with AddressSanitizer and UBSan (checks on Linux)" OFF)

set(UX ${CMAKE_CURRENT_SOURCE_DIR}/uxplay/lib)
set(PL ${CMAKE_CURRENT_SOURCE_DIR}/libplist)

file(GLOB UX_SOURCES ${UX}/*.c)
file(GLOB UX_LLHTTP ${UX}/llhttp/*.c)
file(GLOB UX_PLAYFAIR ${UX}/playfair/*.c)
file(GLOB UX_MDNSD ${UX}/mdnsd/*.c)
set(PL_SOURCES
    ${PL}/src/base64.c ${PL}/src/bplist.c ${PL}/src/bytearray.c ${PL}/src/common.c ${PL}/src/hashtable.c
    ${PL}/src/jplist.c ${PL}/src/jsmn.c ${PL}/src/oplist.c ${PL}/src/out-default.c ${PL}/src/out-limd.c
    ${PL}/src/out-plutil.c ${PL}/src/plist.c ${PL}/src/ptrarray.c ${PL}/src/time64.c ${PL}/src/xplist.c
    ${PL}/libcnary/node.c ${PL}/libcnary/node_list.c)

add_library(relios_plist STATIC ${PL_SOURCES})
target_include_directories(relios_plist PRIVATE ${PL} ${PL}/src ${PL}/libcnary/include PUBLIC ${PL}/include)
target_compile_definitions(relios_plist PRIVATE HAVE_CONFIG_H LIBPLIST_STATIC)

find_package(OpenSSL 3.0 REQUIRED)

add_executable(relios-airplay-receiver receiver.c ${UX_SOURCES} ${UX_LLHTTP} ${UX_PLAYFAIR} ${UX_MDNSD})
target_include_directories(relios-airplay-receiver PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${UX} ${UX}/llhttp
                                                           ${UX}/playfair ${UX}/mdnsd)
# (as UxPlay's lib/CMakeLists.txt; libplist >= 2.3)
target_compile_definitions(relios-airplay-receiver PRIVATE NOHOLD PLIST_210 PLIST_230 OPENSSL_API_COMPAT=0x10101000L
                                                           LIBPLIST_STATIC)
target_link_libraries(relios-airplay-receiver PRIVATE relios_plist OpenSSL::Crypto)

if(WIN32)
  # (as UxPlay's lib/CMakeLists.txt for Windows)
  target_compile_definitions(relios-airplay-receiver PRIVATE WIN32 _WIN32_WINNT=0x0A00 WIN32_LEAN_AND_MEAN STANDALONE
                                                             TARGET_POSIX _REENTRANT _LARGEFILE64_SOURCE
                                                             _FILE_OFFSET_BITS=64)
  target_compile_definitions(relios_plist PRIVATE WIN32)
  target_link_libraries(relios-airplay-receiver PRIVATE ws2_32 iphlpapi crypt32 bcrypt advapi32 user32)
  if(RELIOS_AIRPLAY_STATIC)
    target_link_options(relios-airplay-receiver PRIVATE -static -static-libgcc)
  endif()
else()
  target_compile_definitions(relios-airplay-receiver PRIVATE _GNU_SOURCE)
  target_compile_definitions(relios_plist PRIVATE _GNU_SOURCE)
  target_link_libraries(relios-airplay-receiver PRIVATE pthread m)
endif()

target_compile_options(relios-airplay-receiver PRIVATE -Wall -Wno-unused-variable -Wno-unused-but-set-variable)
# (FairPlay's obfuscated code counts on signed numbers wrapping around: so they do, as the code expects, instead of
# leaving it to the compiler's assumptions)
set_source_files_properties(${UX_PLAYFAIR} PROPERTIES COMPILE_OPTIONS "-fwrapv")
if(RELIOS_AIRPLAY_SANITIZE)
  foreach(t relios-airplay-receiver relios_plist)
    target_compile_options(${t} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer -g)
    target_link_options(${t} PRIVATE -fsanitize=address,undefined)
  endforeach()
endif()
