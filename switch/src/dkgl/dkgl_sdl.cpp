// SDL's GL calls, given to dkgl (SMS_SWITCH_RENDERER=deko3d) through
// -Wl,--wrap, so sms_gx's window code runs unchanged on deko3d:
// - the GL context is dkgl's, its entry points come from dkgl::getProc, and
//   the swap presents dkgl's default framebuffer
// - the Switch's SDL makes an EGL (Mesa) surface on the default window with
//   every window, and remakes it, with new window dimensions, on a docking
//   change; the window's buffers are deko3d's swapchain's, so SDL gets a
//   surface of no consequence and the dimensions stay the swapchain's
#include <SDL.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include "dkgl.h"

extern "C" {
SDL_GLContext __real_SDL_GL_CreateContext(SDL_Window* window);
int __real_SDL_GL_MakeCurrent(SDL_Window* window, SDL_GLContext context);
void __real_SDL_GL_SwapWindow(SDL_Window* window);
int __real_SDL_GL_SetSwapInterval(int interval);
int __real_SDL_GL_GetSwapInterval(void);
void __real_SDL_GL_DeleteContext(SDL_GLContext context);
void* __real_SDL_GL_GetProcAddress(const char* proc);
void __real_SDL_GL_GetDrawableSize(SDL_Window* window, int* w, int* h);
void* __real_SDL_EGL_CreateSurface(void* video, void* nativeWindow);
void __real_SDL_EGL_DestroySurface(void* video, void* surface);
Result __real_nwindowSetDimensions(NWindow* nw, u32 width, u32 height);
}

namespace {
int s_context;  // what SDL_GL_CreateContext returns: only compared, never used

// Read each time: libnx sizes the default window before main, before
// env.txt is read.
bool deko3d() {
    const char* e = getenv("SMS_SWITCH_RENDERER");
    return e && !strcmp(e, "deko3d");
}
}  // namespace

extern "C" bool sms_switch_deko3d(void) { return deko3d(); }

extern "C" SDL_GLContext __wrap_SDL_GL_CreateContext(SDL_Window* window) {
    if (!deko3d()) return __real_SDL_GL_CreateContext(window);
    int w = 1280, h = 720;
    SDL_GetWindowSize(window, &w, &h);
    if (!dkgl::init(w, h)) {
        SDL_SetError("deko3d could not start");
        return nullptr;
    }
    return &s_context;
}

extern "C" int __wrap_SDL_GL_MakeCurrent(SDL_Window* window, SDL_GLContext context) {
    return deko3d() ? 0 : __real_SDL_GL_MakeCurrent(window, context);
}

extern "C" void __wrap_SDL_GL_SwapWindow(SDL_Window* window) {
    if (deko3d()) dkgl::swap();
    else __real_SDL_GL_SwapWindow(window);
}

extern "C" int __wrap_SDL_GL_SetSwapInterval(int interval) {
    if (!deko3d()) return __real_SDL_GL_SetSwapInterval(interval);
    dkgl::setSwapInterval(interval);
    return 0;
}

extern "C" int __wrap_SDL_GL_GetSwapInterval(void) {
    return deko3d() ? 1 : __real_SDL_GL_GetSwapInterval();
}

extern "C" void __wrap_SDL_GL_DeleteContext(SDL_GLContext context) {
    if (deko3d()) dkgl::shutdown();
    else __real_SDL_GL_DeleteContext(context);
}

extern "C" void* __wrap_SDL_GL_GetProcAddress(const char* proc) {
    return deko3d() ? dkgl::getProc(proc) : __real_SDL_GL_GetProcAddress(proc);
}

extern "C" void __wrap_SDL_GL_GetDrawableSize(SDL_Window* window, int* w, int* h) {
    if (deko3d()) SDL_GetWindowSize(window, w, h);
    else __real_SDL_GL_GetDrawableSize(window, w, h);
}

extern "C" void* __wrap_SDL_EGL_CreateSurface(void* video, void* nativeWindow) {
    // any non-null surface: SDL only checks it
    return deko3d() ? reinterpret_cast<void*>(&s_context) : __real_SDL_EGL_CreateSurface(video, nativeWindow);
}

extern "C" void __wrap_SDL_EGL_DestroySurface(void* video, void* surface) {
    if (!deko3d()) __real_SDL_EGL_DestroySurface(video, surface);
}

extern "C" Result __wrap_nwindowSetDimensions(NWindow* nw, u32 width, u32 height) {
    if (deko3d() && dkgl::ready()) return 0;  // the swapchain's images set them
    return __real_nwindowSetDimensions(nw, width, height);
}
