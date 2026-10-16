// Performance on the console: which cores the program's threads run on, and
// the CPU and GPU clocks.
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
// Clocks. Homebrew runs at the console's default profile (CPU 1020 MHz, GPU
// 307-384 MHz handheld, 768 MHz docked). The game's own CPU work is native
// code, but the GX layer and Mesa add a lot of driver work per draw, so the
// CPU goes to 1785 MHz (what the system's own load boost uses) and the GPU to
// 460.8 MHz handheld / 768 MHz docked, through clkrst (pcv before 8.0.0).
// The system resets them on a docking change, so a thread re-applies them.
// SMS_SWITCH_CLOCKS=0 keeps the default profile; SMS_SWITCH_CPU_MHZ and
// SMS_SWITCH_GPU_MHZ override the rates.
#include <switch.h>
#include <atomic>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" Result __real_threadCreate(Thread* t, ThreadFunc entry, void* arg, void* stack_mem, size_t stack_sz,
                                      int prio, int cpuid);

namespace {
constexpr u32 kAllCores = 0x7; // the cores applications may use
constexpr int kMainPriority = 0x2C;
constexpr int kBackgroundPriority = 0x2E;
constexpr int kGlCore = 1;
constexpr int kOtherCores[] = {2, 0}; // the cores other threads start on, in turn
std::atomic<int> g_next_core(0);

bool spread_threads()
{
	static int on = -1;
	if (on < 0) {
		const char* e = getenv("SMS_SWITCH_SPREAD_THREADS");
		on = !(e && *e == '0');
	}
	return on != 0;
}

u32 env_mhz(const char* name, u32 fallback)
{
	const char* e = getenv(name);
	const long v = e ? strtol(e, nullptr, 10) : 0;
	return v > 0 ? u32(v) : fallback;
}
} // namespace

extern "C" Result __wrap_threadCreate(Thread* t, ThreadFunc entry, void* arg, void* stack_mem, size_t stack_sz,
                                      int prio, int cpuid)
{
	if (!spread_threads())
		return __real_threadCreate(t, entry, arg, stack_mem, stack_sz, prio, cpuid);
	if (prio == 0x3B) // libnx's pthread_create
		prio = kMainPriority;
	const bool spread = cpuid == -2;
	const int core = spread ? kOtherCores[g_next_core.fetch_add(1) % 2] : cpuid;
	Result rc = __real_threadCreate(t, entry, arg, stack_mem, stack_sz, prio, core);
	if (R_SUCCEEDED(rc) && spread)
		svcSetThreadCoreMask(t->handle, core, kAllCores);
	return rc;
}

extern "C" void sms_switch_spread_main_thread(void)
{
	svcSetThreadCoreMask(CUR_THREAD_HANDLE, 0, kAllCores);
}

// The port's GL thread, as it starts (gx_glthread.cpp).
extern "C" void sms_switch_gl_thread(void)
{
	if (spread_threads())
		svcSetThreadCoreMask(CUR_THREAD_HANDLE, kGlCore, kAllCores);
}

// A thread of work that may wait for idle time (gx_hires.cpp: the texture
// pack's levels it makes itself).
extern "C" void sms_switch_background_thread(void)
{
	if (spread_threads())
		svcSetThreadPriority(CUR_THREAD_HANDLE, kBackgroundPriority);
}

// --- clocks -----------------------------------------------------------------
namespace {
bool g_clocks_open;
bool g_use_clkrst;
Thread g_clock_thread;
std::atomic<bool> g_clock_stop(false);

void set_rate(bool gpu, u32 hz)
{
	const PcvModuleId module = gpu ? PcvModuleId_GPU : PcvModuleId_CpuBus;
	if (g_use_clkrst) {
		ClkrstSession s;
		if (R_SUCCEEDED(clkrstOpenSession(&s, module, 3))) {
			clkrstSetClockRate(&s, hz);
			clkrstCloseSession(&s);
		}
	} else {
		pcvSetClockRate(gpu ? PcvModule_GPU : PcvModule_CpuBus, hz);
	}
}

u32 get_rate(bool gpu)
{
	const PcvModuleId module = gpu ? PcvModuleId_GPU : PcvModuleId_CpuBus;
	u32 hz = 0;
	if (g_use_clkrst) {
		ClkrstSession s;
		if (R_SUCCEEDED(clkrstOpenSession(&s, module, 3))) {
			clkrstGetClockRate(&s, &hz);
			clkrstCloseSession(&s);
		}
	} else {
		pcvGetClockRate(gpu ? PcvModule_GPU : PcvModule_CpuBus, &hz);
	}
	return hz;
}

void wanted(u32* cpu_hz, u32* gpu_hz)
{
	const bool docked = appletGetOperationMode() == AppletOperationMode_Console;
	*cpu_hz = env_mhz("SMS_SWITCH_CPU_MHZ", 1785) * 1000000u;
	*gpu_hz = env_mhz("SMS_SWITCH_GPU_MHZ", docked ? 768 : 460) * 1000000u;
	if (*gpu_hz == 460000000u)
		*gpu_hz = 460800000u; // the GPU's table value
}

void apply(bool log)
{
	u32 cpu, gpu;
	wanted(&cpu, &gpu);
	if (get_rate(false) != cpu)
		set_rate(false, cpu);
	if (get_rate(true) != gpu)
		set_rate(true, gpu);
	if (log)
		fprintf(stderr, "[switch] clocks: CPU %u MHz, GPU %u MHz (%s)\n", get_rate(false) / 1000000,
		        get_rate(true) / 1000000,
		        appletGetOperationMode() == AppletOperationMode_Console ? "docked" : "handheld");
}

void clock_thread(void*)
{
	while (!g_clock_stop.load()) {
		svcSleepThread(2000000000ull); // the system resets them on docking changes
		apply(false);
	}
}
} // namespace

extern "C" void sms_switch_start_clocks(void)
{
	const char* e = getenv("SMS_SWITCH_CLOCKS");
	if (e && *e == '0') {
		fprintf(stderr, "[switch] clocks: the console's default profile (SMS_SWITCH_CLOCKS=0)\n");
		return;
	}
	g_use_clkrst = hosversionAtLeast(8, 0, 0);
	if (R_FAILED(g_use_clkrst ? clkrstInitialize() : pcvInitialize())) {
		fprintf(stderr, "[switch] clocks: no clock service; default profile\n");
		return;
	}
	g_clocks_open = true;
	apply(true);
	if (R_SUCCEEDED(threadCreate(&g_clock_thread, clock_thread, nullptr, nullptr, 0x4000, 0x3B, 2)))
		threadStart(&g_clock_thread);
}
