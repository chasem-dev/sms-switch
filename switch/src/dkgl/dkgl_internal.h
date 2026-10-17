// dkgl's objects and the device under them (dkgl.h).
#ifndef SMS_SWITCH_DKGL_INTERNAL_H
#define SMS_SWITCH_DKGL_INTERNAL_H

#include <deko3d.h>
#include <GL/glcorearb.h>
#include <stdint.h>
#include <string.h>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "dkgl_glsl.h"

namespace dkgl {

void logf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// What the GL thread is waiting for (a string literal, or null when it is
// not waiting), and its draws and swaps so far: a watchdog thread logs a wait
// of over 3 seconds, and whether the GL thread still draws (startWatchdog).
struct Activity {
    const char* volatile waiting = nullptr;
    volatile uint64_t since = 0;  // system tick the wait began
    volatile uint32_t draws = 0, swaps = 0;
};
extern Activity g_activity;
struct Waiting {
    const char* prev;
    explicit Waiting(const char* what);
    ~Waiting();
};
void startWatchdog();

// --- memory ----------------------------------------------------------------
// A piece of one of the heaps' memory blocks.
struct Alloc {
    DkMemBlock block = nullptr;
    uint32_t offset = 0, size = 0;
    uint8_t* cpu = nullptr;  // null for GPU-only memory
    DkGpuAddr gpu = DK_GPU_ADDR_INVALID;
    int heap = -1;
    explicit operator bool() const { return block != nullptr; }
};

enum HeapId {
    HEAP_DATA,      // CPU-uncached, GPU-cached: buffers, uploads, commands, descriptors
    HEAP_READBACK,  // CPU-cached: what the CPU reads back (pixel pack buffers)
    HEAP_IMAGE,     // GPU-only images: textures, render targets
    HEAP_CODE,      // shader code
    HEAP_COUNT
};

// First-fit over memory blocks of `chunk` bytes (bigger requests get a block
// of their own), free ranges merged.
class Heap {
public:
    void init(DkDevice dev, uint32_t flags, uint32_t chunk, int id);
    Alloc alloc(uint32_t size, uint32_t align);
    void free(const Alloc& a);
    uint64_t used() const { return m_used; }

private:
    struct Block {
        DkMemBlock mem;
        uint32_t size;
        std::map<uint32_t, uint32_t> freeRanges;  // offset -> size
    };
    DkDevice m_dev = nullptr;
    uint32_t m_flags = 0, m_chunk = 0;
    int m_id = 0;
    std::vector<Block> m_blocks;
    uint64_t m_used = 0;
};

// --- the device ------------------------------------------------------------
struct Device {
    DkDevice dev = nullptr;
    DkQueue queue = nullptr;
    DkCmdBuf cmd = nullptr;
    Heap heaps[HEAP_COUNT];

    // Submissions: commands are recorded under `serial`; submit() hands them
    // to the queue with a fence and starts the next serial.
    uint64_t serial = 1;
    uint64_t completed = 0;  // every serial up to this one has finished
    struct Submitted {
        uint64_t serial;
        DkFence fence;
    };
    std::deque<Submitted> inFlight;
    uint32_t commandsSinceSubmit = 0;  // bytes of command memory used

    // Memory freed while the GPU may still use it, returned once its serial ends.
    struct Retired {
        uint64_t serial;
        Alloc alloc;
    };
    std::deque<Retired> retired;

    // Command memory: chunks handed to the command buffer, reused once their serial ends.
    struct CmdChunk {
        Alloc mem;
        uint64_t serial;
    };
    std::deque<CmdChunk> cmdChunks;  // in use, oldest first
    std::vector<Alloc> freeCmdChunks;

    // Transient memory (uniform blocks, constant attributes, staging for
    // uploads): bump allocation in chunks reused once their serial ends.
    struct Transient {
        Alloc mem;
        uint32_t used = 0;
        uint64_t serial = 0;
    };
    Transient transient;
    std::deque<Transient> transientRetired;
    std::vector<Alloc> freeTransient;

    // Descriptor tables.
    Alloc imageDescs, samplerDescs;
    uint32_t imageDescCount = 0, samplerDescCount = 0;
    std::vector<uint32_t> freeImageDescs;
    std::deque<std::pair<uint64_t, uint32_t>> retiredImageDescs;
    std::unordered_map<std::string, uint32_t> samplerIds;  // DkSampler bytes -> slot
    bool descriptorsDirty = false;  // written since the last invalidate

    // The swapchain and the default framebuffer drawn into before it.
    DkSwapchain swapchain = nullptr;
    Alloc swapMem[3];
    DkImage swapImages[3];
    int width = 0, height = 0;
};
extern Device g_dev;

bool initDevice(int width, int height);
void shutdownDevice();
void submit(bool wait = false);
void poll();  // updates g_dev.completed and returns what has finished
void waitSerial(uint64_t serial);
void retire(const Alloc& a);
// Transient memory, written by the CPU now, read by commands recorded now.
Alloc transient(uint32_t size, uint32_t align);
uint32_t newImageDescriptor();
void writeImageDescriptor(uint32_t slot, const DkImageDescriptor& d);
void retireImageDescriptor(uint32_t slot);
uint32_t samplerDescriptor(const DkSampler& s);

// --- GL objects ------------------------------------------------------------
struct Buffer {
    Alloc mem;
    uint32_t size = 0;
    GLenum usage = 0;
    bool immutable = false;
    // a map that is not the memory itself: written back on unmap
    void* mapPtr = nullptr;
    uint32_t mapOffset = 0, mapLength = 0;
    GLbitfield mapAccess = 0;
    std::vector<uint8_t> mapShadow;
};

struct SamplerParams {
    GLenum minFilter = GL_NEAREST_MIPMAP_LINEAR, magFilter = GL_LINEAR;
    GLenum wrapS = GL_REPEAT, wrapT = GL_REPEAT, wrapR = GL_REPEAT;
    float minLod = -1000.0f, maxLod = 1000.0f, lodBias = 0.0f, maxAniso = 1.0f;
};

struct Texture {
    struct Level {
        uint32_t w = 0, h = 0;
        bool defined = false;
        std::vector<uint8_t> pending;  // data given before the image existed
        bool pendingCompressed = false;
    };
    GLenum internalFormat = 0;
    DkImageFormat format = DkImageFormat_None;
    bool compressed = false, depth = false;
    uint32_t samples = 1;  // renderbuffers
    std::vector<Level> levels;
    int baseLevel = 0, maxLevel = 1000;
    GLint swizzle[4] = {GL_RED, GL_GREEN, GL_BLUE, GL_ALPHA};
    SamplerParams sampler;

    // the deko3d image, made at first use with the levels then defined
    bool realized = false;
    DkImageLayout layout;
    DkImage image;
    Alloc mem;
    uint32_t imageLevels = 0;
    uint32_t desc = ~0u;       // image descriptor slot
    bool descDirty = true;     // swizzle or levels changed
    bool isRenderbuffer = false;
    // the handle last made, and the sampler settings it was made with: most
    // draws sample a texture as the draw before did
    SamplerParams handleParams;
    DkResHandle handle = 0;
    bool handleValid = false;
};

struct Renderbuffer;  // a Texture with isRenderbuffer

struct Attachment {
    GLuint texture = 0;      // or renderbuffer
    bool renderbuffer = false;
    int level = 0;
};
struct Framebuffer {
    Attachment color, depth;
};

struct VertexAttrib {
    bool enabled = false;
    GLuint buffer = 0;
    GLint size = 4;
    GLenum type = GL_FLOAT;
    bool normalized = false, integer = false;
    GLsizei stride = 0;
    uintptr_t offset = 0;
};
struct VertexArray {
    VertexAttrib attribs[16];
    GLuint elementBuffer = 0;
    uint32_t version = 1;  // bumped on any change
};

struct Shader {
    GLenum type = 0;
    std::string source;
};

struct Program {
    std::vector<GLuint> shaders;
    std::map<std::string, int> attribBindings, fragBindings;
    bool linked = false;
    std::string infoLog;
    GlslProgram glsl;
    DkShader vs, gs, fs;
    bool hasGs = false;
    Alloc code;
    std::vector<uint8_t> defaultBlock;  // the loose uniforms' bytes
    bool defaultDirty = true;
    std::vector<int> samplerUnits;     // texture binding -> GL texture unit
    uint32_t samplerStages[kMaxSamplerBindings] = {};
    int samplerCount = 0;
    std::vector<int> blockBindings;    // block index -> GL uniform buffer binding
    uint32_t version = 1;              // bumped when samplers or blocks are re-pointed
    uint32_t attribMask = 0;           // vertex input locations read
};

struct Query {
    GLenum target = 0;
    Alloc mem;  // two 16-byte counter reports
    uint64_t serial = 0;
};

struct Sync {
    DkFence fence;
    uint64_t serial;
    bool submitted;
};

}  // namespace dkgl

#endif
