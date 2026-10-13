// The low-memory layout switch/src/low_memory.cpp set up (see there).
#pragma once
#include <switch/types.h>

struct LowZone {
	u64 base, size; // the window below 4 GiB
	u64 source;     // the heap pages it aliases
	u32 mem1;       // reserved for MEM1 (sms_switch_mem1_window), not mmap
	u32 taken;
};

struct LowMemoryInfo {
	u32 in_data;     // nonzero, so the object is in .data (see low_memory.cpp)
	u32 image_moved; // the program was moved from where the loader put it
	u64 loaded_at;   // where the loader put it
	u64 image_base;  // where it runs
	u64 image_size;
	enum { kMaxZones = 4 };
	LowZone zones[kMaxZones];
	int zone_count;
	u64 heap_base, heap_size; // newlib's heap (the rest)
};

#ifdef __cplusplus
extern "C" {
#endif
const LowMemoryInfo* sms_switch_low_memory(void);
// MEM1's window (at 0x80000000 if that was free), or NULL if `size` does not fit.
void* sms_switch_mem1_window(u32 size);
#ifdef __cplusplus
}
#endif
