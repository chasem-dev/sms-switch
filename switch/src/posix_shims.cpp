// POSIX pieces the PC port uses that newlib on the Switch lacks.
#include <switch.h>
#include <SDL.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <unwind.h>
#include "low_memory.h"


// --- dlfcn: the SDL functions the port looks up by name --------------------
namespace {
struct Symbol {
	const char* name;
	void* address;
};
const Symbol kSymbols[] = {
	{"SDL_InitSubSystem", (void*)&SDL_InitSubSystem},
	{"SDL_OpenAudioDevice", (void*)&SDL_OpenAudioDevice},
	{"SDL_PauseAudioDevice", (void*)&SDL_PauseAudioDevice},
	{"SDL_CloseAudioDevice", (void*)&SDL_CloseAudioDevice},
	{"SDL_GetError", (void*)&SDL_GetError},
	{"SDL_PushEvent", (void*)&SDL_PushEvent},
	{"SDL_QuitSubSystem", (void*)&SDL_QuitSubSystem},
};
const char* g_dlerror;
char g_self;
} // namespace

extern "C" void* dlopen(const char*, int) { return &g_self; }
extern "C" int dlclose(void*) { return 0; }
extern "C" void* dlsym(void*, const char* name)
{
	for (const Symbol& s : kSymbols)
		if (!strcmp(s.name, name))
			return s.address;
	g_dlerror = "symbol not available on the Switch";
	return NULL;
}
extern "C" char* dlerror(void)
{
	const char* e = g_dlerror;
	g_dlerror = NULL;
	return (char*)e;
}
extern "C" int dladdr(const void* addr, Dl_info* info)
{
	const LowMemoryInfo* m = sms_switch_low_memory();
	const u64 base = m->image_base;
	if ((u64)addr < base || (u64)addr >= base + (m->image_size ? m->image_size : 0x10000000))
		return 0;
	info->dli_fname = "sms.nro";
	info->dli_fbase = (void*)base;
	info->dli_sname = NULL;
	info->dli_saddr = NULL;
	return 1;
}

// --- execinfo: unwinding with the .eh_frame tables --------------------------
namespace {
struct Trace {
	void** frames;
	int capacity, count;
};
_Unwind_Reason_Code trace_one(struct _Unwind_Context* ctx, void* arg)
{
	Trace* t = (Trace*)arg;
	if (t->count >= t->capacity)
		return _URC_END_OF_STACK;
	uintptr_t pc = _Unwind_GetIP(ctx);
	if (!pc)
		return _URC_END_OF_STACK;
	t->frames[t->count++] = (void*)pc;
	return _URC_NO_REASON;
}
} // namespace

extern "C" int backtrace(void** frames, int capacity)
{
	Trace t = {frames, capacity, 0};
	_Unwind_Backtrace(trace_one, &t);
	return t.count;
}

// "sms.nro+0xOFFSET": resolve with addr2line on build/sms.elf (offsets are
// link addresses: the ELF is linked at 0).
extern "C" char** backtrace_symbols(void* const* frames, int count)
{
	enum { kLen = 48 };
	if (count <= 0)
		return NULL;
	char** out = (char**)malloc(count * (sizeof(char*) + kLen));
	if (!out)
		return NULL;
	char* text = (char*)(out + count);
	const uintptr_t base = sms_switch_low_memory()->image_base;
	for (int i = 0; i < count; i++) {
		out[i] = text + i * kLen;
		uintptr_t pc = (uintptr_t)frames[i];
		if (pc >= base)
			snprintf(out[i], kLen, "sms.nro+0x%lx", (unsigned long)(pc - base));
		else
			snprintf(out[i], kLen, "%p", frames[i]);
	}
	return out;
}

extern "C" void backtrace_symbols_fd(void* const* frames, int count, int fd)
{
	char** lines = backtrace_symbols(frames, count);
	for (int i = 0; lines && i < count; i++) {
		write(fd, lines[i], strlen(lines[i]));
		write(fd, "\n", 1);
	}
	free(lines);
}

// --- signals: Horizon has none; crashes go to the exception handler ---------
extern "C" int sigaction(int, const struct sigaction*, struct sigaction* old)
{
	if (old)
		memset(old, 0, sizeof *old);
	return 0;
}

extern "C" int sigaltstack(const stack_t*, stack_t* old)
{
	if (old)
		memset(old, 0, sizeof *old);
	return 0;
}

// --- identity ---------------------------------------------------------------
extern "C" uid_t getuid(void) { return 0; }

#include <sys/utsname.h>
extern "C" int uname(struct utsname* name)
{
	memset(name, 0, sizeof *name);
	strcpy(name->sysname, "Horizon");
	strcpy(name->nodename, "switch");
	const u32 v = hosversionGet();
	snprintf(name->release, sizeof name->release, "%u.%u.%u%s", HOSVER_MAJOR(v), HOSVER_MINOR(v), HOSVER_MICRO(v),
	         hosversionIsAtmosphere() ? " (Atmosphere)" : "");
	strcpy(name->version, "libnx");
	strcpy(name->machine, "aarch64");
	return 0;
}

// --- local time ---------------------------------------------------------------
// The console's time zone offset from UTC at `t`, in seconds (newlib's
// struct tm has no tm_gmtoff, and its localtime knows no time zone).
extern "C" long sms_switch_utc_offset(time_t t)
{
	TimeCalendarTime cal;
	TimeCalendarAdditionalInfo info;
	if (R_SUCCEEDED(timeToCalendarTimeWithMyRule((u64)t, &cal, &info)))
		return info.offset;
	return 0;
}

// --- memory probing (the port's crash reports) --------------------------------
extern "C" bool sms_switch_read_memory(void* to, uintptr_t from, size_t size)
{
	for (uintptr_t at = from; at < from + size;) {
		MemoryInfo mi;
		u32 pi;
		if (R_FAILED(svcQueryMemory(&mi, &pi, at)) || !(mi.perm & Perm_R) || mi.addr + mi.size <= at)
			return false;
		at = mi.addr + mi.size;
	}
	memcpy(to, (const void*)from, size);
	return true;
}

// --- files and system information ---------------------------------------------
// newlib has no pread; seek and read under one lock (the disc is read from
// several threads).
namespace {
Mutex g_pread_lock;
}
extern "C" ssize_t pread(int fd, void* buf, size_t count, off_t offset)
{
	mutexLock(&g_pread_lock);
	ssize_t n = -1;
	off_t was = lseek(fd, 0, SEEK_CUR);
	if (was != (off_t)-1 && lseek(fd, offset, SEEK_SET) != (off_t)-1) {
		n = read(fd, buf, count);
		lseek(fd, was, SEEK_SET);
	}
	mutexUnlock(&g_pread_lock);
	return n;
}

extern "C" long sysconf(int name)
{
	switch (name) {
	case _SC_PAGESIZE: return 0x1000;
	case _SC_NPROCESSORS_CONF: return 4;
	case _SC_NPROCESSORS_ONLN: return 3; // cores 0-2 run applications
	default: errno = EINVAL; return -1;
	}
}
