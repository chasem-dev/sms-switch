/* dlfcn.h for the Switch: no dynamic loading. The port looks up a few SDL
 * functions by name (audio, event push); dlsym answers from the table in
 * switch/src/posix_shims.cpp, which the static SDL2 provides. */
#ifndef SMS_SWITCH_DLFCN_H
#define SMS_SWITCH_DLFCN_H
#ifdef __cplusplus
extern "C" {
#endif
#define RTLD_LAZY 1
#define RTLD_NOW 2
#define RTLD_GLOBAL 0x100
#define RTLD_LOCAL 0
#define RTLD_DEFAULT ((void*)0)
void* dlopen(const char* name, int flags);
void* dlsym(void* handle, const char* name);
int dlclose(void* handle);
char* dlerror(void);
typedef struct {
	const char* dli_fname;
	void* dli_fbase;
	const char* dli_sname;
	void* dli_saddr;
} Dl_info;
int dladdr(const void* addr, Dl_info* info);
#ifdef __cplusplus
}
#endif
#endif
