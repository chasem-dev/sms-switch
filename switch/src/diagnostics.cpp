// What the log needs when the game stops on a console far away.
//
// A crash: libnx hands the program's CPU exceptions (a bad address, a bad
// instruction) to __libnx_exception_handler, on a stack of its own. It writes
// the error, the registers and the return addresses up the frame-pointer
// chain to sms.log, each also as an offset into the program (the ELF's
// address: addr2line -e build/port/sms.elf), then lets the system end the
// program as before (Atmosphere's crash report as well).
//
// Memory: every 10 seconds (SMS_MEM_LOG=seconds, 0 for none) a "[mem]" line:
// the process's memory and the kernel's for its page tables
// (svcGetInfo), newlib's heap (mallinfo), the low windows' mmap ranges, the
// threads made so far, and dkgl's GPU memory by heap (in use / blocks made).
// What grows from line to line is what runs out.
//
// SMS_SWITCH_TEST_CRASH=seconds: a write to address 0 that long after the
// start (a test of the crash lines).
#include <switch.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "low_memory.h"
#ifdef SMS_SWITCH_DKGL
#include "dkgl/dkgl.h"
#endif

extern "C" unsigned sms_switch_threads_created(void);       // performance.cpp
extern "C" void sms_switch_low_mmap_used(u64* bytes, int* ranges); // low_memory.cpp

// libnx's: the exception handler's stack
extern "C" {
alignas(16) u8 __nx_exception_stack[0x8000];
u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);
}

namespace {

char g_line[512]; // the handler's (no allocation there)

void put(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void put(const char* fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	const int n = vsnprintf(g_line, sizeof g_line, fmt, ap);
	va_end(ap);
	if (n > 0)
		write(STDERR_FILENO, g_line, n < (int)sizeof g_line ? n : (int)sizeof g_line - 1);
}

// An address, and its offset into the program if it is in it.
const char* where(u64 a, char* buf, size_t n)
{
	const LowMemoryInfo* m = sms_switch_low_memory();
	if (a >= m->image_base && a < m->image_base + m->image_size)
		snprintf(buf, n, "0x%lx (sms.elf 0x%lx)", (unsigned long)a, (unsigned long)(a - m->image_base));
	else
		snprintf(buf, n, "0x%lx", (unsigned long)a);
	return buf;
}

// Readable memory holding [a, a + 16)?
bool readable(u64 a)
{
	if (!a || (a & 7))
		return false;
	MemoryInfo mi;
	u32 pi;
	if (R_FAILED(svcQueryMemory(&mi, &pi, a)))
		return false;
	return (mi.perm & Perm_R) && mi.type != MemType_Unmapped && a + 16 <= mi.addr + mi.size;
}

const char* desc_name(u32 d)
{
	switch (d) {
	case ThreadExceptionDesc_InstructionAbort: return "instruction abort";
	case ThreadExceptionDesc_MisalignedPC: return "misaligned PC";
	case ThreadExceptionDesc_MisalignedSP: return "misaligned SP";
	case ThreadExceptionDesc_SError: return "SError";
	case ThreadExceptionDesc_BadSVC: return "bad SVC";
	case ThreadExceptionDesc_Trap: return "trap";
	case 0x101: return "data abort or other";
	default: return "?";
	}
}

Thread g_mem_thread;
int g_mem_seconds = 10;
int g_crash_seconds = 0;
bool g_mem_lines = true;
volatile u64 g_crash_address = 0; // (opaque to the compiler, which would put a trap at a write through null)

// (in_crash: the heap's lock may be held, so no mallinfo, and no stdio)
void mem_line(bool in_crash = false)
{
	u64 used = 0, total = 0, sys_used = 0, sys_total = 0;
	svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
	svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
	svcGetInfo(&sys_used, InfoType_SystemResourceSizeUsed, CUR_PROCESS_HANDLE, 0);
	svcGetInfo(&sys_total, InfoType_SystemResourceSizeTotal, CUR_PROCESS_HANDLE, 0);
	struct mallinfo mi = {};
	if (!in_crash)
		mi = mallinfo();
	u64 low_bytes = 0;
	int low_ranges = 0;
	sms_switch_low_mmap_used(&low_bytes, &low_ranges);
	char gpu[160] = "";
#ifdef SMS_SWITCH_DKGL
	uint64_t gu[5], gr[5];
	dkgl::memoryStats(gu, gr);
	if (gr[0] | gr[2])
		snprintf(gpu, sizeof gpu,
		         "; GPU MiB data %.1f/%.0f, readback %.1f/%.0f, images %.1f/%.0f, code %.1f/%.0f, commands %.1f/%.0f",
		         gu[0] / 1048576.0, gr[0] / 1048576.0, gu[1] / 1048576.0, gr[1] / 1048576.0, gu[2] / 1048576.0,
		         gr[2] / 1048576.0, gu[3] / 1048576.0, gr[3] / 1048576.0, gu[4] / 1048576.0, gr[4] / 1048576.0);
#endif
	char line[512];
	const int n = snprintf(line, sizeof line,
	        "[mem] %lu s: process %lu/%lu MiB, kernel %lu/%lu KiB; heap %lu MiB in use, %lu MiB free of %lu MiB; "
	        "low mmap %lu KiB in %d; threads made %u%s\n",
	        (unsigned long)(armTicksToNs(armGetSystemTick()) / 1000000000ull), (unsigned long)(used >> 20),
	        (unsigned long)(total >> 20), (unsigned long)(sys_used >> 10), (unsigned long)(sys_total >> 10),
	        (unsigned long)(mi.uordblks >> 20), (unsigned long)(mi.fordblks >> 20), (unsigned long)(mi.arena >> 20),
	        (unsigned long)(low_bytes >> 10), low_ranges, sms_switch_threads_created(), gpu);
	if (n > 0)
		write(STDERR_FILENO, line, n < (int)sizeof line ? n : (int)sizeof line - 1);
}

void mem_loop(void*)
{
	for (int s = 0;; s += g_mem_seconds) {
		if (g_crash_seconds > 0 && s >= g_crash_seconds) {
			fprintf(stderr, "[crash] SMS_SWITCH_TEST_CRASH: writing to address 0\n");
			*(volatile int*)g_crash_address = 1;
		}
		svcSleepThread(u64(g_mem_seconds) * 1000000000ull);
		if (g_mem_lines)
			mem_line();
	}
}

} // namespace

extern "C" void __libnx_exception_handler(ThreadExceptionDump* ctx)
{
	char a[64], b[64];
	u64 tid = 0;
	svcGetThreadId(&tid, CUR_THREAD_HANDLE);
	put("\n[crash] %s (0x%x) on thread %lu: pc %s, lr %s\n", desc_name(ctx->error_desc), ctx->error_desc,
	    (unsigned long)tid, where(ctx->pc.x, a, sizeof a), where(ctx->lr.x, b, sizeof b));
	put("[crash] address 0x%lx, sp 0x%lx, fp 0x%lx, esr 0x%x, pstate 0x%x\n", (unsigned long)ctx->far.x,
	    (unsigned long)ctx->sp.x, (unsigned long)ctx->fp.x, ctx->esr, ctx->pstate);
	for (int i = 0; i < 29; i += 4) {
		put("[crash]");
		for (int j = i; j < i + 4 && j < 29; j++)
			put(" x%d %lx", j, (unsigned long)ctx->cpu_gprs[j].x);
		put("\n");
	}
	// up the frame records (x29: the caller's x29, then its return address)
	u64 fp = ctx->fp.x;
	for (int depth = 0; depth < 32 && readable(fp); depth++) {
		const u64* rec = (const u64*)fp;
		if (!rec[1])
			break;
		put("[crash] called from %s\n", where(rec[1], a, sizeof a));
		if (rec[0] <= fp)
			break;
		fp = rec[0];
	}
	mem_line(true);
	put("[crash] (end)\n");
}

// Started from switch_main.cpp once the log is open.
extern "C" void sms_switch_diagnostics_start(void)
{
	if (const char* e = getenv("SMS_MEM_LOG"))
		g_mem_seconds = atoi(e);
	if (const char* e = getenv("SMS_SWITCH_TEST_CRASH"))
		g_crash_seconds = atoi(e);
	if (g_mem_seconds <= 0 && g_crash_seconds <= 0)
		return;
	if (g_mem_seconds <= 0) {
		// (the thread, for the test, with no lines)
		g_mem_seconds = g_crash_seconds;
		g_mem_lines = false;
	}
	if (g_mem_lines)
		mem_line();
	if (R_SUCCEEDED(threadCreate(&g_mem_thread, mem_loop, nullptr, nullptr, 0x4000, 0x2C, -2)))
		threadStart(&g_mem_thread);
}
