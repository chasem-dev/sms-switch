// dkgl: the OpenGL subset sms_gx draws with (platform/gx/src/gl_funcs.h, GL
// 4.3 core), on deko3d, the Switch's own low-level graphics API. Mesa's
// nouveau driver spent ~30 us of CPU per draw validating GL state; here a draw
// writes only the deko3d commands for the state that changed.
//
// It is made for one renderer, not as a general GL: one context, called from
// one thread at a time, 2D textures, the formats and calls sms_gx uses.
// Shaders are compiled with deko3d's compiler (uam, in the program) after the
// GLSL adapter (dkgl_glsl.h); compiled programs are kept on the SD card.
//
// SDL is given its GL calls through switch/src/dkgl/dkgl_sdl.cpp (linked with
// --wrap), so sms_gx's window code is unchanged.
#ifndef SMS_SWITCH_DKGL_H
#define SMS_SWITCH_DKGL_H

#include <stddef.h>

namespace dkgl {

// Starts the device and a swapchain on the default window, w x h.
bool init(int width, int height);
bool ready();  // init has made the device and swapchain
void shutdown();
// A GL entry point by name (for SDL_GL_GetProcAddress), or null.
void* getProc(const char* name);
// Shows the default framebuffer.
void swap();
void setSwapInterval(int interval);
// Where compiled programs are cached (a directory), and a prebuilt pack of
// them to read first (a file, or null).
void setShaderCache(const char* dir, const char* pack);
// dkgl's own pixel checks (gx_selftest.nro), after init: the number failed.
int runChecks();

}  // namespace dkgl

#endif
