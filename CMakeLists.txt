cmake_minimum_required(VERSION 3.20)
project(sms_switch VERSION 0.1.0 LANGUAGES C CXX ASM)

if(NOT NINTENDO_SWITCH)
  message(FATAL_ERROR "Configure with the devkitPro toolchain: "
    "-DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/Switch.cmake (or use ./build.sh)")
endif()

set(SMS_SWITCH_DIR ${CMAKE_CURRENT_SOURCE_DIR}/switch)

# The game is built as on the 64-bit PC builds: everything it can point at is
# kept below 4 GiB (switch/src/low_memory.cpp moves the program there).
set(SMS_ARCH 64 CACHE STRING "" FORCE)
# The GameCube compiler's char is signed (-char signed); AArch64's is not.
add_compile_options(-fsigned-char)
# Host headers newlib does not have (dlfcn.h, execinfo.h, sys/mman.h, ...),
# searched after the system's, so the port's own wrappers' #include_next
# reaches them too.
add_compile_options("SHELL:-idirafter ${SMS_SWITCH_DIR}/include")
# Mesa's GL/EGL headers (their pkg-config files are unusable, see below).
include_directories(SYSTEM ${DEVKITPRO}/portlibs/switch/include)

# No libdl: dlopen/dlsym are switch/src/posix_shims.cpp.
set(CMAKE_DL_LIBS "")
# The GX self-tests are desktop programs.
set(SMS_GX_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(sms-port ${CMAKE_BINARY_DIR}/port)

target_sources(sms PRIVATE
  ${SMS_SWITCH_DIR}/src/low_memory.cpp
  ${SMS_SWITCH_DIR}/src/performance.cpp
  ${SMS_SWITCH_DIR}/src/posix_shims.cpp
  ${SMS_SWITCH_DIR}/src/stack_switch.cpp
  ${SMS_SWITCH_DIR}/src/switch_main.cpp)
target_include_directories(sms PRIVATE ${SMS_SWITCH_DIR}/src)
# Every thread spread over cores 0-2 (switch/src/performance.cpp).
target_link_options(sms PRIVATE -Wl,--wrap=threadCreate)

# Mesa (OpenGL over nouveau). Its pkg-config files carry an unexpanded
# $(DEVKITPRO), so the libraries are named directly; order matters.
target_link_libraries(sms PRIVATE EGL glapi drm_nouveau)

# The icon (homebrew menu, HOME menu): SMS_SWITCH_ICON=
#   steamgriddb (default)  the box art from SteamGridDB, fetched at build time,
#                          else the disc's memory-card Mario (with a disc)
#   disc                   the memory-card Mario from SMS_SWITCH_BUNDLE_DISC
#   default                libnx's icon (what CI and releases use)
#   /path/to/icon.jpg      your own 256x256 JPEG
# (tools/make_icon.py; no art is kept in this repository.)
set(SMS_SWITCH_ICON "steamgriddb" CACHE STRING "steamgriddb, disc, default, or a 256x256 JPEG")
set(_icon_args "")
if(SMS_SWITCH_ICON STREQUAL "steamgriddb")
  list(APPEND _icon_args --steamgriddb)
endif()
if(SMS_SWITCH_ICON MATCHES "^(steamgriddb|disc)$" AND SMS_SWITCH_BUNDLE_DISC AND EXISTS "${SMS_SWITCH_BUNDLE_DISC}")
  list(APPEND _icon_args --disc "${SMS_SWITCH_BUNDLE_DISC}")
endif()
set(SMS_SWITCH_ICON_FILE ${NX_DEFAULT_ICON})
if(SMS_SWITCH_ICON MATCHES "\\.(jpg|jpeg)$")
  set(SMS_SWITCH_ICON_FILE "${SMS_SWITCH_ICON}")
elseif(_icon_args)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  execute_process(
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/make_icon.py ${_icon_args}
            --out ${CMAKE_BINARY_DIR}/icon.jpg
    RESULT_VARIABLE _icon_rc OUTPUT_VARIABLE _icon_out ERROR_VARIABLE _icon_err)
  if(_icon_rc EQUAL 0)
    set(SMS_SWITCH_ICON_FILE ${CMAKE_BINARY_DIR}/icon.jpg)
    string(STRIP "${_icon_out}" _icon_out)
    message(STATUS "sms-switch: ${_icon_out}")
  else()
    message(WARNING "sms-switch: no icon (${_icon_err}); using libnx's default")
  endif()
endif()

nx_generate_nacp(sms.nacp
  NAME "Super Mario Sunshine"
  AUTHOR "sms-switch"
  VERSION "${PROJECT_VERSION}")
nx_create_nro(sms
  OUTPUT ${CMAKE_BINARY_DIR}/sms.nro
  NACP sms.nacp
  ICON ${SMS_SWITCH_ICON_FILE})

# sms-standalone.nro (optional): the same program with your disc image inside
# its RomFS, so the SD card needs no image beside it. For your own use only:
# it contains the game, so it must never be shared (CI never builds it).
#   -DSMS_SWITCH_BUNDLE_DISC=/path/to/GMSE01.iso   (.iso, .gcm or .ciso)
set(SMS_SWITCH_BUNDLE_DISC "" CACHE FILEPATH "Disc image to pack into sms-standalone.nro (personal use only)")
# A standalone NRO left from another image (or none now) would be stale:
# elf2nro reruns only when the program changes.
if(NOT "${SMS_SWITCH_BUNDLE_DISC}" STREQUAL "${_SMS_SWITCH_BUNDLED}")
  file(REMOVE ${CMAKE_BINARY_DIR}/sms-standalone.nro)
  set(_SMS_SWITCH_BUNDLED "${SMS_SWITCH_BUNDLE_DISC}" CACHE INTERNAL "")
endif()
if(SMS_SWITCH_BUNDLE_DISC)
  if(NOT EXISTS "${SMS_SWITCH_BUNDLE_DISC}")
    message(FATAL_ERROR "SMS_SWITCH_BUNDLE_DISC: ${SMS_SWITCH_BUNDLE_DISC} does not exist")
  endif()
  get_filename_component(_disc_ext "${SMS_SWITCH_BUNDLE_DISC}" LAST_EXT)
  string(TOLOWER "${_disc_ext}" _disc_ext)
  if(NOT _disc_ext MATCHES "^\\.(iso|gcm|ciso)$")
    message(FATAL_ERROR "SMS_SWITCH_BUNDLE_DISC: expected an .iso, .gcm or .ciso file")
  endif()
  # The RomFS holds only the image, as romfs:/disc<ext> (a link, not a copy).
  set(_romfs ${CMAKE_BINARY_DIR}/standalone-romfs)
  file(REMOVE_RECURSE ${_romfs})
  file(MAKE_DIRECTORY ${_romfs})
  file(CREATE_LINK "${SMS_SWITCH_BUNDLE_DISC}" ${_romfs}/disc${_disc_ext} SYMBOLIC)
  nx_create_nro(sms_standalone
    TARGET sms
    OUTPUT ${CMAKE_BINARY_DIR}/sms-standalone.nro
    NACP sms.nacp
    ICON ${SMS_SWITCH_ICON_FILE}
    ROMFS ${_romfs})
  message(STATUS "sms-switch: also building sms-standalone.nro with ${SMS_SWITCH_BUNDLE_DISC} inside")
endif()

# gx_selftest.nro: the port's GX self-test on the Switch's GL stack (diagnosis).
option(SMS_SWITCH_GX_SELFTEST "Also build gx_selftest.nro" ON)
if(SMS_SWITCH_GX_SELFTEST)
  set(_gx ${CMAKE_CURRENT_SOURCE_DIR}/sms-port/platform/gx)
  add_executable(gx_selftest
    ${_gx}/tests/gx_selftest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/sms-port/platform/misc/msl_math.c
    ${CMAKE_CURRENT_SOURCE_DIR}/sms-port/platform/misc/port_cheats.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/sms-port/platform/misc/port_warp.cpp
    ${SMS_SWITCH_DIR}/src/gx_selftest_main.cpp
    ${SMS_SWITCH_DIR}/src/low_memory.cpp
    ${SMS_SWITCH_DIR}/src/posix_shims.cpp)
  set_source_files_properties(${_gx}/tests/gx_selftest.cpp PROPERTIES COMPILE_DEFINITIONS main=gx_selftest_main)
  set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/sms-port/platform/misc/msl_math.c PROPERTIES
    COMPILE_OPTIONS -ffp-contract=off INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/sms-port/src)
  set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/sms-port/platform/misc/port_warp.cpp PROPERTIES
    INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/sms-port/src/port_include)
  target_include_directories(gx_selftest PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/sms-port/decomp/include ${CMAKE_CURRENT_SOURCE_DIR}/sms-port/decomp/libs/dolphin/include
    ${_gx}/src ${SMS_SWITCH_DIR}/src)
  target_compile_features(gx_selftest PRIVATE cxx_std_17)
  target_link_libraries(gx_selftest PRIVATE sms_gx EGL glapi drm_nouveau)
  nx_create_nro(gx_selftest OUTPUT ${CMAKE_BINARY_DIR}/gx_selftest.nro NOICON NONACP)
endif()
