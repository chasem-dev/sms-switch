// Performance on the console: which cores the program's threads run on.
//
// Threads. libnx creates every pthread (and SDL every thread) on the
// process's default core, core 0, and pthreads at the lowest application
// priority (0x3B). The port runs its OS threads, the GL thread, the audio
// mixer and the retrace timer as host threads, so all of them shared core 0
// with the game, the GL thread only getting the core when the game waited.
// threadCreate is wrapped (-Wl,--wrap=threadCreate, which also covers libnx's
// own pthread_create and SDL): each new thread may run on any of cores 0-2,
// and pthreads get the main thread's priority. The two threads that do most
// of the work get a core each to start from: the main thread, which runs the
// game, core 0, and the GL thread (the driver's work, sms_switch_gl_thread)
// core 1; every other thread starts on core 2 or 0 in turn. Horizon does not
// share a core between threads of the same priority by time: a thread keeps
// it until it waits, and one that is woken there waits for it unless another
// core is idle. So the texture pack's background work
// (sms_switch_background_thread) runs below the game's priority, where the
// game's threads take a core from it rather than wait for it.
// SMS_SWITCH_SPREAD_THREADS=0 leaves all of this to libnx.
//
// Clocks: the console's own (CPU 1020 MHz, GPU 307-384 MHz handheld, 768 MHz
// docked); the port never changes them. Raised clocks (CPU 1785 MHz, what the
// system's load boost uses) ran the fan hard for a few more frames a second.
#include <switch.h>
#include <atomic>
#include <stdio.h>
#include <stdlib.h>

extern "C" Result __real_threadCreate(Thread* t, ThreadFunc entry, void* arg, void* stack_mem, size_t stack_sz,
                                      int prio, int cpuid);

namespace {
constexpr u32 kAllCores = 0x7; // the cores applications may use
constexpr int kMainPriority = 0x2C;
constexpr int kBackgroundPriority = 0x2E;
constexpr int kGlCore = 1;
constexpr int kDspCore = 2;
constexpr int kOtherCores[] = {2, 0}; // the cores other threads start on, in turn
std::atomic<int> g_next_core(0);
std::atomic<unsigned> g_threads_created(0); // (diagnostics.cpp's memory lines)

bool spread_threads()
{
	static int on = -1;
	if (on < 0) {
		const char* e = getenv("SMS_SWITCH_SPREAD_THREADS");
		on = !(e && *e == '0');
	}
	return on != 0;
}

} // namespace

extern "C" void sms_switch_profile_created(Handle h, void* entry); // profiler.cpp

extern "C" unsigned sms_switch_threads_created(void) { return g_threads_created.load(std::memory_order_relaxed); }

extern "C" Result __wrap_threadCreate(Thread* t, ThreadFunc entry, void* arg, void* stack_mem, size_t stack_sz,
                                      int prio, int cpuid)
{
	g_threads_created.fetch_add(1, std::memory_order_relaxed);
	if (!spread_threads()) {
		const Result r = __real_threadCreate(t, entry, arg, stack_mem, stack_sz, prio, cpuid);
		if (R_SUCCEEDED(r))
			sms_switch_profile_created(t->handle, (void*)entry);
		return r;
	}
	if (prio == 0x3B) // libnx's pthread_create
		prio = kMainPriority;
	const bool spread = cpuid == -2;
	const int core = spread ? kOtherCores[g_next_core.fetch_add(1) % 2] : cpuid;
	Result rc = __real_threadCreate(t, entry, arg, stack_mem, stack_sz, prio, core);
	if (R_SUCCEEDED(rc))
		sms_switch_profile_created(t->handle, (void*)entry);
	if (R_SUCCEEDED(rc) && spread)
		svcSetThreadCoreMask(t->handle, core, kAllCores);
	static int trace = -1;
	if (trace < 0) {
		const char* e = getenv("SMS_SWITCH_TRACE_THREADS");
		trace = e && *e == '1';
	}
	if (trace && R_SUCCEEDED(rc)) {
		// where each thread's stack is: an emulator's bad address in the
		// stack region names its thread
		char line[128];
		const int n = snprintf(line, sizeof line, "[switch] thread %p: stack %p-%p (mirror of %p), entry %p",
		                       (void*)t, t->stack_mirror, (char*)t->stack_mirror + t->stack_sz, t->stack_mem,
		                       (void*)entry);
		svcOutputDebugString(line, size_t(n));
	}
	return rc;
}

extern "C" void sms_switch_profile_thread(int slot); // profiler.cpp
void sms_switch_profile_add_current(const char* label); // profiler.cpp

extern "C" void sms_switch_spread_main_thread(void)
{
	sms_switch_profile_thread(0);
	svcSetThreadCoreMask(CUR_THREAD_HANDLE, 0, kAllCores);
}

// The port's GL thread, as it starts (gx_glthread.cpp).
extern "C" void sms_switch_gl_thread(void)
{
	sms_switch_profile_thread(1);
	if (spread_threads())
		svcSetThreadCoreMask(CUR_THREAD_HANDLE, kGlCore, kAllCores);
}

// The DSP's mixing thread (sms-port's audio/dsp_hle.cpp): the audio thread
// waits for each subframe it mixes, so it starts on the core the game's own
// threads leave least busy.
extern "C" void sms_switch_dsp_thread(void)
{
	sms_switch_profile_add_current("dsp");
	if (spread_threads())
		svcSetThreadCoreMask(CUR_THREAD_HANDLE, kDspCore, kAllCores);
}

// The graphics worker (sms-port's gx_thread.h, SMS_GX_THREAD=1): the GX
// front end beside the game, on the core the GL thread does not use.
extern "C" void sms_switch_gx_thread(void)
{
	sms_switch_profile_add_current("gx");
	if (spread_threads())
		svcSetThreadCoreMask(CUR_THREAD_HANDLE, kDspCore, kAllCores);
}

// A movie frame (the THP player's decoding thread, below the game's priority):
// it touches only the movie's own buffers until it is done, so the game's
// other threads have the CPU meanwhile (sms-port's port_os_host_work_begin)
// and the decoding runs on another core beside the game instead of in the
// game's turn (when a late frame steps down for it, a quarter frame at most).
// SMS_SWITCH_THP_AWAY=0 decodes on the CPU as before.
extern "C" s32 __real_THPVideoDecode(void* file, void* tileY, void* tileU, void* tileV, void* work);
extern "C" void* port_os_host_work_begin(void);
extern "C" void port_os_host_work_end(void* away);

extern "C" s32 __wrap_THPVideoDecode(void* file, void* tileY, void* tileU, void* tileV, void* work)
{
	static int on = -1;
	if (on < 0) {
		const char* e = getenv("SMS_SWITCH_THP_AWAY");
		on = !(e && *e == '0');
	}
	void* away = on ? port_os_host_work_begin() : nullptr;
	const s32 r = __real_THPVideoDecode(file, tileY, tileU, tileV, work);
	port_os_host_work_end(away);
	return r;
}

// A thread of work that may wait for idle time (gx_hires.cpp: the texture
// pack's levels it makes itself).
extern "C" void sms_switch_background_thread(void)
{
	if (spread_threads())
		svcSetThreadPriority(CUR_THREAD_HANDLE, kBackgroundPriority);
}
