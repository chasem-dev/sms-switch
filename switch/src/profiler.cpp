// A sampling profiler for the console (SMS_SWITCH_PROFILE=start,seconds):
// from `start` seconds after launch, for `seconds`, a thread on core 2 pauses
// each of the program's threads about 1000 times a second, reads where it was
// (svcGetThreadContext3: the address, and the return addresses of a few frame
// pointer frames above it) and lets it go on. At the end each thread's counts
// per call stack go to the log as "profile" lines; tools/switch-profile.py
// names their functions from the ELF.
#include <switch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <unordered_map>
#include <vector>

void sms_switch_profile_add(Handle h, const char* label);

namespace {
constexpr int kMaxThreads = 32;
constexpr int kDepth = 5;  // the address and four return addresses
struct Sampled {
	Handle handle;
	char label[24];
};
Sampled g_threads[kMaxThreads];
int g_count = 0;  // grows as threads start (never shrinks: a finished thread's pause just fails)
Mutex g_lock;
Thread g_sampler;
double g_start = 0, g_seconds = 0;
bool g_on = false;

struct Stack {
	u64 a[kDepth];
	bool operator==(const Stack& o) const { return !memcmp(a, o.a, sizeof a); }
};
struct StackHash {
	size_t operator()(const Stack& s) const
	{
		u64 h = 1469598103934665603ull;
		for (u64 v : s.a)
			h = (h ^ v) * 1099511628211ull;
		return size_t(h);
	}
};

// Return addresses up the frame-pointer chain, as far as it stays on this
// thread's stack (above sp, within 1 MiB) and aligned.
void walk(const ThreadContext& ctx, Stack* s)
{
	memset(s, 0, sizeof *s);
	s->a[0] = ctx.pc.x;
	const u64 sp = ctx.sp, top = sp + 0x100000;
	u64 fp = ctx.fp;
	int n = 1;
	// the leaf may not have a frame of its own yet: lr is its caller
	s->a[n++] = ctx.lr;
	while (n < kDepth && fp >= sp && fp + 16 <= top && !(fp & 7)) {
		const u64* f = reinterpret_cast<const u64*>(fp);
		const u64 next = f[0], ra = f[1];
		if (ra && ra != s->a[n - 1])
			s->a[n++] = ra;
		if (next <= fp)
			break;
		fp = next;
	}
}

void sample_loop(void*)
{
	svcSleepThread(u64(g_start * 1e9));
	std::vector<std::unordered_map<Stack, u32, StackHash>> counts(kMaxThreads);
	std::vector<u32> total(kMaxThreads);
	const Handle self = threadGetCurHandle();
	const u64 end = armGetSystemTick() + armNsToTicks(u64(g_seconds * 1e9));
	while (armGetSystemTick() < end) {
		mutexLock(&g_lock);
		const int n = g_count;
		mutexUnlock(&g_lock);
		for (int i = 0; i < n; i++) {
			const Handle h = g_threads[i].handle;
			if (h == INVALID_HANDLE || h == self)
				continue;
			if (R_FAILED(svcSetThreadActivity(h, ThreadActivity_Paused)))
				continue;
			ThreadContext ctx;
			const bool ok = R_SUCCEEDED(svcGetThreadContext3(&ctx, h));
			Stack s;
			if (ok)
				walk(ctx, &s);
			svcSetThreadActivity(h, ThreadActivity_Runnable);
			if (!ok)
				continue;
			counts[i][s]++;
			total[i]++;
		}
		svcSleepThread(1000000);  // 1 ms
	}
	for (int i = 0; i < g_count; i++) {
		if (!total[i])
			continue;
		std::vector<std::pair<Stack, u32>> v(counts[i].begin(), counts[i].end());
		std::sort(v.begin(), v.end(),
		          [](const std::pair<Stack, u32>& a, const std::pair<Stack, u32>& b) { return a.second > b.second; });
		fprintf(stderr, "[switch] profile thread %d %s: %u samples, %zu stacks\n", i, g_threads[i].label, total[i],
		        v.size());
		const size_t keep = std::min<size_t>(v.size(), 30000);
		for (size_t k = 0; k < keep; k++) {
			const Stack& s = v[k].first;
			fprintf(stderr, "[switch] profile %d %u %llx %llx %llx %llx %llx\n", i, v[k].second,
			        (unsigned long long)s.a[0], (unsigned long long)s.a[1], (unsigned long long)s.a[2],
			        (unsigned long long)s.a[3], (unsigned long long)s.a[4]);
		}
	}
	fprintf(stderr, "[switch] profile: done\n");
}
} // namespace

void sms_switch_profile_add(Handle h, const char* label)
{
	if (!g_on)
		return;
	mutexLock(&g_lock);
	for (int i = 0; i < g_count; i++)
		if (g_threads[i].handle == h) {  // named again (main, gl): the better name
			snprintf(g_threads[i].label, sizeof g_threads[i].label, "%s", label);
			mutexUnlock(&g_lock);
			return;
		}
	if (g_count < kMaxThreads) {
		g_threads[g_count].handle = h;
		snprintf(g_threads[g_count].label, sizeof g_threads[g_count].label, "%s", label);
		g_count++;
	}
	mutexUnlock(&g_lock);
}

// From the thread itself (performance.cpp): 0 the main thread, 1 the GL thread.
extern "C" void sms_switch_profile_thread(int slot)
{
	sms_switch_profile_add(threadGetCurHandle(), slot == 0 ? "main" : "gl");
}

// Any other thread, from itself, by a name.
void sms_switch_profile_add_current(const char* label)
{
	sms_switch_profile_add(threadGetCurHandle(), label);
}

// A thread made with threadCreate (performance.cpp's wrap), by its entry point.
extern "C" void sms_switch_profile_created(Handle h, void* entry)
{
	char label[24];
	snprintf(label, sizeof label, "entry %llx", (unsigned long long)(uintptr_t)entry);
	sms_switch_profile_add(h, label);
}

extern "C" void sms_switch_start_profiler(void)
{
	const char* e = getenv("SMS_SWITCH_PROFILE");
	if (!e || sscanf(e, "%lf,%lf", &g_start, &g_seconds) != 2 || g_seconds <= 0)
		return;
	g_on = true;
	sms_switch_profile_add(threadGetCurHandle(), "main");
	if (R_SUCCEEDED(threadCreate(&g_sampler, sample_loop, nullptr, nullptr, 0x10000, 0x20, 2)))
		threadStart(&g_sampler);
	fprintf(stderr, "[switch] profile: sampling every thread from %.0f s for %.0f s\n", g_start, g_seconds);
}
