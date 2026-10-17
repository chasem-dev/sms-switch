// Everything the game can point at must lie below 4 GiB: it keeps addresses
// in 4-byte fields (sms-port docs/64-BIT.md). On a PC the executable is linked
// low and mmap takes an address; a Switch NRO is position independent, and
// hbloader maps it, its heap and every thread stack wherever the 39-bit
// address space has room, normally far above 4 GiB. So:
//
// 1. The program itself. crt0 calls __nx_dynamic before anything else runs,
//    to relocate the image. This file's __nx_dynamic first checks where the
//    image is; if it is high, it copies the still unrelocated image into the
//    heap, maps that copy at a low address as code (svcMapProcessCodeMemory,
//    as hbloader maps an NRO), and enters the copy's crt0 as if hbloader had
//    started it there, with the rest of the heap passed on as its heap. The
//    copy's __nx_dynamic then relocates it normally. Nothing has run yet at
//    that point (no relocation, no libc, no globals of other files), so
//    phase 1 only uses this file's statics, the stack and SVCs.
//
// 2. Memory the port maps for the game: MEM1 at its GameCube address
//    0x80000000, and the stacks game code runs on (below 2 GiB). Before the
//    heap is handed to newlib, __libnx_initheap maps two windows of it there
//    (elsewhere below 4 GiB if the loader is in the way): MEM1's, which the
//    port takes with sms_switch_mem1_window, and the stacks', from which
//    mmap()/munmap() (switch/include/sys/mman.h) serve the port's
//    MAP_FIXED_NOREPLACE and anonymous requests.
//
// Thread stacks libnx creates are mapped again at an address of its own, so
// threads that run game code switch onto their low stacks themselves
// (stack_switch.cpp, PORT_LOW_STACK_CALL in the port).
#include <switch.h>
#include <errno.h>
#include <string.h>
#include <sys/mman.h>
#include "low_memory.h"

namespace {

// Where things go. The image's preferred address keeps it the same from run
// to run (save states hold game pointers into it).
constexpr u64 kImagePreferred = 0x10000000;
constexpr u64 kImageLimit = 0x40000000;   // the image must end below this
constexpr u64 kStackZone = 0x40000000;    // port_low_alloc searches from here
constexpr u64 kStackZoneSize = 64 << 20;
constexpr u64 kMem1Zone = 0x80000000;     // the game's MEM1 (cached) address
constexpr u64 kMem1ZoneSize = 64 << 20;   // 64-bit MEM1 default (SMS_MEM_MB)

// A line with two hex numbers in an emulator's or debugger's log (this runs
// before the C library is ready).
void trace_hex(const char* text, u64 a, u64 b)
{
	char buf[96];
	size_t n = 0;
	while (*text && n < 60)
		buf[n++] = *text++;
	const u64 values[2] = {a, b};
	for (u64 v : values) {
		buf[n++] = '0';
		buf[n++] = 'x';
		for (int shift = 36; shift >= 0; shift -= 4)
			buf[n++] = "0123456789abcdef"[(v >> shift) & 15];
		buf[n++] = ' ';
	}
	svcOutputDebugString(buf, n);
}

struct Mod0Header {
	u32 magic_mod0;
	s32 dyn_offset;
	s32 bss_start_offset;
	s32 bss_end_offset;
	s32 eh_frame_hdr_start_offset;
	s32 eh_frame_hdr_end_offset;
	s32 unused;
	u32 magic_lny0;
	s32 got_start_offset;
	s32 got_end_offset;
	u32 magic_lny1;
	s32 relro_start_offset;
	s32 relro_end_offset;
};

struct ElfDyn {
	s64 tag;
	u64 val;
};
struct ElfRela {
	u64 offset;
	u64 info;
	s64 addend;
};
enum : s64 { DT_NULL_ = 0, DT_RELA_ = 7, DT_RELASZ_ = 8, DT_RELRSZ_ = 35, DT_RELR_ = 36 };
enum : u32 { R_AARCH64_NONE_ = 0, R_AARCH64_RELATIVE_ = 1027 };

// The homebrew ABI context the relocated copy starts with: the loader's
// entries, with the heap replaced by what is left of it. Initialised, so it is
// in .data (copied with the image) rather than .bss (cleared by the copy).
constexpr int kMaxEntries = 64;
ConfigEntry g_low_context[kMaxEntries] = {{EntryType_EndOfList, 0, {1, 0}}};

LowMemoryInfo g_info = {1}; // in .data, like g_low_context

// Phase 1 helpers: no libc (memcpy may not be reached yet through a
// relocated pointer, and the compiler must not call it either).
__attribute__((optimize("no-tree-loop-distribute-patterns")))
void copy_words(u64* dst, const u64* src, u64 bytes)
{
	for (u64 i = 0; i < bytes / 8; i++)
		dst[i] = src[i];
}

__attribute__((optimize("no-tree-loop-distribute-patterns")))
void zero_words(u64* dst, u64 bytes)
{
	for (u64 i = 0; i < bytes / 8; i++)
		dst[i] = 0;
}

u64 align_up(u64 v, u64 a) { return (v + a - 1) & ~(a - 1); }

bool range_unmapped(u64 addr, u64 size)
{
	MemoryInfo mi;
	u32 pi;
	if (R_FAILED(svcQueryMemory(&mi, &pi, addr)))
		return false;
	return mi.type == MemType_Unmapped && mi.addr + mi.size >= addr + size;
}

// The first free, `align`-aligned range of `size` bytes in [from, limit).
u64 find_free(u64 from, u64 limit, u64 size, u64 align)
{
	for (u64 at = align_up(from, align); at + size <= limit;) {
		MemoryInfo mi;
		u32 pi;
		if (R_FAILED(svcQueryMemory(&mi, &pi, at)))
			return 0;
		if (mi.type == MemType_Unmapped && mi.addr + mi.size >= at + size)
			return at;
		u64 next = mi.addr + mi.size;
		if (next <= at)
			return 0;
		at = align_up(next, align);
	}
	return 0;
}

Handle process_handle(const ConfigEntry* ctx)
{
	for (const ConfigEntry* e = ctx; ctx && e->Key != EntryType_EndOfList; e++)
		if (e->Key == EntryType_ProcessHandle)
			return (Handle)e->Value[0];
	return CUR_PROCESS_HANDLE;
}

// libnx's own relocation (runtime/dynamic.c), without locking the relro
// segment read-only: that segment of the relocated copy is code-alias
// memory, and its permissions stay as phase 1 set them.
void relocate(u64 base, const Mod0Header* mod0)
{
	u8* bss_start = (u8*)mod0 + mod0->bss_start_offset;
	u8* bss_end = (u8*)mod0 + mod0->bss_end_offset;
	if (bss_end > bss_start)
		memset(bss_start, 0, bss_end - bss_start);
	const ElfDyn* dyn = (const ElfDyn*)((const u8*)mod0 + mod0->dyn_offset);
	const ElfRela* rela = nullptr;
	const u64* relr = nullptr;
	u64 relasz = 0, relrsz = 0;
	for (; dyn->tag != DT_NULL_; dyn++) {
		switch (dyn->tag) {
		case DT_RELA_: rela = (const ElfRela*)(base + dyn->val); break;
		case DT_RELASZ_: relasz = dyn->val / sizeof(ElfRela); break;
		case DT_RELR_: relr = (const u64*)(base + dyn->val); break;
		case DT_RELRSZ_: relrsz = dyn->val / sizeof(u64); break;
		}
	}
	for (u64 i = 0; rela && i < relasz; i++) {
		u32 type = (u32)rela[i].info;
		if (type == R_AARCH64_RELATIVE_)
			*(u64*)(base + rela[i].offset) = base + rela[i].addend;
		else if (type != R_AARCH64_NONE_)
			svcBreak(BreakReason_Panic, 0, 0);
	}
	u64* where = nullptr;
	for (u64 i = 0; relr && i < relrsz; i++) {
		u64 entry = relr[i];
		if ((entry & 1) == 0) {
			where = (u64*)(base + entry);
			*where++ += base;
		} else {
			for (u64 bitmap = entry >> 1, id = 0; bitmap; bitmap >>= 1, id++)
				if (bitmap & 1)
					where[id] += base;
			where += 63;
		}
	}
}

} // namespace

extern "C" {

void sms_switch_enter(const ConfigEntry* ctx, u64 handle, u64 lr, u64 sp, u64 entry) __attribute__((noreturn));
__asm__(
	".section .text.sms_switch_enter, \"ax\", %progbits\n"
	".global sms_switch_enter\n"
	".type sms_switch_enter, %function\n"
	"sms_switch_enter:\n"
	"  mov sp, x3\n"
	"  mov x30, x2\n"
	"  br x4\n");

// crt0 keeps the loader's arguments in x25..x28 (context, main thread
// handle, return address, stack pointer); hand them to the C part.
void __nx_dynamic(u64 base, const void* mod0);
__asm__(
	".section .text.__nx_dynamic, \"ax\", %progbits\n"
	".global __nx_dynamic\n"
	".type __nx_dynamic, %function\n"
	"__nx_dynamic:\n"
	"  mov x2, x25\n"
	"  mov x3, x26\n"
	"  mov x4, x27\n"
	"  mov x5, x28\n"
	"  b sms_switch_dynamic\n");

void sms_switch_dynamic(u64 base, const Mod0Header* mod0, const ConfigEntry* ctx, u64 handle, u64 lr, u64 sp)
{
	if (mod0->magic_mod0 != 0x30444f4d) // MOD0
		return;
	const NroHeader* nro = (const NroHeader*)(base + sizeof(NroStart));
	// Where this image runs, unless it moves below (the copy records it
	// again when it starts).
	g_info.image_base = base;
	if (!g_info.image_moved)
		g_info.loaded_at = base;
	if (nro->magic != 0x304f524e) {
		relocate(base, mod0); // not an NRO image
		return;
	}
	// Started without a homebrew loader (an emulator running the NRO
	// directly passes the NSO arguments: no context, x1 = main thread):
	// the copy gets a context of its own, as hbloader would give it.
	ConfigEntry nso_context[] = {
		{EntryType_MainThreadHandle, 0, {handle, 0}},
		{EntryType_SyscallAvailableHint, 0, {~(u64)0, ~(u64)0}},
		{EntryType_SyscallAvailableHint2, 0, {~(u64)0, 0}},
		{EntryType_EndOfList, 0, {0, 0}},
	};
	if (!ctx) {
		ctx = nso_context;
		handle = ~(u64)0; // the homebrew ABI's x1
	}

	// The image as mapped: text, rodata, data, then .bss.
	const u64 text_size = nro->segments[0].size;
	const u64 ro_off = nro->segments[1].file_off, ro_size = nro->segments[1].size;
	const u64 data_off = nro->segments[2].file_off, data_size = nro->segments[2].size;
	const u64 file_size = data_off + data_size;
	const u64 image_size = align_up(file_size + nro->bss_size, 0x1000);
	g_info.image_size = image_size;
	if (base + image_size <= kImageLimit) {
		relocate(base, mod0);
		return;
	}

	// The heap: the loader's, or our own as libnx would size it.
	u64 heap = 0, heap_size = 0;
	for (const ConfigEntry* e = ctx; e->Key != EntryType_EndOfList; e++)
		if (e->Key == EntryType_OverrideHeap)
			heap = e->Value[0], heap_size = e->Value[1];
	if (!heap) {
		u64 total = 0, used = 0;
		svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
		svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
		if (total > used + 0x200000)
			heap_size = (total - used - 0x200000) & ~(u64)0x1FFFFF;
		void* p = nullptr;
		if (!heap_size || R_FAILED(svcSetHeapSize(&p, heap_size))) {
			relocate(base, mod0);
			return;
		}
		heap = (u64)p;
	}
	const u64 low = range_unmapped(kImagePreferred, image_size)
		? kImagePreferred : find_free(0x08000000, kImageLimit, image_size, 0x200000);
	if (!low || heap_size < image_size + (16 << 20)) {
		relocate(base, mod0); // the game reports the high addresses itself
		return;
	}

	// The copy, unrelocated as this image still is, with the context.
	copy_words((u64*)heap, (const u64*)base, align_up(file_size, 8));
	zero_words((u64*)(heap + align_up(file_size, 8)), image_size - align_up(file_size, 8));
	ConfigEntry* out = (ConfigEntry*)(heap + ((u64)g_low_context - base));
	int n = 0;
	bool have_heap = false;
	for (const ConfigEntry* e = ctx;; e++) {
		if (n == kMaxEntries - 2)
			break;
		ConfigEntry c = *e;
		if (c.Key == EntryType_OverrideHeap) {
			c.Value[0] = heap + image_size;
			c.Value[1] = heap_size - image_size;
			have_heap = true;
		}
		if (c.Key == EntryType_EndOfList && !have_heap) {
			ConfigEntry h = {EntryType_OverrideHeap, 0, {heap + image_size, heap_size - image_size}};
			out[n++] = h;
		}
		out[n++] = c;
		if (c.Key == EntryType_EndOfList)
			break;
	}
	LowMemoryInfo* info = (LowMemoryInfo*)(heap + ((u64)&g_info - base));
	info->image_moved = 1;
	info->image_base = low;
	info->image_size = image_size;
	armDCacheFlush((void*)heap, image_size);

	const Handle proc = process_handle(ctx);
	if (R_FAILED(svcMapProcessCodeMemory(proc, low, heap, image_size))) {
		relocate(base, mod0);
		return;
	}
	Result rc = svcSetProcessMemoryPermission(proc, low, text_size, Perm_Rx);
	if (R_SUCCEEDED(rc))
		rc = svcSetProcessMemoryPermission(proc, low + ro_off, ro_size, Perm_R);
	if (R_SUCCEEDED(rc))
		rc = svcSetProcessMemoryPermission(proc, low + data_off, image_size - data_off, Perm_Rw);
	if (R_FAILED(rc)) {
		svcUnmapProcessCodeMemory(proc, low, heap, image_size);
		relocate(base, mod0);
		return;
	}
	armICacheInvalidate((void*)low, text_size);
	sms_switch_enter((const ConfigEntry*)(low + ((u64)g_low_context - base)), handle, lr, sp, low);
}

// Phase 2: the low windows, carved from the start of the heap; newlib gets
// the rest.
void __libnx_initheap(void)
{
	u64 heap = 0, heap_size = 0;
	if (envHasHeapOverride()) {
		heap = (u64)envGetHeapOverrideAddr();
		heap_size = envGetHeapOverrideSize();
	} else {
		u64 total = 0, used = 0;
		svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
		svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
		if (total > used + 0x200000)
			heap_size = (total - used - 0x200000) & ~(u64)0x1FFFFF;
		void* p = nullptr;
		if (R_FAILED(svcSetHeapSize(&p, heap_size)))
			diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_HeapAllocFailed));
		heap = (u64)p;
	}
	// hbloader passes the process's own handle; an emulator starting the
	// NRO directly does not, and takes the pseudo-handle.
	Handle proc = envGetOwnProcessHandle();
	if (proc == INVALID_HANDLE)
		proc = CUR_PROCESS_HANDLE;
	// The stacks' window where port_low_alloc looks first, else anywhere
	// below 2 GiB; MEM1's at its GameCube address, else anywhere below 4 GiB.
	struct Want {
		u64 at, size, lo, hi;
		u32 mem1;
	} wants[] = {
		{kStackZone, kStackZoneSize, 0x08000000, 0x80000000, 0},
		{kMem1Zone, kMem1ZoneSize, 0x08000000, 0xFFE00000, 1},
	};
	g_info.zone_count = 0;
	for (const Want& w : wants) {
		if (heap_size < w.size + (64 << 20))
			continue;
		u64 at = range_unmapped(w.at, w.size) ? w.at : find_free(w.lo, w.hi, w.size, 0x200000);
		bool mapped = false;
		if (at && R_SUCCEEDED(svcMapProcessCodeMemory(proc, at, heap, w.size))) {
			mapped = R_SUCCEEDED(svcSetProcessMemoryPermission(proc, at, w.size, Perm_Rw));
			if (!mapped)
				svcUnmapProcessCodeMemory(proc, at, heap, w.size);
		}
		if (!mapped) {
			// An emulator that gives homebrew no process handle (Ryubing):
			// svcMapMemory needs none, but maps only into the stack region,
			// so the window goes there if that region reaches below hi.
			u64 region = 0, region_size = 0;
			svcGetInfo(&region, InfoType_StackRegionAddress, CUR_PROCESS_HANDLE, 0);
			svcGetInfo(&region_size, InfoType_StackRegionSize, CUR_PROCESS_HANDLE, 0);
			trace_hex("[switch] stack region ", region, region_size);
			const u64 lo = region > w.lo ? region : w.lo;
			const u64 hi = region + region_size < w.hi ? region + region_size : w.hi;
			at = w.at >= lo && w.at + w.size <= hi && range_unmapped(w.at, w.size) ? w.at
			     : hi > lo + w.size                                                  ? find_free(lo, hi, w.size, 0x200000)
			                                                                         : 0;
			mapped = at && R_SUCCEEDED(svcMapMemory((void*)at, (void*)heap, w.size));
			if (w.mem1 && !(mapped && at == w.at)) {
				// MEM1 at its own address if the alias region holds it, with
				// memory of its own (svcMapPhysicalMemory): where the console
				// has it, so an emulator runs the same code paths
				u64 alias = 0, alias_size = 0;
				svcGetInfo(&alias, InfoType_AliasRegionAddress, CUR_PROCESS_HANDLE, 0);
				svcGetInfo(&alias_size, InfoType_AliasRegionSize, CUR_PROCESS_HANDLE, 0);
				trace_hex("[switch] alias region ", alias, alias_size);
				if (w.at >= alias && w.at + w.size <= alias + alias_size && range_unmapped(w.at, w.size) &&
				    R_SUCCEEDED(svcMapPhysicalMemory((void*)w.at, w.size))) {
					if (mapped)
						svcUnmapMemory((void*)at, (void*)heap, w.size);
					at = w.at;
					mapped = true;
					trace_hex("[switch] MEM1 by svcMapPhysicalMemory ", at, w.size);
					// (the heap piece stays the process's: it is not passed on below)
				}
			}
		}
		if (!mapped)
			continue;
		LowZone& z = g_info.zones[g_info.zone_count++];
		z.base = at;
		z.size = w.size;
		z.source = heap;
		z.mem1 = w.mem1;
		z.taken = 0;
		heap += w.size;
		heap_size -= w.size;
	}
	g_info.heap_base = heap;
	g_info.heap_size = heap_size;

	extern char* fake_heap_start;
	extern char* fake_heap_end;
	fake_heap_start = (char*)heap;
	fake_heap_end = (char*)(heap + heap_size);
}

const LowMemoryInfo* sms_switch_low_memory(void) { return &g_info; }

void* sms_switch_mem1_window(u32 size)
{
	for (int i = 0; i < g_info.zone_count; i++) {
		LowZone& z = g_info.zones[i];
		if (z.mem1 && !z.taken && size <= z.size) {
			z.taken = 1;
			return (void*)z.base;
		}
	}
	return NULL;
}

} // extern "C"

// mmap over the zones: a first-fit list of used ranges per zone.
namespace {
constexpr int kMaxRanges = 256;
struct Used {
	u64 at, size;
};
Used g_used[LowMemoryInfo::kMaxZones][kMaxRanges];
int g_used_count[LowMemoryInfo::kMaxZones];
Mutex g_mmap_lock;

bool overlaps_used(int z, u64 at, u64 size)
{
	for (int i = 0; i < g_used_count[z]; i++)
		if (at < g_used[z][i].at + g_used[z][i].size && g_used[z][i].at < at + size)
			return true;
	return false;
}

bool take(int z, u64 at, u64 size)
{
	if (g_used_count[z] == kMaxRanges)
		return false;
	g_used[z][g_used_count[z]++] = {at, size};
	memset((void*)at, 0, size);
	return true;
}
} // namespace

extern "C" void* mmap(void* addr, size_t length, int prot, int flags, int fd, off_t offset)
{
	(void)prot;
	(void)offset;
	if (!(flags & MAP_ANONYMOUS) || fd != -1 || length == 0) {
		errno = ENODEV;
		return MAP_FAILED;
	}
	const u64 size = align_up(length, 0x1000);
	const u64 want = (u64)addr;
	void* result = MAP_FAILED;
	mutexLock(&g_mmap_lock);
	for (int z = 0; z < g_info.zone_count && result == MAP_FAILED; z++) {
		const LowZone& zone = g_info.zones[z];
		if (zone.mem1)
			continue;
		if (want && (want & 0xFFF) == 0 && want >= zone.base && want + size <= zone.base + zone.size
		    && !overlaps_used(z, want, size) && take(z, want, size))
			result = (void*)want;
	}
	if (result == MAP_FAILED && !(flags & (MAP_FIXED | MAP_FIXED_NOREPLACE))) {
		// Anywhere below 2 GiB (MEM1's zone stays whole for MEM1).
		for (int z = 0; z < g_info.zone_count && result == MAP_FAILED; z++) {
			const LowZone& zone = g_info.zones[z];
			if (zone.mem1)
				continue;
			for (u64 at = zone.base; at + size <= zone.base + zone.size; at += 0x10000) {
				if (!overlaps_used(z, at, size)) {
					if (take(z, at, size))
						result = (void*)at;
					break;
				}
			}
		}
	}
	mutexUnlock(&g_mmap_lock);
	if (result == MAP_FAILED)
		errno = ENOMEM;
	return result;
}

extern "C" int munmap(void* addr, size_t length)
{
	const u64 at = (u64)addr, size = align_up(length, 0x1000);
	mutexLock(&g_mmap_lock);
	for (int z = 0; z < g_info.zone_count; z++) {
		for (int i = 0; i < g_used_count[z];) {
			Used& u = g_used[z][i];
			if (u.at >= at && u.at + u.size <= at + size)
				u = g_used[z][--g_used_count[z]]; // wholly inside: drop
			else {
				if (u.at < at && u.at + u.size > at)
					u.size = at - u.at; // trim the tail
				else if (u.at < at + size && u.at + u.size > at + size) {
					u.size -= at + size - u.at; // trim the head
					u.at = at + size;
				}
				i++;
			}
		}
	}
	mutexUnlock(&g_mmap_lock);
	return 0;
}

extern "C" int mprotect(void*, size_t, int) { return 0; }
extern "C" int madvise(void*, size_t, int) { return 0; }
