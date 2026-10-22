// dkgl's device: memory, submissions and fences, descriptors, the swapchain.
#include "dkgl_internal.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <iterator>
#include <stdio.h>
#include <switch.h>

namespace dkgl {
extern bool g_tracing;  // dkgl_draw.cpp

Device g_dev;

void logf(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    fprintf(stderr, "[dkgl] %s\n", buf);
    // and in an emulator's or debugger's log, which outlives a crash
    svcOutputDebugString(buf, size_t(n < 0 ? 0 : n < int(sizeof buf) ? n : int(sizeof buf) - 1));
}

Activity g_activity;

Waiting::Waiting(const char* what) : prev(g_activity.waiting) {
    g_activity.since = armGetSystemTick();
    g_activity.waiting = what;
}
Waiting::~Waiting() {
    g_activity.since = armGetSystemTick();
    g_activity.waiting = prev;
}

namespace {
Thread s_watchdog;

void watchdog(void*) {
    const char* told = nullptr;
    uint64_t toldSince = 0;
    uint32_t draws = g_activity.draws, swaps = g_activity.swaps, still = 0;
    for (;;) {
        svcSleepThread(1000000000ll);
        const char* what = g_activity.waiting;
        const uint64_t since = g_activity.since;
        if (what && (what != told || since != toldSince) &&
            armTicksToNs(armGetSystemTick() - since) > 3000000000ull) {
            logf("the GL thread has been %s for 3 s (draws %u, swaps %u; GPU submissions %llu, finished %llu)", what,
                 g_activity.draws, g_activity.swaps, (unsigned long long)g_dev.serial - 1,
                 (unsigned long long)g_dev.completed);
            told = what;
            toldSince = since;
        }
        // drawing on without swapping (or not drawing at all) for 5 s
        if (g_activity.swaps != swaps) {
            swaps = g_activity.swaps;
            draws = g_activity.draws;
            still = 0;
        } else if (++still == 5) {
            logf("no swap for 5 s: %u draws since the last one (%s)", g_activity.draws - draws,
                 what ? what : "the GL thread is not waiting for the GPU");
        }
    }
}

uint32_t alignUp(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }

void debugCallback(void*, const char* context, DkResult result, const char* message) {
    logf("deko3d: %s (%s, result %d)", message ? message : "?", context ? context : "?", int(result));
}

const uint32_t kCmdChunk = 1u << 20;        // command memory per chunk
const uint32_t kTransientChunk = 4u << 20;  // transient memory per chunk
const uint32_t kSubmitBytes = 512u << 10;   // submit once this much is recorded
const uint32_t kImageDescriptors = 8192, kSamplerDescriptors = 2048;
const uint32_t kTimeSlots = 256;  // GPU timestamp pairs (more than lists in flight)

// Each command chunk ends with a guard deko3d is not told about: it writes a
// command's words after reserving room for them, and where it reserves too
// few (dkCmdBufBindVtxBuffers did, 5 words a buffer for 6: the console's GPU
// method errors, 2359-0001, after minutes of play) the rest went past the
// chunk into the memory after it, which another chunk or data then
// overwrote before the GPU read it. In the guard they stay the chunk's, and
// the GPU reads them as written. The log says when the command buffer moves
// on from a chunk whose guard was written; it is filled again when the chunk
// is used again (its commands done).
const uint32_t kCmdGuard = 0x1000;
const uint32_t kCmdGuardWord = 0x6b647567;  // (not a word deko3d writes there)

uint32_t* cmdGuard(const Alloc& a) { return reinterpret_cast<uint32_t*>(a.cpu + a.size - kCmdGuard); }

void fillCmdGuard(const Alloc& a) {
    uint32_t* g = cmdGuard(a);
    for (uint32_t i = 0; i < kCmdGuard / 4 && g[i] != kCmdGuardWord; i++) g[i] = kCmdGuardWord;
}

void checkCmdGuard(const Alloc& a) {
    const uint32_t* g = cmdGuard(a);
    uint32_t n = 0;
    while (n < kCmdGuard / 4 && g[n] != kCmdGuardWord) n++;
    static int said = 0;
    if (n && said++ < 8)
        logf("deko3d wrote %u words past a command chunk's end (into its guard; submission %llu)", n,
             (unsigned long long)g_dev.serial);
}

void addCommandMemory(void*, DkCmdBuf cmd, size_t minReqSize) {
    Alloc a;
    if (!g_dev.freeCmdChunks.empty() && minReqSize <= kCmdChunk) {
        a = g_dev.freeCmdChunks.back();
        g_dev.freeCmdChunks.pop_back();
        fillCmdGuard(a);
    } else {
        a = g_dev.heaps[HEAP_CMD].alloc(
            std::max<uint32_t>(kCmdChunk, alignUp(uint32_t(minReqSize), 0x1000)) + kCmdGuard, 0x100);
        if (a)
            for (uint32_t i = 0; i < kCmdGuard / 4; i++) cmdGuard(a)[i] = kCmdGuardWord;
    }
    if (!a) {
        logf("out of memory for commands");
        abort();
    }
    // the chunk filled so far ends with commands of this serial
    if (!g_dev.cmdChunks.empty()) {
        g_dev.cmdChunks.back().serial = g_dev.serial;
        checkCmdGuard(g_dev.cmdChunks.back().mem);
    }
    g_dev.cmdChunks.push_back({a, g_dev.serial});
    if (g_tracing) logf("  command chunk gpu 0x%llx-0x%llx", (unsigned long long)a.gpu, (unsigned long long)(a.gpu + a.size));
    g_dev.commandsSinceSubmit += a.size;
    dkCmdBufAddMemory(cmd, a.block, a.offset, a.size - kCmdGuard);
}
}  // namespace

// --- Heap ---------------------------------------------------------------------
void Heap::init(DkDevice dev, uint32_t flags, uint32_t chunk, int id) {
    m_dev = dev;
    m_flags = flags;
    m_chunk = chunk;
    m_id = id;
}

Alloc Heap::alloc(uint32_t size, uint32_t align) {
    Alloc r;
    if (!size) size = 1;
    if (align < 4) align = 4;
    for (int pass = 0; pass < 2; pass++) {
        for (Block& b : m_blocks) {
            for (auto it = b.freeRanges.begin(); it != b.freeRanges.end(); ++it) {
                const uint32_t start = alignUp(it->first, align);
                const uint32_t end = it->first + it->second;
                if (start + size > end || start < it->first) continue;
                const uint32_t rangeStart = it->first;
                b.freeRanges.erase(it);
                if (start > rangeStart) b.freeRanges[rangeStart] = start - rangeStart;
                if (start + size < end) b.freeRanges[start + size] = end - (start + size);
                r.block = b.mem;
                r.offset = start;
                r.size = size;
                r.heap = m_id;
                uint8_t* cpu = static_cast<uint8_t*>(dkMemBlockGetCpuAddr(b.mem));
                r.cpu = cpu ? cpu + start : nullptr;
                r.gpu = dkMemBlockGetGpuAddr(b.mem) + start;
                m_used += size;
                if (g_tracing) logf("  alloc heap %d gpu 0x%llx-0x%llx (%u bytes)", m_id, (unsigned long long)r.gpu,
                                    (unsigned long long)(r.gpu + size), size);
                return r;
            }
        }
        if (pass) break;
        // a new block: a chunk, or one of its own for a bigger request
        const uint32_t blockSize = alignUp(std::max(m_chunk, size + align), DK_MEMBLOCK_ALIGNMENT);
        DkMemBlockMaker mk;
        dkMemBlockMakerDefaults(&mk, m_dev, blockSize);
        mk.flags = m_flags;
        DkMemBlock mem = dkMemBlockCreate(&mk);
        if (!mem) {
            logf("cannot make a %u KiB memory block (heap %d, %llu KiB used)", blockSize >> 10, m_id,
                 (unsigned long long)(m_used >> 10));
            return r;
        }
        Block b;
        b.mem = mem;
        b.size = blockSize;
        b.freeRanges[0] = blockSize;
        m_blocks.push_back(std::move(b));
        __atomic_store_n(&m_reserved, m_reserved + blockSize, __ATOMIC_RELAXED);
    }
    return r;
}

void Heap::free(const Alloc& a) {
    if (!a) return;
    if (g_tracing) logf("  free heap %d gpu 0x%llx-0x%llx (%u bytes)", m_id, (unsigned long long)a.gpu,
                        (unsigned long long)(a.gpu + a.size), a.size);
    for (Block& b : m_blocks) {
        if (b.mem != a.block) continue;
        m_used -= a.size;
        uint32_t start = a.offset, size = a.size;
        auto next = b.freeRanges.lower_bound(start);
        if (next != b.freeRanges.end() && start + size == next->first) {
            size += next->second;
            next = b.freeRanges.erase(next);
        }
        if (next != b.freeRanges.begin()) {
            auto prev = std::prev(next);
            if (prev->first + prev->second == start) {
                start = prev->first;
                size += prev->second;
                b.freeRanges.erase(prev);
            }
        }
        b.freeRanges[start] = size;
        return;
    }
}

void Heap::logBlocks() const {
    for (size_t i = 0; i < m_blocks.size(); i++) {
        const DkGpuAddr gpu = dkMemBlockGetGpuAddr(m_blocks[i].mem);
        uint64_t freeBytes = 0;
        for (const auto& r : m_blocks[i].freeRanges) freeBytes += r.second;
        logf("  heap %d block %u: gpu 0x%llx-0x%llx, %llu KiB free", m_id, unsigned(i), (unsigned long long)gpu,
             (unsigned long long)(gpu + m_blocks[i].size), (unsigned long long)(freeBytes >> 10));
    }
}

bool Heap::describe(DkGpuAddr addr, char* buf, size_t n) const {
    for (size_t i = 0; i < m_blocks.size(); i++) {
        const Block& b = m_blocks[i];
        const DkGpuAddr gpu = dkMemBlockGetGpuAddr(b.mem);
        if (addr < gpu || addr >= gpu + b.size) continue;
        const uint32_t off = uint32_t(addr - gpu);
        auto it = b.freeRanges.upper_bound(off);
        const bool isFree = it != b.freeRanges.begin() && off < std::prev(it)->first + std::prev(it)->second;
        snprintf(buf, n, "heap %d block %u + 0x%x (%s)", m_id, unsigned(i), off, isFree ? "free" : "in use");
        return true;
    }
    for (size_t i = 0; i < m_blocks.size(); i++) {
        const DkGpuAddr end = dkMemBlockGetGpuAddr(m_blocks[i].mem) + m_blocks[i].size;
        if (addr >= end && addr < end + (16u << 20))
            snprintf(buf, n, "0x%llx past the end of heap %d block %u", (unsigned long long)(addr - end), m_id,
                     unsigned(i));
    }
    return false;
}

void Heap::logWord(uint32_t value, int* budget) const {
    for (size_t i = 0; i < m_blocks.size() && *budget > 0; i++) {
        const uint32_t* w = static_cast<const uint32_t*>(dkMemBlockGetCpuAddr(m_blocks[i].mem));
        if (!w) return;  // GPU-only memory
        const DkGpuAddr gpu = dkMemBlockGetGpuAddr(m_blocks[i].mem);
        const size_t n = m_blocks[i].size / 4;
        for (size_t k = 0; k < n && *budget > 0; k++) {
            if (w[k] != value) continue;
            --*budget;
            char where[128];
            describe(gpu + k * 4, where, sizeof where);
            logf("  0x%08x at gpu 0x%llx, %s:", value, (unsigned long long)(gpu + k * 4), where);
            char words[200];
            int len = 0;
            for (size_t j = k >= 8 ? k - 8 : 0; j < n && j <= k + 4; j++)
                len += snprintf(words + len, sizeof words - size_t(len), j == k ? " [%08x]" : " %08x", w[j]);
            logf("   %s", words);
            logObjectsAt(gpu + k * 4);
        }
    }
}

void memoryStats(uint64_t used[5], uint64_t reserved[5]) {
    for (int i = 0; i < 5; i++) {
        used[i] = i < HEAP_COUNT ? g_dev.heaps[i].usedRelaxed() : 0;
        reserved[i] = i < HEAP_COUNT ? g_dev.heaps[i].reserved() : 0;
    }
}

// --- submissions ---------------------------------------------------------------
bool g_pollNoFree = false;  // (a test: poll() updates `completed` only)
void poll() {
    while (!g_dev.inFlight.empty()) {
        Device::Submitted& s = g_dev.inFlight.front();
        if (dkFenceWait(&s.fence, 0) != DkResult_Success) break;
        g_dev.completed = s.serial;
        if (s.timeSlot >= 0) {
            const uint32_t at = uint32_t(s.timeSlot) * 32;
            dkMemBlockFlushCpuCache(g_dev.gpuTimes.block, g_dev.gpuTimes.offset + at, 32);
            // (a report: its counter, 0 for a timestamp, then the time in ns)
            uint64_t t0, t1;
            memcpy(&t0, g_dev.gpuTimes.cpu + at + 8, 8);
            memcpy(&t1, g_dev.gpuTimes.cpu + at + 16 + 8, 8);
            if (t1 > t0 && t1 - t0 < 1000000000ull) g_dev.gpuBusyNs += t1 - t0;
        }
        g_dev.inFlight.pop_front();
    }
    if (g_pollNoFree) return;
    const uint64_t done = g_dev.completed;
    // SMS_DKGL_NO_FREE=1: retired memory is never reused (a test for memory
    // freed while still in use; it runs out in time)
    static const bool keep = getenv("SMS_DKGL_NO_FREE") && atoi(getenv("SMS_DKGL_NO_FREE"));
    while (!keep && !g_dev.retired.empty() && g_dev.retired.front().serial <= done) {
        const Alloc& a = g_dev.retired.front().alloc;
        g_dev.heaps[a.heap].free(a);
        g_dev.retired.pop_front();
    }
    // (SMS_DKGL_KEEP_CMD=1, SMS_DKGL_KEEP_TRANSIENT=1: that memory never
    // used again, tests)
    static const bool keepCmd = getenv("SMS_DKGL_KEEP_CMD") && atoi(getenv("SMS_DKGL_KEEP_CMD"));
    static const bool keepTransient = getenv("SMS_DKGL_KEEP_TRANSIENT") && atoi(getenv("SMS_DKGL_KEEP_TRANSIENT"));
    // (the last chunk is the one the command buffer is writing into)
    while (g_dev.cmdChunks.size() > 1 && g_dev.cmdChunks.front().serial <= done) {
        if (!keepCmd) g_dev.freeCmdChunks.push_back(g_dev.cmdChunks.front().mem);
        g_dev.cmdChunks.pop_front();
    }
    while (!g_dev.transientRetired.empty() && g_dev.transientRetired.front().serial <= done) {
        if (!keepTransient) g_dev.freeTransient.push_back(g_dev.transientRetired.front().mem);
        g_dev.transientRetired.pop_front();
    }
    while (!g_dev.retiredImageDescs.empty() && g_dev.retiredImageDescs.front().first <= done) {
        g_dev.freeImageDescs.push_back(g_dev.retiredImageDescs.front().second);
        g_dev.retiredImageDescs.pop_front();
    }
}

// A GPU page fault's address, from deko3d's report of the queue's error
// (dkErrorReport, below), 0 if none; a method error's data.
uint64_t g_gpuFaultAddress = 0;
uint32_t g_gpuMethodData = 0;
bool g_gpuMethodError = false;

// Once the GPU has failed the queue (a page fault, a bad command, a timeout),
// deko3d aborts the program at the next submission (2359-0001), so first the
// log gets what dkgl knows: its memory blocks, and what holds a fault's
// address. (deko3d finds the error in a fence wait.)
void checkQueue(const char* found) {
    static bool said = false;
    if (said || !dkQueueIsInErrorState(g_dev.queue)) return;
    said = true;
    logf("the GPU failed the queue (found %s): submission %llu being recorded, %llu finished, %u draws so far", found,
         (unsigned long long)g_dev.serial, (unsigned long long)g_dev.completed, unsigned(g_activity.draws));
    for (const Heap& h : g_dev.heaps) h.logBlocks();
    if (__atomic_load_n(&g_gpuMethodError, __ATOMIC_ACQUIRE)) {
        // a method error: commands the GPU found wrong, as once floats
        // executed as commands; where their data is in memory tells whether
        // the commands were overwritten (in command memory) or the GPU read
        // commands from data (only elsewhere)
        const uint32_t data = g_gpuMethodData;
        logf("where the method's data 0x%08x is in memory:", data);
        int budget = 12;
        for (int heap : {HEAP_CMD, HEAP_DATA, HEAP_READBACK, HEAP_CODE}) g_dev.heaps[heap].logWord(data, &budget);
        if (budget == 12) logf("  nowhere");
    }
    const uint64_t addr = __atomic_load_n(&g_gpuFaultAddress, __ATOMIC_RELAXED);
    if (!addr) return;
    char where[128] = "in no memory block";
    for (const Heap& h : g_dev.heaps)
        if (h.describe(addr, where, sizeof where)) break;
    logf("fault address 0x%llx: %s", (unsigned long long)addr, where);
    logObjectsAt(addr);
}

// Waits for a fence, saying so in the log when the GPU takes over 2 seconds
// (a GPU hang otherwise leaves nothing there).
void waitFence(DkFence* f, uint64_t serial) {
    Waiting w("waiting for the GPU");
    if (dkFenceWait(f, 2000000000ll) != DkResult_Timeout) {
        checkQueue("waiting for a submission");
        return;
    }
    logf("the GPU has not finished submission %llu after 2 s (%llu submitted, %llu finished)",
         (unsigned long long)serial, (unsigned long long)g_dev.serial - 1, (unsigned long long)g_dev.completed);
    dkFenceWait(f, -1);
    logf("submission %llu finished", (unsigned long long)serial);
    checkQueue("after a long wait");
}

void startWatchdog() {
    static bool started = false;
    if (started) return;
    started = true;
    if (R_SUCCEEDED(threadCreate(&s_watchdog, watchdog, nullptr, nullptr, 0x4000, 0x20, 2)))
        threadStart(&s_watchdog);
}

// The start of each command list. The CPU writes what the list's commands
// read (vertices, indices, uniform blocks, descriptors) in CPU-uncached
// memory that the GPU caches in its L2, which does not see those writes: the
// GPU could read a ring slot's or transient chunk's last contents from an
// earlier list instead (stale lights, uniforms and vertices: the self-test's
// lighting, the menus' streaks on the console; emulators have no such cache).
// Everything the list reads is written before it is submitted, so dropping
// the L2's lines once at its start is enough.
void startList() {
    // (SMS_DKGL_LIST_INVALIDATE=all: every cache, a test)
    static const bool all = getenv("SMS_DKGL_LIST_INVALIDATE") && !strcmp(getenv("SMS_DKGL_LIST_INVALIDATE"), "all");
    dkCmdBufBarrier(g_dev.cmd, DkBarrier_None,
                    all ? DkInvalidateFlags_L2Cache | DkInvalidateFlags_Image | DkInvalidateFlags_Shader |
                              DkInvalidateFlags_Descriptors | DkInvalidateFlags_Zcull
                        : DkInvalidateFlags_L2Cache);
    dkCmdBufBindImageDescriptorSet(g_dev.cmd, g_dev.imageDescs.gpu, kImageDescriptors);
    dkCmdBufBindSamplerDescriptorSet(g_dev.cmd, g_dev.samplerDescs.gpu, kSamplerDescriptors);
    // (SMS_DKGL_TILED=off: the GPU's tiled caching disabled, a test)
    static const char* tiled = getenv("SMS_DKGL_TILED");
    if (tiled && !strcmp(tiled, "off")) dkCmdBufTiledCacheOp(g_dev.cmd, DkTiledCacheOp_Disable);
    g_dev.samplerUsed.assign(kSamplerDescriptors, false);
    g_dev.imageUsed.assign(kImageDescriptors, false);
    // (SMS_TIMING_LOG: the GPU's time per list, timestamps at its start and end)
    if (g_dev.gpuTimes) {
        g_dev.timeSlot = int(g_dev.nextTimeSlot++ % kTimeSlots);
        dkCmdBufReportCounter(g_dev.cmd, DkCounter_Timestamp, g_dev.gpuTimes.gpu + uint32_t(g_dev.timeSlot) * 32);
    }
}

void submit(bool wait) {
    checkInputsAtSubmit();
    // the chunk the command buffer is writing into stays with this serial
    if (!g_dev.cmdChunks.empty()) g_dev.cmdChunks.back().serial = g_dev.serial;
    const int slot = g_dev.timeSlot;
    if (slot >= 0)
        dkCmdBufReportCounter(g_dev.cmd, DkCounter_Timestamp, g_dev.gpuTimes.gpu + uint32_t(slot) * 32 + 16);
    DkCmdList list = dkCmdBufFinishList(g_dev.cmd);
    checkQueue("at a submission");
    if (list) dkQueueSubmitCommands(g_dev.queue, list);
    g_dev.inFlight.push_back({g_dev.serial, {}, slot});
    dkQueueSignalFence(g_dev.queue, &g_dev.inFlight.back().fence, true);
    dkQueueFlush(g_dev.queue);
    // the transient chunk being filled ends with this serial too
    g_dev.transient.serial = g_dev.serial;
    g_dev.serial++;
    g_dev.commandsSinceSubmit = 0;
    // descriptor sets and everything else bound stay bound in the queue;
    // the command buffer's next list starts in its remaining memory
    startList();
    if (wait) {
        waitFence(&g_dev.inFlight.back().fence, g_dev.inFlight.back().serial);
    }
    poll();
}

void waitSerial(uint64_t serial) {
    if (serial >= g_dev.serial) submit();
    while (g_dev.completed < serial && !g_dev.inFlight.empty()) {
        waitFence(&g_dev.inFlight.front().fence, g_dev.inFlight.front().serial);
        poll();
    }
}

void retire(const Alloc& a) {
    if (a) g_dev.retired.push_back({g_dev.serial, a});
}

Alloc transient(uint32_t size, uint32_t align) {
    Device::Transient& t = g_dev.transient;
    if (size > kTransientChunk / 2) {
        // too big to share a chunk: its own memory, freed after this serial
        Alloc a = g_dev.heaps[HEAP_DATA].alloc(size, std::max<uint32_t>(align, 0x100));
        retire(a);
        return a;
    }
    uint32_t at = t.mem ? alignUp(t.used, align) : 0;
    if (!t.mem || at + size > t.mem.size) {
        if (t.mem) {
            t.serial = g_dev.serial;
            g_dev.transientRetired.push_back(t);
        }
        poll();
        if (!g_dev.freeTransient.empty()) {
            t.mem = g_dev.freeTransient.back();
            g_dev.freeTransient.pop_back();
        } else {
            t.mem = g_dev.heaps[HEAP_DATA].alloc(kTransientChunk, 0x1000);
        }
        if (g_tracing) logf("  transient chunk gpu 0x%llx", (unsigned long long)t.mem.gpu);
        t.used = 0;
        at = 0;
    }
    Alloc a = t.mem;
    a.offset += at;
    a.size = size;
    a.cpu += at;
    a.gpu += at;
    t.used = at + size;
    return a;
}

// --- descriptors -----------------------------------------------------------------
uint32_t newImageDescriptor() {
    poll();
    if (!g_dev.freeImageDescs.empty()) {
        const uint32_t s = g_dev.freeImageDescs.back();
        g_dev.freeImageDescs.pop_back();
        return s;
    }
    if (g_dev.imageDescCount >= kImageDescriptors) {
        // wait for retired slots
        waitSerial(g_dev.serial - 1);
        poll();
        if (!g_dev.freeImageDescs.empty()) {
            const uint32_t s = g_dev.freeImageDescs.back();
            g_dev.freeImageDescs.pop_back();
            return s;
        }
        logf("out of image descriptors");
        return 0;
    }
    return g_dev.imageDescCount++;
}

void writeImageDescriptor(uint32_t slot, const DkImageDescriptor& d) {
    // a slot no queued draw uses (a changed texture gets a new one), so
    // written directly; the descriptor cache is invalidated before the next draw
    memcpy(g_dev.imageDescs.cpu + slot * sizeof(DkImageDescriptor), &d, sizeof d);
    g_dev.descriptorsDirty = true;
    if (g_tracing) logf("  image descriptor %u written", slot);
}

void retireImageDescriptor(uint32_t slot) {
    if (slot != ~0u) g_dev.retiredImageDescs.push_back({g_dev.serial, slot});
}

uint32_t samplerDescriptor(const DkSampler& s) {
    const std::string key(reinterpret_cast<const char*>(&s), sizeof s);
    auto it = g_dev.samplerIds.find(key);
    if (it != g_dev.samplerIds.end()) return it->second;
    if (g_dev.samplerDescCount >= kSamplerDescriptors) {
        logf("out of sampler descriptors");
        return 0;
    }
    const uint32_t id = g_dev.samplerDescCount++;
    DkSamplerDescriptor d;
    dkSamplerDescriptorInitialize(&d, &s);
    // a slot never used before: written directly
    memcpy(g_dev.samplerDescs.cpu + id * sizeof(DkSamplerDescriptor), &d, sizeof d);
    g_dev.descriptorsDirty = true;
    g_dev.samplersAdded = true;
    if (g_tracing) logf("  new sampler descriptor %u", id);
    g_dev.samplerIds[key] = id;
    return id;
}

// --- start ---------------------------------------------------------------------
bool initDevice(int width, int height) {
    DkDeviceMaker dm;
    dkDeviceMakerDefaults(&dm);
    dm.cbDebug = debugCallback;
    // GL's conventions: clip-space depth from -1, and the lower-left origin,
    // where clip-space y points up and an image's row 0 is GL's y = 0 (in
    // upper-left mode deko3d flips y as Direct3D does, which turned every
    // draw upside down against the copies and reads); the compositor is told
    // to show the swapchain's images flipped
    dm.flags = DkDeviceFlags_DepthMinusOneToOne | DkDeviceFlags_OriginLowerLeft;
    logf("starting: device");
    g_dev.dev = dkDeviceCreate(&dm);
    if (!g_dev.dev) return false;
    g_dev.heaps[HEAP_DATA].init(g_dev.dev, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached, 32u << 20, HEAP_DATA);
    g_dev.heaps[HEAP_READBACK].init(g_dev.dev, DkMemBlockFlags_CpuCached | DkMemBlockFlags_GpuCached, 8u << 20,
                                    HEAP_READBACK);
    g_dev.heaps[HEAP_IMAGE].init(g_dev.dev, DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image, 64u << 20, HEAP_IMAGE);
    g_dev.heaps[HEAP_CODE].init(g_dev.dev,
                                DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code,
                                8u << 20, HEAP_CODE);
    g_dev.heaps[HEAP_CMD].init(g_dev.dev, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached, 4u << 20, HEAP_CMD);

    DkQueueMaker qm;
    dkQueueMakerDefaults(&qm, g_dev.dev);
    qm.flags = DkQueueFlags_Graphics | DkQueueFlags_MediumPrio;
    // (SMS_DKGL_NO_ZCULL=1: without the GPU's coarse depth culling, a test)
    if (getenv("SMS_DKGL_NO_ZCULL") && atoi(getenv("SMS_DKGL_NO_ZCULL"))) qm.flags |= DkQueueFlags_DisableZcull;
    logf("starting: queue");
    g_dev.queue = dkQueueCreate(&qm);
    if (!g_dev.queue) return false;

    DkCmdBufMaker cm;
    dkCmdBufMakerDefaults(&cm, g_dev.dev);
    cm.cbAddMem = addCommandMemory;
    g_dev.cmd = dkCmdBufCreate(&cm);

    g_dev.imageDescs = g_dev.heaps[HEAP_DATA].alloc(kImageDescriptors * sizeof(DkImageDescriptor), 0x100);
    g_dev.samplerDescs = g_dev.heaps[HEAP_DATA].alloc(kSamplerDescriptors * sizeof(DkSamplerDescriptor), 0x100);
    memset(g_dev.imageDescs.cpu, 0, g_dev.imageDescs.size);
    memset(g_dev.samplerDescs.cpu, 0, g_dev.samplerDescs.size);
    if (getenv("SMS_TIMING_LOG") && atoi(getenv("SMS_TIMING_LOG")))
        g_dev.gpuTimes = g_dev.heaps[HEAP_READBACK].alloc(kTimeSlots * 32, 32);
    startList();

    logf("starting: swapchain images, %dx%d", width, height);
    // the swapchain's images
    g_dev.width = width;
    g_dev.height = height;
    DkImageLayoutMaker lm;
    dkImageLayoutMakerDefaults(&lm, g_dev.dev);
    lm.flags = DkImageFlags_UsageRender | DkImageFlags_UsagePresent | DkImageFlags_Usage2DEngine;
    lm.format = DkImageFormat_RGBA8_Unorm;
    lm.dimensions[0] = uint32_t(width);
    lm.dimensions[1] = uint32_t(height);
    DkImageLayout& layout = g_dev.swapLayout;
    dkImageLayoutInitialize(&layout, &lm);
    const DkImage* images[3];
    for (int i = 0; i < 3; i++) {
        g_dev.swapMem[i] = g_dev.heaps[HEAP_IMAGE].alloc(uint32_t(dkImageLayoutGetSize(&layout)),
                                                          dkImageLayoutGetAlignment(&layout));
        if (!g_dev.swapMem[i]) return false;
        dkImageInitialize(&g_dev.swapImages[i], &layout, g_dev.swapMem[i].block, g_dev.swapMem[i].offset);
        images[i] = &g_dev.swapImages[i];
    }
    logf("starting: swapchain");
    DkSwapchainMaker sm;
    dkSwapchainMakerDefaults(&sm, g_dev.dev, nwindowGetDefault(), images, 3);
    g_dev.swapchain = dkSwapchainCreate(&sm);
    if (!g_dev.swapchain) return false;
    logf("deko3d device ready, %dx%d swapchain", width, height);
    return true;
}

void shutdownDevice() {
    if (!g_dev.dev) return;
    if (g_dev.queue) dkQueueWaitIdle(g_dev.queue);
    if (g_dev.swapchain) dkSwapchainDestroy(g_dev.swapchain);
    g_dev.swapchain = nullptr;
    // memory blocks go with the device
    if (g_dev.cmd) dkCmdBufDestroy(g_dev.cmd);
    if (g_dev.queue) dkQueueDestroy(g_dev.queue);
    dkDeviceDestroy(g_dev.dev);
    g_dev = Device();
}

}  // namespace dkgl

// deko3d's report of the queue's error (tools/deko3d/deko3d-error-report.patch),
// with a page fault's address kept for checkQueue.
extern "C" void dkErrorReport(const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    va_list copy;
    va_copy(copy, ap);
    vsnprintf(buf, sizeof buf, fmt, copy);
    va_end(copy);
    if (strstr(fmt, "Address:")) {  // "  Address: 0x%02x%08x"
        const uint32_t hi = va_arg(ap, uint32_t), lo = va_arg(ap, uint32_t);
        __atomic_store_n(&dkgl::g_gpuFaultAddress, (uint64_t(hi) << 32) | lo, __ATOMIC_RELAXED);
    } else if (strstr(fmt, "] = 0x")) {  // a method error's "  [%04x:%03x] = 0x%08x" (class, method, data)
        va_arg(ap, uint32_t);
        va_arg(ap, uint32_t);
        dkgl::g_gpuMethodData = va_arg(ap, uint32_t);
        __atomic_store_n(&dkgl::g_gpuMethodError, true, __ATOMIC_RELEASE);
    }
    va_end(ap);
    dkgl::logf("deko3d:%s", buf);
}
