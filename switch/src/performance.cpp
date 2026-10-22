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
// Clocks: in handheld the docked rates, GPU 768 MHz and memory 1600 MHz,
// which 2x at 60 fps needs there (the handheld boost, below); the console's
// own otherwise (CPU 1020 MHz always). R3 + D-pad Up switches the boost off
// and on, SMS_SWITCH_HANDHELD_BOOST=0 starts with it off. Only Nintendo's own
// table rates (no overclock). Raised CPU clocks (1785 MHz, what the system's
// load boost uses) ran the fan hard for a few more frames a second.
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

// --- clocks for tests: SMS_SWITCH_TEST_*_MHZ, and R3 + D-pad Up in game ------
namespace {

enum { CPU, GPU, MEM, DOMAINS };
const PcvModuleId kModule[DOMAINS] = {PcvModuleId_CpuBus, PcvModuleId_GPU, PcvModuleId_EMC};
const char* const kEnv[DOMAINS] = {"SMS_SWITCH_TEST_CPU_MHZ", "SMS_SWITCH_TEST_GPU_MHZ", "SMS_SWITCH_TEST_MEM_MHZ"};
// Nintendo's rates in kHz, ascending (the highest of each: no overclock)
const u32 kCpuKhz[] = {612000, 816000, 1020000, 1224000, 1428000, 1581000, 1785000};
const u32 kGpuKhz[] = {307200, 384000, 460800, 537600, 614400, 691200, 768000, 844800, 921600};
const u32 kMemKhz[] = {665600, 800000, 1065600, 1331200, 1600000};
const u32* const kTable[DOMAINS] = {kCpuKhz, kGpuKhz, kMemKhz};
const int kCount[DOMAINS] = {7, 9, 5};

// The handheld boost, on unless SMS_SWITCH_HANDHELD_BOOST=0 (clocks.txt keeps
// what R3 + D-pad Up last chose): the docked rates in handheld too, GPU 768
// and memory 1600 (the CPU's own 1020 was enough). Delfino Plaza at 2x holds
// 60 fps in handheld with it, where the console's own handheld rates (GPU
// 307.2, memory 1331.2) give about 27 (2026-10-22 runs). Docked it changes
// nothing: those are the docked rates.
struct Profile {
	const char* name;
	u32 mhz[DOMAINS];
};
const Profile kProfiles[] = {
	{"handheld boost off (the console's own)", {0, 0, 0}},
	{"handheld boost on", {0, 768, 1600}},
};
const int kProfileCount = int(sizeof kProfiles / sizeof kProfiles[0]);

std::atomic<u32> g_wanted_hz[DOMAINS];  // 0: the console's own
int g_start_profile;
bool g_clocks_open;
Thread g_clock_thread;

u32 nearest_hz(int d, long mhz)
{
	u32 best = kTable[d][0];
	for (int i = 0; i < kCount[d]; i++)
		if (labs(long(kTable[d][i]) - mhz * 1000) < labs(long(best) - mhz * 1000))
			best = kTable[d][i];
	return best * 1000u;
}

// The console's own rates for the mode it is in (homebrew's profile): what
// a test's clocks go back to.
u32 own_hz(int d)
{
	const bool docked = appletGetOperationMode() == AppletOperationMode_Console;
	switch (d) {
	case CPU: return 1020000000u;
	case GPU: return docked ? 768000000u : 307200000u;
	default: return docked ? 1600000000u : 1331200000u;
	}
}

u32 clock_rate(int d)
{
	u32 hz = 0;
	ClkrstSession s;
	if (R_SUCCEEDED(clkrstOpenSession(&s, kModule[d], 3))) {
		clkrstGetClockRate(&s, &hz);
		clkrstCloseSession(&s);
	}
	return hz;
}

void set_clock_rate(int d, u32 hz)
{
	ClkrstSession s;
	if (R_SUCCEEDED(clkrstOpenSession(&s, kModule[d], 3))) {
		clkrstSetClockRate(&s, hz);
		clkrstCloseSession(&s);
	}
}

// What the clocks should be now: the test's while the game has the screen,
// the console's own otherwise (the HOME menu, sleep, closing it from there,
// which ends the process without its atexit).
void apply_clocks(bool focused)
{
	for (int d = 0; d < DOMAINS; d++) {
		const u32 wanted = g_wanted_hz[d].load();
		const u32 hz = focused && wanted ? wanted : own_hz(d);
		if (clock_rate(d) != hz)
			set_clock_rate(d, hz);
	}
}

bool any_wanted()
{
	for (int d = 0; d < DOMAINS; d++)
		if (g_wanted_hz[d].load())
			return true;
	return false;
}

void describe_clocks(char* buf, size_t n)
{
	snprintf(buf, n, "CPU %.0f, GPU %.1f, memory %.0f MHz", clock_rate(CPU) / 1e6, clock_rate(GPU) / 1e6,
	         clock_rate(MEM) / 1e6);
}

extern "C" void GXPC_ShowNotice(const char* text, double seconds); // sms_gx
extern "C" uint32_t GXPC_FrameCount(void);                         // sms_gx

void clock_thread(void*)
{
	PadState pad;
	padInitializeDefault(&pad);
	int profile = g_start_profile;
	bool was_focused = true, set = any_wanted();
	uint32_t last_frame = 0;
	u64 last_frame_tick = armGetSystemTick();
	for (int tick = 0;; tick++) {
		svcSleepThread(50000000ull);  // 20 times a second
		if (!serviceIsActive(hidGetServiceSession()))
			continue;  // (started before the program's input is)
		padUpdate(&pad);
		const u64 down = padGetButtonsDown(&pad), held = padGetButtons(&pad);
		// On the screen: in focus, and making frames. Out of it (the HOME
		// menu, sleep) the game's presenting stops, and with it the main
		// thread that would read the system's focus message.
		const uint32_t frame = GXPC_FrameCount();
		if (frame != last_frame) {
			last_frame = frame;
			last_frame_tick = armGetSystemTick();
		}
		const bool focused = appletGetFocusState() == AppletFocusState_InFocus &&
		                     armTicksToNs(armGetSystemTick() - last_frame_tick) < 3000000000ull;  // (loads stall for less)
		if (focused && (down & HidNpadButton_Up) && (held & HidNpadButton_StickR) && !(held & HidNpadButton_StickL)) {
			profile = (profile + 1) % kProfileCount;
			for (int d = 0; d < DOMAINS; d++)
				g_wanted_hz[d].store(kProfiles[profile].mhz[d] ? nearest_hz(d, kProfiles[profile].mhz[d]) : 0);
			apply_clocks(true);
			set = true;
			char line[160], rates[96];
			describe_clocks(rates, sizeof rates);
			snprintf(line, sizeof line, "Clocks: %s (%s)", kProfiles[profile].name, rates);
			GXPC_ShowNotice(line, 4.0);
			fprintf(stderr, "[switch] %s\n", line);
			if (FILE* f = fopen("clocks.txt", "w")) {  // (beside the NRO, read at the next start)
				fprintf(f, "SMS_SWITCH_HANDHELD_BOOST=%d\n", profile);
				fclose(f);
			}
			continue;
		}
		// again on leaving or regaining the screen, and every 2 s (the
		// system resets them on a docking change)
		if (set && (focused != was_focused || tick % 40 == 0)) {
			apply_clocks(focused);
			if (focused != was_focused) {
				char rates[96];
				describe_clocks(rates, sizeof rates);
				fprintf(stderr, "[switch] clocks: %s (%s)\n", focused ? "the game's again" : "the console's own off the screen", rates);
			}
		}
		was_focused = focused;
	}
}

void restore_clocks()
{
	if (any_wanted())
		apply_clocks(false);
}

} // namespace

// The handheld boost from the start (SMS_SWITCH_HANDHELD_BOOST), or a test's
// clocks (SMS_SWITCH_TEST_CPU_MHZ, _GPU_MHZ, _MEM_MHZ; not the old
// SMS_SWITCH_GPU_MHZ, which env.txt files may still hold); R3 + D-pad Up
// switches the boost in game. Nintendo's table rates only, the console's own
// after the game ends (a close from the HOME menu leaves memory raised until
// the next start, a docking change or another game).
// SMS_SWITCH_CLOCK_TOGGLE=0 leaves the clocks alone entirely.
extern "C" void sms_switch_start_clocks(void)
{
	const char* off = getenv("SMS_SWITCH_CLOCK_TOGGLE");
	if (off && *off == '0')
		return;
	if (R_FAILED(clkrstInitialize())) {
		fprintf(stderr, "[switch] clocks: no clock service\n");
		return;
	}
	g_clocks_open = true;
	// A run closed from the HOME menu while boosted (suspended there, it
	// cannot put them back) leaves the memory clock raised for the HOME
	// menu and homebrew: back to the console's own first.
	for (int d = 0; d < DOMAINS; d++) {
		const u32 hz = clock_rate(d), own = own_hz(d);
		if (hz && hz != own) {
			set_clock_rate(d, own);
			fprintf(stderr, "[switch] clocks: %s was left at %.1f MHz; back to %.1f\n", d == CPU ? "CPU" : d == GPU ? "GPU" : "memory",
			        hz / 1e6, clock_rate(d) / 1e6);
		}
	}
	const char* boost = getenv("SMS_SWITCH_HANDHELD_BOOST");
	g_start_profile = boost && *boost == '0' ? 0 : 1;
	for (int d = 0; d < DOMAINS; d++) {
		const char* e = getenv(kEnv[d]);
		const long mhz = e ? strtol(e, nullptr, 10) : 0;
		g_wanted_hz[d].store(mhz > 0 ? nearest_hz(d, mhz) : kProfiles[g_start_profile].mhz[d] ? nearest_hz(d, kProfiles[g_start_profile].mhz[d]) : 0);
	}
	if (any_wanted()) {
		apply_clocks(true);
		char rates[96];
		describe_clocks(rates, sizeof rates);
		fprintf(stderr, "[switch] clocks (%s, %s): %s\n",
		        appletGetOperationMode() == AppletOperationMode_Console ? "docked" : "handheld",
		        kProfiles[g_start_profile].name, rates);
	}
	atexit(restore_clocks);
	if (R_SUCCEEDED(threadCreate(&g_clock_thread, clock_thread, nullptr, nullptr, 0x4000, 0x2C, 2)))
		threadStart(&g_clock_thread);
}
