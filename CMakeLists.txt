cmake_minimum_required(VERSION 3.20)
project(sms_switch VERSION 0.1.0 LANGUAGES C CXX ASM)

if(NOT NINTENDO_SWITCH)
  message(FATAL_ERROR "Configure with the devkitPro toolchain: "
    "-DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/Switch.cmake (or use ./build.sh)")
endif()

set(SMS_SWITCH_DIR ${CMAKE_CURRENT_SOURCE_DIR}/switch)

# -DSMS_ECLIPSE=ON (the port's option, sms-port docs/ECLIPSE.md): Super Mario
# Eclipse's code built into the game, as sms-eclipse.nro, for the Eclipse disc
# (GMSE04). It is its own program, so it gets its own name and folder
# (sdmc:/switch/sms-eclipse/: settings, saves and shader cache apart).
option(SMS_ECLIPSE "Build Super Mario Eclipse in (sms-eclipse.nro)" OFF)
if(SMS_ECLIPSE)
  set(SMS_SWITCH_APP sms-eclipse)
  set(SMS_SWITCH_TITLE "Super Mario Eclipse")
else()
  set(SMS_SWITCH_APP sms)
  set(SMS_SWITCH_TITLE "Super Mario Sunshine")
endif()

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
  ${SMS_SWITCH_DIR}/src/dev_link.cpp
  ${SMS_SWITCH_DIR}/src/low_memory.cpp
  ${SMS_SWITCH_DIR}/src/performance.cpp
  ${SMS_SWITCH_DIR}/src/posix_shims.cpp
  ${SMS_SWITCH_DIR}/src/stack_switch.cpp
  ${SMS_SWITCH_DIR}/src/switch_main.cpp)
target_include_directories(sms PRIVATE ${SMS_SWITCH_DIR}/src)
# The Switch's button prompt images (switch/prompts/switch.png, drawn by
# tools/make_prompts.py), built into the program: switch_main.cpp writes them
# to prompts/ beside the NRO when that has none.
set(_prompts_png ${SMS_SWITCH_DIR}/prompts/switch.png)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_prompts_png})
file(READ ${_prompts_png} _png HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _png "${_png}")
file(WRITE ${CMAKE_BINARY_DIR}/prompts_png.c.new
  "const unsigned char sms_switch_prompts_png[] = {${_png}};\n"
  "const unsigned long sms_switch_prompts_png_size = sizeof sms_switch_prompts_png;\n")
configure_file(${CMAKE_BINARY_DIR}/prompts_png.c.new ${CMAKE_BINARY_DIR}/prompts_png.c COPYONLY)
target_sources(sms PRIVATE ${CMAKE_BINARY_DIR}/prompts_png.c)
set_source_files_properties(${SMS_SWITCH_DIR}/src/switch_main.cpp TARGET_DIRECTORY sms PROPERTIES COMPILE_DEFINITIONS
  "SMS_SWITCH_APP=\"${SMS_SWITCH_APP}\";SMS_SWITCH_TITLE=\"${SMS_SWITCH_TITLE}\"")
# Every thread spread over cores 0-2 (switch/src/performance.cpp).
target_link_options(sms PRIVATE -Wl,--wrap=threadCreate)
# main with an argument list even when the loader gives none (switch_main.cpp)
target_link_options(sms PRIVATE -Wl,--wrap=main)

# Mesa (OpenGL over nouveau). Its pkg-config files carry an unexpanded
# $(DEVKITPRO), so the libraries are named directly; order matters.
# tools/mesa/build-mesa.sh builds it with the disk shader cache into
# build/mesa/; when that is there, everything that links EGL and glapi
# (SDL2's target too) takes those instead of the installed switch-mesa.
set(SMS_SWITCH_MESA_DIR "${CMAKE_BINARY_DIR}/mesa" CACHE PATH "Mesa built by tools/mesa/build-mesa.sh")
if(EXISTS "${SMS_SWITCH_MESA_DIR}/lib/libEGL.a")
  set(SMS_SWITCH_EGL "${SMS_SWITCH_MESA_DIR}/lib/libEGL.a")
  set(SMS_SWITCH_GLAPI "${SMS_SWITCH_MESA_DIR}/lib/libglapi.a")
  file(READ "${SMS_SWITCH_MESA_DIR}/cache-id" _mesa_id)
  string(STRIP "${_mesa_id}" _mesa_id)
  message(STATUS "sms-switch: Mesa with the shader cache (${_mesa_id})")
  foreach(_t SDL2::SDL2-static SDL2::SDL2)
    if(TARGET ${_t})
      get_target_property(_libs ${_t} INTERFACE_LINK_LIBRARIES)
      if(_libs)
        list(TRANSFORM _libs REPLACE "^(-l)?EGL$" "${SMS_SWITCH_EGL}")
        list(TRANSFORM _libs REPLACE "^(-l)?glapi$" "${SMS_SWITCH_GLAPI};z")
        set_target_properties(${_t} PROPERTIES INTERFACE_LINK_LIBRARIES "${_libs}")
      endif()
    endif()
  endforeach()
else()
  set(SMS_SWITCH_EGL EGL)
  set(SMS_SWITCH_GLAPI glapi)
  message(STATUS "sms-switch: the installed switch-mesa (no shader cache; tools/mesa/build-mesa.sh builds one)")
endif()
target_link_libraries(sms PRIVATE ${SMS_SWITCH_EGL} ${SMS_SWITCH_GLAPI} drm_nouveau z)

# dkgl (switch/src/dkgl): the renderer's GL calls on deko3d, with
# SMS_SWITCH_RENDERER=deko3d. It compiles shaders with uam built as a library
# (tools/uam/build-uam.sh, into build/uam/); without it, only Mesa's GL.
set(SMS_SWITCH_UAM_DIR "${CMAKE_BINARY_DIR}/uam" CACHE PATH "libuam built by tools/uam/build-uam.sh")
# deko3d with this project's fixes (tools/deko3d/build-deko3d.sh, into
# build/deko3d/) when built, else devkitPro's: its blend states use the
# colour's destination factor for alpha too (the game's shadows draw solid)
set(SMS_SWITCH_DEKO3D_DIR "${CMAKE_BINARY_DIR}/deko3d" CACHE PATH "deko3d built by tools/deko3d/build-deko3d.sh")
if(EXISTS "${SMS_SWITCH_DEKO3D_DIR}/lib/libdeko3d.a")
  set(SMS_SWITCH_DEKO3D ${SMS_SWITCH_DEKO3D_DIR}/lib/libdeko3d.a)
  set(SMS_SWITCH_DEKO3DD ${SMS_SWITCH_DEKO3D_DIR}/lib/libdeko3dd.a)
  message(STATUS "sms-switch: deko3d from ${SMS_SWITCH_DEKO3D_DIR}")
else()
  set(SMS_SWITCH_DEKO3D deko3d)
  set(SMS_SWITCH_DEKO3DD deko3dd)
  message(WARNING "sms-switch: devkitPro's deko3d (tools/deko3d/build-deko3d.sh builds the fixed one): shadows draw solid")
endif()
if(EXISTS "${SMS_SWITCH_UAM_DIR}/lib/libuam.a")
  add_library(dkgl STATIC
    ${SMS_SWITCH_DIR}/src/dkgl/dkgl_api.cpp
    ${SMS_SWITCH_DIR}/src/dkgl/dkgl_device.cpp
    ${SMS_SWITCH_DIR}/src/dkgl/dkgl_draw.cpp
    ${SMS_SWITCH_DIR}/src/dkgl/dkgl_glsl.cpp
    ${SMS_SWITCH_DIR}/src/dkgl/dkgl_program.cpp
    ${SMS_SWITCH_DIR}/src/dkgl/dkgl_sdl.cpp
    ${SMS_SWITCH_DIR}/src/dkgl/dkgl_test.cpp
    ${SMS_SWITCH_DIR}/src/dkgl/dkgl_texture.cpp)
  # (SDL itself is linked by the program)
  target_include_directories(dkgl PRIVATE ${SMS_SWITCH_UAM_DIR}/include ${DEVKITPRO}/portlibs/switch/include/SDL2)
  target_compile_features(dkgl PRIVATE cxx_std_17)
  # (deko3d itself: the program's choice, deko3dd being its checked build)
  target_link_libraries(dkgl PUBLIC ${SMS_SWITCH_UAM_DIR}/lib/libuam.a)
  # SDL's GL calls go to dkgl when it is chosen (dkgl_sdl.cpp)
  set(SMS_SWITCH_DKGL_WRAPS
    -Wl,--wrap=SDL_GL_CreateContext -Wl,--wrap=SDL_GL_MakeCurrent -Wl,--wrap=SDL_GL_SwapWindow
    -Wl,--wrap=SDL_GL_SetSwapInterval -Wl,--wrap=SDL_GL_GetSwapInterval -Wl,--wrap=SDL_GL_DeleteContext
    -Wl,--wrap=SDL_GL_GetProcAddress -Wl,--wrap=SDL_GL_GetDrawableSize -Wl,--wrap=SDL_EGL_CreateSurface
    -Wl,--wrap=SDL_EGL_DestroySurface -Wl,--wrap=nwindowSetDimensions)
  target_link_libraries(sms PRIVATE dkgl ${SMS_SWITCH_DEKO3D})
  target_link_options(sms PRIVATE ${SMS_SWITCH_DKGL_WRAPS})
  target_compile_definitions(sms PRIVATE SMS_SWITCH_DKGL=1)
  message(STATUS "sms-switch: the deko3d renderer (dkgl) with ${SMS_SWITCH_UAM_DIR}/lib/libuam.a")
else()
  message(STATUS "sms-switch: no libuam (tools/uam/build-uam.sh): Mesa's GL only")
endif()

# The icon (homebrew menu, HOME menu): SMS_SWITCH_ICON=
#   steamgriddb (default)  the box art from SteamGridDB, fetched at build time,
#                          else the disc's memory-card Mario (with a disc);
#                          Sunshine's art, so for Eclipse only the disc's
#   disc                   the memory-card Mario from SMS_SWITCH_BUNDLE_DISC
#   default                libnx's icon (what CI and releases use)
#   /path/to/icon.jpg      your own 256x256 JPEG
# (tools/make_icon.py; no art is kept in this repository.)
set(SMS_SWITCH_ICON "steamgriddb" CACHE STRING "steamgriddb, disc, default, or a 256x256 JPEG")
set(_icon_args "")
if(SMS_SWITCH_ICON STREQUAL "steamgriddb" AND NOT SMS_ECLIPSE)
  list(APPEND _icon_args --steamgriddb)
endif()
if(SMS_SWITCH_ICON MATCHES "^(steamgriddb|disc)$" AND SMS_SWITCH_BUNDLE_DISC AND EXISTS "${SMS_SWITCH_BUNDLE_DISC}")
  list(APPEND _icon_args --disc "${SMS_SWITCH_BUNDLE_DISC}")
endif()
set(SMS_SWITCH_ICON_FILE ${NX_DEFAULT_ICON})
if(SMS_SWITCH_ICON MATCHES "\\.(jpg|jpeg)$")
  # Copied to icon.jpg, which the NSP (tools/make-nsp.sh) takes as well.
  configure_file("${SMS_SWITCH_ICON}" ${CMAKE_BINARY_DIR}/icon.jpg COPYONLY)
  set(SMS_SWITCH_ICON_FILE ${CMAKE_BINARY_DIR}/icon.jpg)
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
  NAME "${SMS_SWITCH_TITLE}"
  AUTHOR "sms-switch"
  VERSION "${PROJECT_VERSION}")
nx_create_nro(sms
  OUTPUT ${CMAKE_BINARY_DIR}/${SMS_SWITCH_APP}.nro
  NACP sms.nacp
  ICON ${SMS_SWITCH_ICON_FILE})

# sms-standalone.nro (optional; sms-eclipse-standalone.nro with SMS_ECLIPSE and
# the Eclipse disc): the same program with your disc image inside
# its RomFS, so the SD card needs no image beside it. For your own use only:
# it contains the game, so it must never be shared (CI never builds it).
#   -DSMS_SWITCH_BUNDLE_DISC=/path/to/GMSE01.iso   (.iso, .gcm or .ciso)
set(_standalone ${CMAKE_BINARY_DIR}/${SMS_SWITCH_APP}-standalone.nro)
set(SMS_SWITCH_BUNDLE_DISC "" CACHE FILEPATH "Disc image to pack into sms-standalone.nro (personal use only)")
#   -DSMS_SWITCH_BUNDLE_TEXTURES=/path/to/pack   an HD texture pack (the
#   Switch's copy, tools/texpack/make-switch-pack.sh) packed beside it, as
#   romfs:/textures: one file to copy instead of thousands
set(SMS_SWITCH_BUNDLE_TEXTURES "" CACHE PATH "HD texture pack to pack into sms-standalone.nro")
# A standalone NRO left from another image or pack (or none now) would be
# stale: elf2nro reruns only when the program changes.
if(NOT "${SMS_SWITCH_BUNDLE_DISC}|${SMS_SWITCH_BUNDLE_TEXTURES}" STREQUAL "${_SMS_SWITCH_BUNDLED}")
  file(REMOVE ${_standalone})
  set(_SMS_SWITCH_BUNDLED "${SMS_SWITCH_BUNDLE_DISC}|${SMS_SWITCH_BUNDLE_TEXTURES}" CACHE INTERNAL "")
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
  # The RomFS holds the image, as romfs:/disc<ext>, and the texture pack, as
  # romfs:/textures/ (links, not copies; build_romfs follows them).
  set(_romfs ${CMAKE_BINARY_DIR}/standalone-romfs)
  file(REMOVE_RECURSE ${_romfs})
  file(MAKE_DIRECTORY ${_romfs})
  file(CREATE_LINK "${SMS_SWITCH_BUNDLE_DISC}" ${_romfs}/disc${_disc_ext} SYMBOLIC)
  if(SMS_SWITCH_BUNDLE_TEXTURES)
    if(NOT IS_DIRECTORY "${SMS_SWITCH_BUNDLE_TEXTURES}")
      message(FATAL_ERROR "SMS_SWITCH_BUNDLE_TEXTURES: ${SMS_SWITCH_BUNDLE_TEXTURES} is not a directory")
    endif()
    file(CREATE_LINK "${SMS_SWITCH_BUNDLE_TEXTURES}" ${_romfs}/textures SYMBOLIC)
    message(STATUS "sms-switch: and the texture pack ${SMS_SWITCH_BUNDLE_TEXTURES}")
  endif()
  nx_create_nro(sms_standalone
    TARGET sms
    OUTPUT ${_standalone}
    NACP sms.nacp
    ICON ${SMS_SWITCH_ICON_FILE}
    ROMFS ${_romfs})
  message(STATUS "sms-switch: also building ${SMS_SWITCH_APP}-standalone.nro with ${SMS_SWITCH_BUNDLE_DISC} inside")
endif()

# gx_selftest.nro: the port's GX self-test on the Switch's GL stack (diagnosis).
option(SMS_SWITCH_GX_SELFTEST "Also build gx_selftest.nro" ON)
if(SMS_ECLIPSE)
  set(SMS_SWITCH_GX_SELFTEST OFF) # the same as the plain build's
endif()
if(SMS_SWITCH_GX_SELFTEST)
  set(_gx ${CMAKE_CURRENT_SOURCE_DIR}/sms-port/platform/gx)
  add_executable(gx_selftest
    ${_gx}/tests/gx_selftest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/sms-port/platform/misc/msl_math.c
    ${CMAKE_CURRENT_SOURCE_DIR}/sms-port/platform/misc/port_cheats.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/sms-port/platform/misc/port_warp.cpp
    ${SMS_SWITCH_DIR}/src/dev_link.cpp
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
  target_link_libraries(gx_selftest PRIVATE sms_gx ${SMS_SWITCH_EGL} ${SMS_SWITCH_GLAPI} drm_nouveau z)
  if(TARGET dkgl)
    # SMS_SWITCH_RENDERER=deko3d runs the self-test on dkgl
    # deko3d's checked build: it reports misuse in the log (dkgl's debug callback)
    target_link_libraries(gx_selftest PRIVATE dkgl ${SMS_SWITCH_DEKO3DD})
    target_link_options(gx_selftest PRIVATE ${SMS_SWITCH_DKGL_WRAPS})
    target_compile_definitions(gx_selftest PRIVATE SMS_SWITCH_DKGL=1)
  endif()
  # (with a NACP: emulators such as Ryubing load no NRO without one)
  nx_generate_nacp(gx_selftest.nacp NAME "gx_selftest" AUTHOR "sms-switch" VERSION "${PROJECT_VERSION}")
  nx_create_nro(gx_selftest OUTPUT ${CMAKE_BINARY_DIR}/gx_selftest.nro NACP gx_selftest.nacp)
endif()
