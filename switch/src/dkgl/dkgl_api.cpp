// dkgl's GL entry points (the ones in sms_gx's gl_funcs.h), dkgl::getProc,
// and presenting.
#include <stdlib.h>
#include <switch.h>
#include "dkgl_state.h"
#include "dkgl.h"

#include <math.h>
#include <stdio.h>
#include <algorithm>

namespace dkgl {

State g;
void setShaderCacheImpl(const char* dir, const char* pack);  // dkgl_program.cpp

namespace {

#define DKGL_API extern "C" __attribute__((visibility("hidden")))

void setError(GLenum e) {
    if (g.error == GL_NO_ERROR) g.error = e;
}

Buffer* boundBuffer(GLenum target) {
    GLuint n = 0;
    switch (target) {
    case GL_ARRAY_BUFFER: n = g.arrayBuffer; break;
    case GL_ELEMENT_ARRAY_BUFFER: n = currentVao()->elementBuffer; break;
    case GL_PIXEL_PACK_BUFFER: n = g.packBuffer; break;
    case GL_PIXEL_UNPACK_BUFFER: n = g.unpackBuffer; break;
    case GL_UNIFORM_BUFFER: n = g.uniformBuffer; break;
    default: break;
    }
    return g.buffers.get(n);
}

int heapForUsage(GLenum usage) {
    return usage == GL_STREAM_READ || usage == GL_DYNAMIC_READ || usage == GL_STATIC_READ ? HEAP_READBACK : HEAP_DATA;
}

// (Re)makes a buffer's storage; the old one is freed once the GPU is done.
void bufferStorage(Buffer* b, GLsizeiptr size, const void* data, GLenum usage) {
    retire(b->mem);
    b->mem = g_dev.heaps[heapForUsage(usage)].alloc(uint32_t(std::max<GLsizeiptr>(size, 4)), 256);
    b->size = uint32_t(size);
    b->usage = usage;
    if (data && b->mem) {
        memcpy(b->mem.cpu, data, size_t(size));
        if (b->mem.heap == HEAP_READBACK) dkMemBlockFlushCpuCache(b->mem.block, b->mem.offset, b->mem.size);
    }
    // vertex arrays and uniform blocks that point into it bind again
    for (auto& va : g.vertexArrays.objs)
        if (va) va->version++;
    g.defaultVao.version++;
    memset(g.boundUbo, 0, sizeof g.boundUbo);
}

Texture* boundTexture2D() { return g.textures.get(g.tex2D[g.activeUnit]); }

Program* currentProgram() { return g.programs.get(g.program); }

void setCap(GLenum cap, bool on) {
    switch (cap) {
    case GL_BLEND: g.blend = on; g.dirty |= D_COLOR; break;
    case GL_DEPTH_TEST: g.depthTest = on; g.dirty |= D_DEPTH; break;
    case GL_CULL_FACE: g.cullFace = on; g.dirty |= D_RASTER; break;
    case GL_SCISSOR_TEST: g.scissorTest = on; g.dirty |= D_SCISSOR; break;
    case GL_COLOR_LOGIC_OP: g.logicOp = on; g.dirty |= D_COLOR; break;
    case GL_CLIP_DISTANCE0: g.clip0 = on; break;  // the shaders write clip distances only where they clip
    case GL_CLIP_DISTANCE1: g.clip1 = on; break;
    case GL_DEPTH_CLAMP: g.depthClamp = on; g.dirty |= D_RASTER; break;
    case GL_STENCIL_TEST: g.stencilTest = on; break;
    default: break;
    }
}

bool getCap(GLenum cap) {
    switch (cap) {
    case GL_BLEND: return g.blend;
    case GL_DEPTH_TEST: return g.depthTest;
    case GL_CULL_FACE: return g.cullFace;
    case GL_SCISSOR_TEST: return g.scissorTest;
    case GL_COLOR_LOGIC_OP: return g.logicOp;
    case GL_CLIP_DISTANCE0: return g.clip0;
    case GL_CLIP_DISTANCE1: return g.clip1;
    case GL_DEPTH_CLAMP: return g.depthClamp;
    case GL_STENCIL_TEST: return g.stencilTest;
    default: return false;
    }
}

DkPrimitive primitiveOf(GLenum mode) {
    switch (mode) {
    case GL_POINTS: return DkPrimitive_Points;
    case GL_LINES: return DkPrimitive_Lines;
    case GL_LINE_LOOP: return DkPrimitive_LineLoop;
    case GL_LINE_STRIP: return DkPrimitive_LineStrip;
    case GL_TRIANGLE_STRIP: return DkPrimitive_TriangleStrip;
    case GL_TRIANGLE_FAN: return DkPrimitive_TriangleFan;
    default: return DkPrimitive_Triangles;
    }
}

// The draws recorded since the last submission, so the GPU starts on a frame
// while the rest of it is recorded.
uint32_t s_drawsSinceSubmit = 0;
void drawn() {
    g.rendered = true;
    if (++s_drawsSinceSubmit >= 256) {
        s_drawsSinceSubmit = 0;
        submit();
    }
}

void drawElements(GLenum mode, GLsizei count, GLenum type, const void* indices, GLint baseVertex) {
    if (count <= 0 || !validateDraw()) return;
    const Buffer* ib = g.buffers.get(currentVao()->elementBuffer);
    if (!ib || !ib->mem) return;
    const uint32_t size = type == GL_UNSIGNED_INT ? 4 : type == GL_UNSIGNED_SHORT ? 2 : 1;
    const DkIdxFormat fmt = size == 4 ? DkIdxFormat_Uint32 : size == 2 ? DkIdxFormat_Uint16 : DkIdxFormat_Uint8;
    const uintptr_t off = reinterpret_cast<uintptr_t>(indices);
    dkCmdBufBindIdxBuffer(g_dev.cmd, fmt, ib->mem.gpu);
    dkCmdBufDrawIndexed(g_dev.cmd, primitiveOf(mode), uint32_t(count), 1, uint32_t(off / size), baseVertex, 0);
    traceDraw("indexed", mode, count, uint32_t(off / size), baseVertex, ib->mem.gpu + off);
    drawn();
}

// Converts and copies a framebuffer's pixels out (glReadPixels), into a
// pixel pack buffer or the caller's memory.
void readPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, void* pixels) {
    Texture* t = nullptr;
    int level = 0;
    if (g.readFbo == 0) {
        t = defaultFramebuffer();
    } else if (const Framebuffer* f = g.framebuffers.get(g.readFbo)) {
        const Attachment& a = format == GL_DEPTH_COMPONENT ? f->depth : f->color;
        t = attachmentTexture(a);
        level = a.level;
    }
    if (!t || !realize(t) || w <= 0 || h <= 0) return;
    const uint32_t bpp = 4;  // RGBA8 and 24-bit depth
    const uint32_t rowBytes = uint32_t(w) * bpp;
    Buffer* pack = g.buffers.get(g.packBuffer);
    barrierBeforeTransfer();
    DkImageView v = viewOf(t, level);
    DkImageRect rect = {uint32_t(x), uint32_t(y), 0, uint32_t(w), uint32_t(h), 1};
    if (pack && pack->mem) {
        // into the buffer, as the image holds them (the renderer reads depth as
        // its high 24 bits, where Z24S8 keeps it)
        DkCopyBuf dst = {pack->mem.gpu + reinterpret_cast<uintptr_t>(pixels), rowBytes, 0};
        dkCmdBufCopyImageToBuffer(g_dev.cmd, &v, &rect, &dst, 0);
        g.transferred = true;
        return;
    }
    Alloc tmp = g_dev.heaps[HEAP_READBACK].alloc(rowBytes * uint32_t(h), 256);
    if (!tmp) return;
    DkCopyBuf dst = {tmp.gpu, rowBytes, 0};
    dkCmdBufCopyImageToBuffer(g_dev.cmd, &v, &rect, &dst, 0);
    submit(true);
    dkMemBlockFlushCpuCache(tmp.block, tmp.offset, tmp.size);
    const uint32_t align = uint32_t(std::max(1, g.packAlign));
    const uint32_t rowLen = g.packRowLength ? uint32_t(g.packRowLength) : uint32_t(w);
    const uint32_t outBpp = format == GL_DEPTH_COMPONENT ? 4 : type == GL_UNSIGNED_BYTE && format == GL_RGBA ? 4 : 4;
    const uint32_t outStride = (rowLen * outBpp + align - 1) / align * align;
    for (int r = 0; r < h; r++) {
        uint8_t* out = static_cast<uint8_t*>(pixels) + size_t(r) * outStride;
        const uint8_t* in = tmp.cpu + size_t(r) * rowBytes;
        if (format == GL_DEPTH_COMPONENT) {
            for (int i = 0; i < w; i++) {
                uint32_t raw;
                memcpy(&raw, in + i * 4, 4);
                const uint32_t d24 = raw >> 8;
                const uint32_t v32 = (d24 << 8) | (d24 >> 16);
                memcpy(out + i * 4, &v32, 4);
            }
        } else if (format == GL_BGRA) {
            for (int i = 0; i < w; i++) {
                out[i * 4 + 0] = in[i * 4 + 2];
                out[i * 4 + 1] = in[i * 4 + 1];
                out[i * 4 + 2] = in[i * 4 + 0];
                out[i * 4 + 3] = in[i * 4 + 3];
            }
        } else {
            memcpy(out, in, rowBytes);
        }
    }
    g_dev.heaps[HEAP_READBACK].free(tmp);
}

// --- the entry points -------------------------------------------------------------
GLenum APIENTRY glGetError_(void) {
    const GLenum e = g.error;
    g.error = GL_NO_ERROR;
    return e;
}

const char* const kExtensions[] = {
    "GL_ARB_shader_image_load_store", "GL_EXT_texture_compression_s3tc", "GL_ARB_texture_compression_bptc",
    "GL_ARB_buffer_storage",          "GL_ARB_texture_filter_anisotropic",
};

const GLubyte* APIENTRY glGetString_(GLenum name) {
    switch (name) {
    case GL_VENDOR: return reinterpret_cast<const GLubyte*>("sms-switch");
    case GL_RENDERER: return reinterpret_cast<const GLubyte*>("deko3d (dkgl)");
    case GL_VERSION: return reinterpret_cast<const GLubyte*>("4.3 (Core Profile) dkgl");
    case GL_SHADING_LANGUAGE_VERSION: return reinterpret_cast<const GLubyte*>("4.30");
    case GL_EXTENSIONS: return reinterpret_cast<const GLubyte*>("");
    default: return nullptr;
    }
}

const GLubyte* APIENTRY glGetStringi_(GLenum name, GLuint index) {
    if (name == GL_EXTENSIONS && index < sizeof kExtensions / sizeof *kExtensions)
        return reinterpret_cast<const GLubyte*>(kExtensions[index]);
    return nullptr;
}

void APIENTRY glGetIntegerv_(GLenum pname, GLint* v) {
    if (!v) return;
    switch (pname) {
    case GL_MAJOR_VERSION: *v = 4; break;
    case GL_MINOR_VERSION: *v = 3; break;
    case GL_NUM_EXTENSIONS: *v = GLint(sizeof kExtensions / sizeof *kExtensions); break;
    case GL_NUM_PROGRAM_BINARY_FORMATS: *v = 0; break;  // dkgl caches compiled programs itself
    case GL_MAX_SAMPLES: *v = 8; break;
    case GL_MAX_TEXTURE_SIZE: *v = 16384; break;
    case GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT: *v = DK_UNIFORM_BUF_ALIGNMENT; break;
    case GL_MAX_UNIFORM_BLOCK_SIZE: *v = DK_UNIFORM_BUF_MAX_SIZE; break;
    case GL_ACTIVE_TEXTURE: *v = GLint(GL_TEXTURE0 + g.activeUnit); break;
    case GL_TEXTURE_BINDING_2D: *v = GLint(g.tex2D[g.activeUnit]); break;
    case GL_CURRENT_PROGRAM: *v = GLint(g.program); break;
    case GL_VERTEX_ARRAY_BINDING: *v = GLint(g.vao); break;
    case GL_DRAW_FRAMEBUFFER_BINDING: *v = GLint(g.drawFbo); break;
    case GL_READ_FRAMEBUFFER_BINDING: *v = GLint(g.readFbo); break;
    case GL_PIXEL_PACK_BUFFER_BINDING: *v = GLint(g.packBuffer); break;
    case GL_PIXEL_UNPACK_BUFFER_BINDING: *v = GLint(g.unpackBuffer); break;
    case GL_ARRAY_BUFFER_BINDING: *v = GLint(g.arrayBuffer); break;
    case GL_UNPACK_ALIGNMENT: *v = g.unpackAlign; break;
    case GL_UNPACK_ROW_LENGTH: *v = g.unpackRowLength; break;
    case GL_PACK_ALIGNMENT: *v = g.packAlign; break;
    case GL_PACK_ROW_LENGTH: *v = g.packRowLength; break;
    case GL_VIEWPORT: memcpy(v, g.viewport, sizeof g.viewport); break;
    case GL_SCISSOR_BOX: memcpy(v, g.scissor, sizeof g.scissor); break;
    case GL_COLOR_WRITEMASK: for (int i = 0; i < 4; i++) v[i] = g.colorMask[i]; break;
    case GL_DEPTH_WRITEMASK: *v = g.depthMask; break;
    case GL_BLEND_SRC_RGB: *v = GLint(g.blendFunc[0]); break;
    case GL_BLEND_DST_RGB: *v = GLint(g.blendFunc[1]); break;
    case GL_BLEND_SRC_ALPHA: *v = GLint(g.blendFunc[2]); break;
    case GL_BLEND_DST_ALPHA: *v = GLint(g.blendFunc[3]); break;
    case GL_BLEND_EQUATION_RGB: *v = GLint(g.blendEq[0]); break;
    case GL_BLEND_EQUATION_ALPHA: *v = GLint(g.blendEq[1]); break;
    case GL_DEPTH_FUNC: *v = GLint(g.depthFunc); break;
    default: *v = getCap(pname) ? 1 : 0; break;
    }
}

void APIENTRY glEnable_(GLenum cap) { setCap(cap, true); }
void APIENTRY glDisable_(GLenum cap) { setCap(cap, false); }
void APIENTRY glViewport_(GLint x, GLint y, GLsizei w, GLsizei h) {
    g.viewport[0] = x; g.viewport[1] = y; g.viewport[2] = w; g.viewport[3] = h;
    g.dirty |= D_VIEWPORT;
}
void APIENTRY glScissor_(GLint x, GLint y, GLsizei w, GLsizei h) {
    g.scissor[0] = x; g.scissor[1] = y; g.scissor[2] = w; g.scissor[3] = h;
    g.dirty |= D_SCISSOR;
}
void APIENTRY glBlendFuncSeparate_(GLenum sr, GLenum dr, GLenum sa, GLenum da) {
    g.blendFunc[0] = sr; g.blendFunc[1] = dr; g.blendFunc[2] = sa; g.blendFunc[3] = da;
    g.dirty |= D_BLEND;
}
void APIENTRY glBlendEquationSeparate_(GLenum rgb, GLenum a) {
    g.blendEq[0] = rgb; g.blendEq[1] = a;
    g.dirty |= D_BLEND;
}
void APIENTRY glBlendColor_(GLfloat r, GLfloat gg, GLfloat b, GLfloat a) {
    g.blendColor[0] = r; g.blendColor[1] = gg; g.blendColor[2] = b; g.blendColor[3] = a;
    g.dirty |= D_BLENDCONST;
}
void APIENTRY glLogicOp_(GLenum op) { g.logicOpMode = op; g.dirty |= D_COLOR; }
void APIENTRY glDepthFunc_(GLenum f) { g.depthFunc = f; g.dirty |= D_DEPTH; }
void APIENTRY glDepthMask_(GLboolean m) { g.depthMask = m; g.dirty |= D_DEPTH; }
void APIENTRY glColorMask_(GLboolean r, GLboolean gg, GLboolean b, GLboolean a) {
    g.colorMask[0] = r; g.colorMask[1] = gg; g.colorMask[2] = b; g.colorMask[3] = a;
    g.dirty |= D_COLORWRITE;
}
void APIENTRY glCullFace_(GLenum m) { g.cullMode = m; g.dirty |= D_RASTER; }
void APIENTRY glFrontFace_(GLenum m) { g.frontFace = m; g.dirty |= D_RASTER; }
void APIENTRY glClearColor_(GLfloat r, GLfloat gg, GLfloat b, GLfloat a) {
    g.clearColor[0] = r; g.clearColor[1] = gg; g.clearColor[2] = b; g.clearColor[3] = a;
}
void APIENTRY glClearDepth_(GLdouble d) { g.clearDepth = d; }
void APIENTRY glDepthRange_(GLdouble n, GLdouble f) {
    g.depthRange[0] = n; g.depthRange[1] = f;
    g.dirty |= D_VIEWPORT;
}
void APIENTRY glLineWidth_(GLfloat w) { g.lineWidth = w; g.dirty |= D_LINE; }
void APIENTRY glPointSize_(GLfloat s) { g.pointSize = s; g.dirty |= D_POINT; }

void APIENTRY glClear_(GLbitfield mask) {
    if (!validateTargets()) return;
    barrierBeforeDraw();
    if (g.dirty & D_SCISSOR) {
        // clears keep to the scissor, as GL's do
        DkScissor s;
        if (g.scissorTest) {
            const int x = std::max(0, g.scissor[0]), y = std::max(0, g.scissor[1]);
            s = {uint32_t(x), uint32_t(y), uint32_t(std::max(0, g.scissor[0] + g.scissor[2] - x)),
                 uint32_t(std::max(0, g.scissor[1] + g.scissor[3] - y))};
        } else {
            s = {0, 0, 16384, 16384};
        }
        dkCmdBufSetScissors(g_dev.cmd, 0, &s, 1);
        g.dirty &= ~D_SCISSOR;
    }
    if ((mask & GL_COLOR_BUFFER_BIT) && g.targetColor) {
        const uint32_t m = (g.colorMask[0] ? DkColorMask_R : 0) | (g.colorMask[1] ? DkColorMask_G : 0) |
                           (g.colorMask[2] ? DkColorMask_B : 0) | (g.colorMask[3] ? DkColorMask_A : 0);
        dkCmdBufClearColorFloat(g_dev.cmd, 0, m, g.clearColor[0], g.clearColor[1], g.clearColor[2], g.clearColor[3]);
    }
    if ((mask & GL_DEPTH_BUFFER_BIT) && g.targetDepth && g.depthMask)
        dkCmdBufClearDepthStencil(g_dev.cmd, true, float(g.clearDepth), 0, 0);
    g.rendered = true;
}

void APIENTRY glFinish_(void) { submit(true); }

// buffers
void APIENTRY glGenBuffers_(GLsizei n, GLuint* out) {
    for (GLsizei i = 0; i < n; i++) out[i] = g.buffers.gen();
}
void APIENTRY glDeleteBuffers_(GLsizei n, const GLuint* names) {
    for (GLsizei i = 0; i < n; i++) {
        Buffer* b = g.buffers.get(names[i]);
        if (!b) continue;
        retire(b->mem);
        g.buffers.del(names[i]);
        if (g.arrayBuffer == names[i]) g.arrayBuffer = 0;
        if (g.packBuffer == names[i]) g.packBuffer = 0;
        if (g.unpackBuffer == names[i]) g.unpackBuffer = 0;
        if (g.uniformBuffer == names[i]) g.uniformBuffer = 0;
    }
}
void APIENTRY glBindBuffer_(GLenum target, GLuint b) {
    switch (target) {
    case GL_ARRAY_BUFFER: g.arrayBuffer = b; break;
    case GL_ELEMENT_ARRAY_BUFFER: {
        VertexArray* va = currentVao();
        if (va->elementBuffer != b) {
            va->elementBuffer = b;
            va->version++;
        }
        break;
    }
    case GL_PIXEL_PACK_BUFFER: g.packBuffer = b; break;
    case GL_PIXEL_UNPACK_BUFFER: g.unpackBuffer = b; break;
    case GL_UNIFORM_BUFFER: g.uniformBuffer = b; break;
    default: break;
    }
}
void APIENTRY glBufferData_(GLenum target, GLsizeiptr size, const void* data, GLenum usage) {
    if (Buffer* b = boundBuffer(target)) bufferStorage(b, size, data, usage);
}
void APIENTRY glBufferStorage_(GLenum target, GLsizeiptr size, const void* data, GLbitfield flags) {
    if (Buffer* b = boundBuffer(target)) {
        bufferStorage(b, size, data, (flags & GL_MAP_READ_BIT) ? GL_STREAM_READ : GL_STREAM_DRAW);
        b->immutable = true;
    }
}
void APIENTRY glBufferSubData_(GLenum target, GLintptr offset, GLsizeiptr size, const void* data) {
    Buffer* b = boundBuffer(target);
    if (!b || !b->mem || !data || offset + size > GLintptr(b->size)) return;
    // in the command stream, after the draws already recorded (GL's order)
    const uint8_t* src = static_cast<const uint8_t*>(data);
    for (GLsizeiptr done = 0; done < size;) {
        const uint32_t n = uint32_t(std::min<GLsizeiptr>(size - done, 0x8000));
        Alloc staging = transient(n, 4);
        memcpy(staging.cpu, src + done, n);
        dkCmdBufCopyBuffer(g_dev.cmd, staging.gpu, b->mem.gpu + uint64_t(offset + done), n);
        done += n;
    }
    g.transferred = true;
}
void APIENTRY glBindBufferBase_(GLenum target, GLuint index, GLuint buffer) {
    if (target == GL_UNIFORM_BUFFER && index < 16) {
        g.ubo[index] = {buffer, 0, 0};
        g.uniformBuffer = buffer;
    }
}
void APIENTRY glBindBufferRange_(GLenum target, GLuint index, GLuint buffer, GLintptr offset, GLsizeiptr size) {
    if (target == GL_UNIFORM_BUFFER && index < 16) {
        g.ubo[index] = {buffer, offset, size};
        g.uniformBuffer = buffer;
    }
}
void* APIENTRY glMapBufferRange_(GLenum target, GLintptr offset, GLsizeiptr length, GLbitfield access) {
    Buffer* b = boundBuffer(target);
    if (!b || !b->mem) return nullptr;
    if (access & GL_MAP_READ_BIT) {
        // GL: the commands writing it have finished
        waitSerial(g_dev.serial);
        if (b->mem.heap == HEAP_READBACK) dkMemBlockFlushCpuCache(b->mem.block, b->mem.offset, b->mem.size);
        return b->mem.cpu + offset;
    }
    if (access & (GL_MAP_UNSYNCHRONIZED_BIT | GL_MAP_PERSISTENT_BIT)) return b->mem.cpu + offset;
    if ((access & GL_MAP_INVALIDATE_BUFFER_BIT) && !b->immutable) {
        bufferStorage(b, b->size, nullptr, b->usage);
        return b->mem.cpu + offset;
    }
    // a synchronized write: to a copy, put in place at the unmap (in order)
    b->mapShadow.resize(size_t(length));
    b->mapOffset = uint32_t(offset);
    b->mapLength = uint32_t(length);
    b->mapAccess = access;
    b->mapPtr = b->mapShadow.data();
    return b->mapPtr;
}
GLboolean APIENTRY glUnmapBuffer_(GLenum target) {
    Buffer* b = boundBuffer(target);
    if (!b) return GL_FALSE;
    if (b->mapPtr) {
        GLuint saved = 0;
        (void)saved;
        const uint8_t* src = b->mapShadow.data();
        for (uint32_t done = 0; done < b->mapLength;) {
            const uint32_t n = std::min<uint32_t>(b->mapLength - done, 0x8000);
            Alloc staging = transient(n, 4);
            memcpy(staging.cpu, src + done, n);
            dkCmdBufCopyBuffer(g_dev.cmd, staging.gpu, b->mem.gpu + b->mapOffset + done, n);
            done += n;
        }
        g.transferred = true;
        b->mapPtr = nullptr;
        std::vector<uint8_t>().swap(b->mapShadow);
    }
    return GL_TRUE;
}

// vertex arrays
void APIENTRY glGenVertexArrays_(GLsizei n, GLuint* out) {
    for (GLsizei i = 0; i < n; i++) out[i] = g.vertexArrays.gen();
}
void APIENTRY glDeleteVertexArrays_(GLsizei n, const GLuint* names) {
    for (GLsizei i = 0; i < n; i++) {
        if (g.vao == names[i]) g.vao = 0;
        if (g.boundVao == g.vertexArrays.get(names[i])) g.boundVao = nullptr;
        g.vertexArrays.del(names[i]);
    }
}
void APIENTRY glBindVertexArray_(GLuint v) { g.vao = v; }
void attribPointer(GLuint i, GLint size, GLenum type, bool normalized, bool integer, GLsizei stride, const void* p) {
    if (i >= 16) return;
    VertexArray* va = currentVao();
    VertexAttrib& a = va->attribs[i];
    a.buffer = g.arrayBuffer;
    a.size = size;
    a.type = type;
    a.normalized = normalized;
    a.integer = integer;
    a.stride = stride;
    a.offset = reinterpret_cast<uintptr_t>(p);
    va->version++;
}
void APIENTRY glVertexAttribPointer_(GLuint i, GLint size, GLenum type, GLboolean norm, GLsizei stride, const void* p) {
    attribPointer(i, size, type, norm, false, stride, p);
}
void APIENTRY glVertexAttribIPointer_(GLuint i, GLint size, GLenum type, GLsizei stride, const void* p) {
    attribPointer(i, size, type, false, true, stride, p);
}
void APIENTRY glEnableVertexAttribArray_(GLuint i) {
    if (i < 16 && !currentVao()->attribs[i].enabled) {
        currentVao()->attribs[i].enabled = true;
        currentVao()->version++;
    }
}
void APIENTRY glDisableVertexAttribArray_(GLuint i) {
    if (i < 16 && currentVao()->attribs[i].enabled) {
        currentVao()->attribs[i].enabled = false;
        currentVao()->version++;
    }
}
void APIENTRY glVertexAttrib4f_(GLuint i, GLfloat x, GLfloat y, GLfloat z, GLfloat w) {
    if (i >= 16) return;
    const float v[4] = {x, y, z, w};
    memcpy(g.current[i].v, v, 16);
    g.current[i].integer = false;
    g.currentVersion++;
}
void APIENTRY glVertexAttribI4ui_(GLuint i, GLuint x, GLuint y, GLuint z, GLuint w) {
    if (i >= 16) return;
    State::CurrentAttrib& c = g.current[i];
    if (c.integer && c.v[0] == x && c.v[1] == y && c.v[2] == z && c.v[3] == w) return;
    c.v[0] = x; c.v[1] = y; c.v[2] = z; c.v[3] = w;
    c.integer = true;
    g.currentVersion++;
}

// draws
void APIENTRY glDrawElements_(GLenum mode, GLsizei count, GLenum type, const void* indices) {
    drawElements(mode, count, type, indices, 0);
}
void APIENTRY glDrawElementsBaseVertex_(GLenum mode, GLsizei count, GLenum type, const void* indices, GLint base) {
    drawElements(mode, count, type, indices, base);
}
void APIENTRY glMultiDrawElementsBaseVertex_(GLenum mode, const GLsizei* count, GLenum type, const void* const* indices,
                                             GLsizei n, const GLint* base) {
    for (GLsizei i = 0; i < n; i++) drawElements(mode, count[i], type, indices[i], base[i]);
}
void APIENTRY glDrawArrays_(GLenum mode, GLint first, GLsizei count) {
    if (count <= 0 || !validateDraw()) return;
    dkCmdBufDraw(g_dev.cmd, primitiveOf(mode), uint32_t(count), 1, uint32_t(first), 0);
    traceDraw("arrays", mode, count, uint32_t(first), 0, 0);
    drawn();
}

// shaders and programs
GLuint APIENTRY glCreateShader_(GLenum type) {
    const GLuint n = g.shaders.gen();
    g.shaders.get(n)->type = type;
    return n;
}
void APIENTRY glShaderSource_(GLuint s, GLsizei count, const GLchar* const* str, const GLint* len) {
    Shader* sh = g.shaders.get(s);
    if (!sh) return;
    sh->source.clear();
    for (GLsizei i = 0; i < count; i++)
        sh->source += len && len[i] >= 0 ? std::string(str[i], size_t(len[i])) : std::string(str[i]);
}
void APIENTRY glCompileShader_(GLuint) {}  // compiled at link, with the other stages
void APIENTRY glGetShaderiv_(GLuint, GLenum pname, GLint* v) {
    *v = pname == GL_COMPILE_STATUS ? GL_TRUE : 0;
}
void APIENTRY glGetShaderInfoLog_(GLuint, GLsizei max, GLsizei* len, GLchar* log) {
    if (len) *len = 0;
    if (max > 0 && log) log[0] = 0;
}
void APIENTRY glDeleteShader_(GLuint s) { g.shaders.del(s); }
GLuint APIENTRY glCreateProgram_(void) { return g.programs.gen(); }
void APIENTRY glAttachShader_(GLuint p, GLuint s) {
    if (Program* pr = g.programs.get(p)) pr->shaders.push_back(s);
}
void APIENTRY glBindAttribLocation_(GLuint p, GLuint i, const GLchar* name) {
    if (Program* pr = g.programs.get(p)) pr->attribBindings[name] = int(i);
}
void APIENTRY glBindFragDataLocation_(GLuint p, GLuint c, const GLchar* name) {
    if (Program* pr = g.programs.get(p)) pr->fragBindings[name] = int(c);
}
void APIENTRY glLinkProgram_(GLuint p) {
    if (Program* pr = g.programs.get(p)) linkProgram(pr);
}
void APIENTRY glGetProgramiv_(GLuint p, GLenum pname, GLint* v) {
    const Program* pr = g.programs.get(p);
    switch (pname) {
    case GL_LINK_STATUS: *v = pr && pr->linked; break;
    case GL_INFO_LOG_LENGTH: *v = pr ? GLint(pr->infoLog.size() + 1) : 0; break;
    case GL_PROGRAM_BINARY_LENGTH: *v = 0; break;
    default: *v = 0; break;
    }
}
void APIENTRY glGetProgramInfoLog_(GLuint p, GLsizei max, GLsizei* len, GLchar* log) {
    const Program* pr = g.programs.get(p);
    const std::string s = pr ? pr->infoLog : "";
    const GLsizei n = max > 0 ? std::min<GLsizei>(GLsizei(s.size()), max - 1) : 0;
    if (max > 0 && log) {
        memcpy(log, s.data(), size_t(n));
        log[n] = 0;
    }
    if (len) *len = n;
}
void APIENTRY glUseProgram_(GLuint p) { g.program = p; }
void APIENTRY glDeleteProgram_(GLuint p) {
    if (Program* pr = g.programs.get(p)) {
        destroyProgram(pr);
        if (g.program == p) g.program = 0;
        g.programs.del(p);
    }
}

// uniforms: a location is (uniform index << 8) | array element
GLint APIENTRY glGetUniformLocation_(GLuint p, const GLchar* name) {
    const Program* pr = g.programs.get(p);
    if (!pr || !pr->linked || !name) return -1;
    std::string base = name;
    int element = 0;
    const size_t br = base.find('[');
    if (br != std::string::npos) {
        element = atoi(base.c_str() + br + 1);
        base.resize(br);
    }
    for (size_t i = 0; i < pr->glsl.uniforms.size(); i++)
        if (pr->glsl.uniforms[i].name == base && element < pr->glsl.uniforms[i].arraySize)
            return GLint((i << 8) | size_t(element));
    return -1;
}
GLuint APIENTRY glGetUniformBlockIndex_(GLuint p, const GLchar* name) {
    const Program* pr = g.programs.get(p);
    if (!pr) return GL_INVALID_INDEX;
    for (size_t i = 0; i < pr->glsl.blocks.size(); i++)
        if (pr->glsl.blocks[i] == name) return GLuint(i);
    return GL_INVALID_INDEX;
}
void APIENTRY glUniformBlockBinding_(GLuint p, GLuint index, GLuint binding) {
    Program* pr = g.programs.get(p);
    if (pr && index < pr->blockBindings.size()) {
        pr->blockBindings[index] = int(binding);
        memset(g.boundUbo, 0, sizeof g.boundUbo);
    }
}

// Writes `count` elements of `comps` 32-bit components at a location.
void setUniform(GLint loc, GLsizei count, int comps, const void* data) {
    Program* p = currentProgram();
    if (!p || loc < 0 || count <= 0) return;
    const size_t index = size_t(loc) >> 8;
    int element = loc & 0xFF;
    if (index >= p->glsl.uniforms.size()) return;
    const GlslUniform& u = p->glsl.uniforms[index];
    const uint8_t* src = static_cast<const uint8_t*>(data);
    if (u.binding >= 0) {
        // a sampler: its texture unit(s)
        for (GLsizei i = 0; i < count && element < u.arraySize; i++, element++) {
            int unit;
            memcpy(&unit, src + size_t(i) * 4, 4);
            if (p->samplerUnits[size_t(u.binding + element)] != unit) {
                p->samplerUnits[size_t(u.binding + element)] = unit;
                p->version++;  // binds its textures again
            }
        }
        return;
    }
    const uint32_t bytes = uint32_t(comps) * 4;
    for (GLsizei i = 0; i < count && element < u.arraySize; i++, element++) {
        const uint32_t at = u.offset + uint32_t(element) * (u.arraySize > 1 ? u.stride : 0);
        if (at + bytes > p->defaultBlock.size()) break;
        if (memcmp(&p->defaultBlock[at], src + size_t(i) * bytes, bytes) != 0) {
            memcpy(&p->defaultBlock[at], src + size_t(i) * bytes, bytes);
            p->defaultDirty = true;
        }
    }
}
void APIENTRY glUniform1i_(GLint l, GLint v) { setUniform(l, 1, 1, &v); }
void APIENTRY glUniform1iv_(GLint l, GLsizei n, const GLint* v) { setUniform(l, n, 1, v); }
void APIENTRY glUniform2iv_(GLint l, GLsizei n, const GLint* v) { setUniform(l, n, 2, v); }
void APIENTRY glUniform4iv_(GLint l, GLsizei n, const GLint* v) { setUniform(l, n, 4, v); }
void APIENTRY glUniform1f_(GLint l, GLfloat v) { setUniform(l, 1, 1, &v); }
void APIENTRY glUniform2f_(GLint l, GLfloat x, GLfloat y) {
    const GLfloat v[2] = {x, y};
    setUniform(l, 1, 2, v);
}
void APIENTRY glUniform1fv_(GLint l, GLsizei n, const GLfloat* v) { setUniform(l, n, 1, v); }
void APIENTRY glUniform2fv_(GLint l, GLsizei n, const GLfloat* v) { setUniform(l, n, 2, v); }
void APIENTRY glUniform4fv_(GLint l, GLsizei n, const GLfloat* v) { setUniform(l, n, 4, v); }

void APIENTRY glGetProgramBinary_(GLuint, GLsizei, GLsizei* len, GLenum*, void*) {
    if (len) *len = 0;
}
void APIENTRY glProgramBinary_(GLuint p, GLenum, const void*, GLsizei) {
    if (Program* pr = g.programs.get(p)) pr->linked = false;  // no binaries: the caller compiles instead
}
void APIENTRY glProgramParameteri_(GLuint, GLenum, GLint) {}
void APIENTRY glMaxShaderCompilerThreadsKHR_(GLuint) {}

// textures
void APIENTRY glGenTextures_(GLsizei n, GLuint* out) {
    for (GLsizei i = 0; i < n; i++) out[i] = g.textures.gen();
}
void APIENTRY glDeleteTextures_(GLsizei n, const GLuint* names) {
    for (GLsizei i = 0; i < n; i++) {
        Texture* t = g.textures.get(names[i]);
        if (!t) continue;
        destroyTexture(t);
        for (GLuint& u : g.tex2D)
            if (u == names[i]) u = 0;
        g.textures.del(names[i]);
    }
    g.boundTexCount[0] = g.boundTexCount[1] = 0;
}
void APIENTRY glBindTexture_(GLenum target, GLuint t) {
    if (target == GL_TEXTURE_2D) g.tex2D[g.activeUnit] = t;
}
void APIENTRY glActiveTexture_(GLenum unit) { g.activeUnit = int(unit - GL_TEXTURE0) & 31; }

uint32_t unpackRow(GLsizei w) { return g.unpackRowLength ? uint32_t(g.unpackRowLength) : uint32_t(w); }

void APIENTRY glTexImage2D_(GLenum, GLint level, GLint ifmt, GLsizei w, GLsizei h, GLint, GLenum format, GLenum type,
                            const void* pixels) {
    Texture* t = boundTexture2D();
    if (!t) return;
    GLenum internal = GLenum(ifmt);
    if (internal == GL_RGBA && type == GL_FLOAT) internal = GL_RGBA32F;
    defineLevel(t, level, internal, uint32_t(w), uint32_t(h));
    if (pixels) uploadLevel(t, level, 0, 0, uint32_t(w), uint32_t(h), pixels, 0, unpackRow(w), false);
    (void)format;
}
void APIENTRY glTexSubImage2D_(GLenum, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum, GLenum,
                               const void* pixels) {
    if (Texture* t = boundTexture2D()) uploadLevel(t, level, x, y, uint32_t(w), uint32_t(h), pixels, 0, unpackRow(w), false);
}
void APIENTRY glCompressedTexImage2D_(GLenum, GLint level, GLenum ifmt, GLsizei w, GLsizei h, GLint, GLsizei bytes,
                                      const void* data) {
    Texture* t = boundTexture2D();
    if (!t) return;
    defineLevel(t, level, ifmt, uint32_t(w), uint32_t(h));
    if (data) uploadLevel(t, level, 0, 0, uint32_t(w), uint32_t(h), data, uint32_t(bytes), 0, true);
}
void APIENTRY glCompressedTexSubImage2D_(GLenum, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum,
                                         GLsizei bytes, const void* data) {
    if (Texture* t = boundTexture2D()) uploadLevel(t, level, x, y, uint32_t(w), uint32_t(h), data, uint32_t(bytes), 0, true);
}

void texParam(Texture* t, SamplerParams* s, GLenum pname, float f, GLint i) {
    switch (pname) {
    case GL_TEXTURE_MIN_FILTER: s->minFilter = GLenum(i); break;
    case GL_TEXTURE_MAG_FILTER: s->magFilter = GLenum(i); break;
    case GL_TEXTURE_WRAP_S: s->wrapS = GLenum(i); break;
    case GL_TEXTURE_WRAP_T: s->wrapT = GLenum(i); break;
    case GL_TEXTURE_WRAP_R: s->wrapR = GLenum(i); break;
    case GL_TEXTURE_MIN_LOD: s->minLod = f; break;
    case GL_TEXTURE_MAX_LOD: s->maxLod = f; break;
    case GL_TEXTURE_LOD_BIAS: s->lodBias = f; break;
    case GL_TEXTURE_MAX_ANISOTROPY: s->maxAniso = f; break;
    case GL_TEXTURE_BASE_LEVEL: if (t) { t->baseLevel = i; t->descDirty = true; } break;
    case GL_TEXTURE_MAX_LEVEL: if (t) { t->maxLevel = i; t->descDirty = true; } break;
    case GL_TEXTURE_SWIZZLE_R: if (t) { t->swizzle[0] = i; t->descDirty = true; } break;
    case GL_TEXTURE_SWIZZLE_G: if (t) { t->swizzle[1] = i; t->descDirty = true; } break;
    case GL_TEXTURE_SWIZZLE_B: if (t) { t->swizzle[2] = i; t->descDirty = true; } break;
    case GL_TEXTURE_SWIZZLE_A: if (t) { t->swizzle[3] = i; t->descDirty = true; } break;
    default: break;
    }
}
void APIENTRY glTexParameteri_(GLenum, GLenum pname, GLint v) {
    Texture* t = boundTexture2D();
    if (t) texParam(t, &t->sampler, pname, float(v), v);
}
void APIENTRY glTexParameterf_(GLenum, GLenum pname, GLfloat v) {
    Texture* t = boundTexture2D();
    if (t) texParam(t, &t->sampler, pname, v, GLint(v));
}
void APIENTRY glGenerateMipmap_(GLenum) {
    Texture* t = boundTexture2D();
    if (!t || t->levels.empty() || !t->levels[0].defined || t->compressed) return;
    // define the whole chain, then make each level from the one above with the 2D engine
    uint32_t w = t->levels[0].w, h = t->levels[0].h;
    int levels = 1;
    while (w > 1 || h > 1) {
        w = std::max(1u, w >> 1);
        h = std::max(1u, h >> 1);
        defineLevel(t, levels++, t->internalFormat, w, h);
    }
    if (!realize(t)) return;
    barrierBeforeTransfer();
    for (uint32_t l = 1; l < t->imageLevels; l++) {
        DkImageView sv = viewOf(t, int(l - 1)), dv = viewOf(t, int(l));
        DkImageRect sr = {0, 0, 0, std::max(1u, t->levels[0].w >> (l - 1)), std::max(1u, t->levels[0].h >> (l - 1)), 1};
        DkImageRect dr = {0, 0, 0, std::max(1u, t->levels[0].w >> l), std::max(1u, t->levels[0].h >> l), 1};
        dkCmdBufBlitImage(g_dev.cmd, &sv, &sr, &dv, &dr, DkBlitFlag_FilterLinear, 0);
    }
    g.transferred = true;
}

// samplers
void APIENTRY glGenSamplers_(GLsizei n, GLuint* out) {
    for (GLsizei i = 0; i < n; i++) out[i] = g.samplers.gen();
}
void APIENTRY glDeleteSamplers_(GLsizei n, const GLuint* names) {
    for (GLsizei i = 0; i < n; i++) {
        for (GLuint& u : g.samplerObj)
            if (u == names[i]) u = 0;
        g.samplers.del(names[i]);
    }
}
void APIENTRY glBindSampler_(GLuint unit, GLuint s) {
    if (unit < 32) g.samplerObj[unit] = s;
}
void APIENTRY glSamplerParameteri_(GLuint s, GLenum pname, GLint v) {
    if (SamplerParams* p = g.samplers.get(s)) texParam(nullptr, p, pname, float(v), v);
}
void APIENTRY glSamplerParameterf_(GLuint s, GLenum pname, GLfloat v) {
    if (SamplerParams* p = g.samplers.get(s)) texParam(nullptr, p, pname, v, GLint(v));
}

// framebuffers and renderbuffers
void APIENTRY glGenFramebuffers_(GLsizei n, GLuint* out) {
    for (GLsizei i = 0; i < n; i++) out[i] = g.framebuffers.gen();
}
void APIENTRY glDeleteFramebuffers_(GLsizei n, const GLuint* names) {
    for (GLsizei i = 0; i < n; i++) {
        if (g.drawFbo == names[i]) g.drawFbo = 0;
        if (g.readFbo == names[i]) g.readFbo = 0;
        g.framebuffers.del(names[i]);
    }
    g.dirty |= D_TARGETS;
}
void APIENTRY glBindFramebuffer_(GLenum target, GLuint f) {
    if (target == GL_FRAMEBUFFER || target == GL_DRAW_FRAMEBUFFER) {
        if (g.drawFbo != f) g.dirty |= D_TARGETS;
        g.drawFbo = f;
    }
    if (target == GL_FRAMEBUFFER || target == GL_READ_FRAMEBUFFER) g.readFbo = f;
}
Framebuffer* fboFor(GLenum target) {
    return g.framebuffers.get(target == GL_READ_FRAMEBUFFER ? g.readFbo : g.drawFbo);
}
void attach(GLenum target, GLenum attachment, GLuint name, bool renderbuffer, int level) {
    Framebuffer* f = fboFor(target);
    if (!f) return;
    Attachment a = {name, renderbuffer, level};
    if (attachment == GL_DEPTH_ATTACHMENT || attachment == GL_DEPTH_STENCIL_ATTACHMENT) f->depth = a;
    else if (attachment == GL_COLOR_ATTACHMENT0) f->color = a;
    g.dirty |= D_TARGETS;
}
void APIENTRY glFramebufferTexture2D_(GLenum target, GLenum attachment, GLenum, GLuint tex, GLint level) {
    attach(target, attachment, tex, false, level);
}
void APIENTRY glFramebufferRenderbuffer_(GLenum target, GLenum attachment, GLenum, GLuint rb) {
    attach(target, attachment, rb, true, 0);
}
GLenum APIENTRY glCheckFramebufferStatus_(GLenum target) {
    if ((target == GL_READ_FRAMEBUFFER ? g.readFbo : g.drawFbo) == 0) return GL_FRAMEBUFFER_COMPLETE;
    const Framebuffer* f = fboFor(target);
    if (!f) return GL_FRAMEBUFFER_UNDEFINED;
    Texture* c = attachmentTexture(f->color);
    Texture* d = attachmentTexture(f->depth);
    if (!c && !d) return GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT;
    if ((c && !realize(c)) || (d && !realize(d))) return GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT;
    return GL_FRAMEBUFFER_COMPLETE;
}
void APIENTRY glGenRenderbuffers_(GLsizei n, GLuint* out) {
    // renderbuffers are textures here (one name space with them)
    for (GLsizei i = 0; i < n; i++) {
        out[i] = g.textures.gen();
        g.textures.get(out[i])->isRenderbuffer = true;
    }
}
void APIENTRY glBindRenderbuffer_(GLenum, GLuint rb) { g.renderbuffer = rb; }
void APIENTRY glRenderbufferStorageMultisample_(GLenum, GLsizei samples, GLenum ifmt, GLsizei w, GLsizei h) {
    Texture* t = g.textures.get(g.renderbuffer);
    if (!t) return;
    t->samples = uint32_t(std::max(1, samples));
    defineLevel(t, 0, ifmt, uint32_t(w), uint32_t(h));
}
void APIENTRY glRenderbufferStorage_(GLenum target, GLenum ifmt, GLsizei w, GLsizei h) {
    glRenderbufferStorageMultisample_(target, 0, ifmt, w, h);
}

void APIENTRY glBlitFramebuffer_(GLint sx0, GLint sy0, GLint sx1, GLint sy1, GLint dx0, GLint dy0, GLint dx1, GLint dy1,
                                 GLbitfield mask, GLenum filter) {
    const Framebuffer* rf = g.framebuffers.get(g.readFbo);
    const Framebuffer* df = g.framebuffers.get(g.drawFbo);
    for (int pass = 0; pass < 2; pass++) {
        const bool depth = pass == 1;
        if (!(mask & (depth ? GL_DEPTH_BUFFER_BIT : GL_COLOR_BUFFER_BIT))) continue;
        Texture* src = g.readFbo ? (rf ? attachmentTexture(depth ? rf->depth : rf->color) : nullptr) : defaultFramebuffer();
        Texture* dst = g.drawFbo ? (df ? attachmentTexture(depth ? df->depth : df->color) : nullptr) : defaultFramebuffer();
        if (!src || !dst || !realize(src) || !realize(dst)) continue;
        const int sl = g.readFbo && rf ? (depth ? rf->depth.level : rf->color.level) : 0;
        const int dl = g.drawFbo && df ? (depth ? df->depth.level : df->color.level) : 0;
        barrierBeforeTransfer();
        DkImageView sv = viewOf(src, sl), dv = viewOf(dst, dl);
        uint32_t flags = filter == GL_LINEAR ? DkBlitFlag_FilterLinear : DkBlitFlag_FilterNearest;
        if ((sx1 < sx0) != (dx1 < dx0)) flags |= DkBlitFlag_FlipX;
        if ((sy1 < sy0) != (dy1 < dy0)) flags |= DkBlitFlag_FlipY;
        DkImageRect sr = {uint32_t(std::min(sx0, sx1)), uint32_t(std::min(sy0, sy1)), 0, uint32_t(abs(sx1 - sx0)),
                          uint32_t(abs(sy1 - sy0)), 1};
        DkImageRect dr = {uint32_t(std::min(dx0, dx1)), uint32_t(std::min(dy0, dy1)), 0, uint32_t(abs(dx1 - dx0)),
                          uint32_t(abs(dy1 - dy0)), 1};
        if (!sr.width || !sr.height || !dr.width || !dr.height) continue;
        if (src->samples > 1 && dst->samples == 1 && sr.width == dr.width && sr.height == dr.height) {
            dkCmdBufResolveImage(g_dev.cmd, &sv, &dv);
        } else if (depth || (sr.width == dr.width && sr.height == dr.height && !(flags & (DkBlitFlag_FlipX | DkBlitFlag_FlipY)))) {
            dkCmdBufCopyImage(g_dev.cmd, &sv, &sr, &dv, &dr, flags & (DkBlitFlag_FlipX | DkBlitFlag_FlipY));
        } else {
            dkCmdBufBlitImage(g_dev.cmd, &sv, &sr, &dv, &dr, flags, 0);
        }
        g.transferred = true;
    }
}

void APIENTRY glReadPixels_(GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, void* pixels) {
    readPixels(x, y, w, h, format, type, pixels);
}
void APIENTRY glPixelStorei_(GLenum pname, GLint v) {
    switch (pname) {
    case GL_PACK_ALIGNMENT: g.packAlign = v; break;
    case GL_PACK_ROW_LENGTH: g.packRowLength = v; break;
    case GL_UNPACK_ALIGNMENT: g.unpackAlign = v; break;
    case GL_UNPACK_ROW_LENGTH: g.unpackRowLength = v; break;
    default: break;
    }
}

// queries: two sample counter reports, the result their difference
void APIENTRY glGenQueries_(GLsizei n, GLuint* out) {
    for (GLsizei i = 0; i < n; i++) out[i] = g.queries.gen();
}
void APIENTRY glDeleteQueries_(GLsizei n, const GLuint* names) {
    for (GLsizei i = 0; i < n; i++) {
        if (Query* q = g.queries.get(names[i])) retire(q->mem);
        g.queries.del(names[i]);
    }
}
GLuint s_activeQuery = 0;
void APIENTRY glBeginQuery_(GLenum target, GLuint id) {
    Query* q = g.queries.get(id);
    if (!q) return;
    if (!q->mem) q->mem = g_dev.heaps[HEAP_READBACK].alloc(32, 32);
    q->target = target;
    dkCmdBufReportCounter(g_dev.cmd, DkCounter_SamplesPassed, q->mem.gpu);
    s_activeQuery = id;
}
void APIENTRY glEndQuery_(GLenum) {
    Query* q = g.queries.get(s_activeQuery);
    if (!q) return;
    dkCmdBufReportCounter(g_dev.cmd, DkCounter_SamplesPassed, q->mem.gpu + 16);
    q->serial = g_dev.serial;
    s_activeQuery = 0;
}
void APIENTRY glGetQueryObjectuiv_(GLuint id, GLenum pname, GLuint* v) {
    Query* q = g.queries.get(id);
    if (!q || !q->mem) {
        *v = 0;
        return;
    }
    poll();
    if (pname == GL_QUERY_RESULT_AVAILABLE) {
        *v = g_dev.completed >= q->serial;
        return;
    }
    waitSerial(q->serial);
    dkMemBlockFlushCpuCache(q->mem.block, q->mem.offset, q->mem.size);
    uint64_t a, b;
    memcpy(&a, q->mem.cpu, 8);
    memcpy(&b, q->mem.cpu + 16, 8);
    const uint64_t n = b - a;
    *v = q->target == GL_SAMPLES_PASSED ? GLuint(std::min<uint64_t>(n, 0xFFFFFFFFu)) : GLuint(n != 0);
}

// fences: a sync is the submission it was made in
struct SyncObj {
    uint64_t serial;
};
GLsync APIENTRY glFenceSync_(GLenum, GLbitfield) {
    SyncObj* s = new SyncObj{g_dev.serial};
    return reinterpret_cast<GLsync>(s);
}
GLenum APIENTRY glClientWaitSync_(GLsync sync, GLbitfield flags, GLuint64 timeout) {
    SyncObj* s = reinterpret_cast<SyncObj*>(sync);
    if (!s) return GL_WAIT_FAILED;
    poll();
    if (g_dev.completed >= s->serial) return GL_ALREADY_SIGNALED;
    if (s->serial >= g_dev.serial && (flags & GL_SYNC_FLUSH_COMMANDS_BIT)) submit();
    if (!timeout) return GL_TIMEOUT_EXPIRED;
    waitSerial(s->serial);
    return GL_CONDITION_SATISFIED;
}
void APIENTRY glDeleteSync_(GLsync sync) { delete reinterpret_cast<SyncObj*>(sync); }

struct Entry {
    const char* name;
    void* fn;
};
#define E(n) {#n, reinterpret_cast<void*>(&n##_)}
const Entry kEntries[] = {
    E(glGetError), E(glGetString), E(glGetStringi), E(glGetIntegerv), E(glEnable), E(glDisable), E(glViewport),
    E(glScissor), E(glBlendFuncSeparate), E(glBlendEquationSeparate), E(glBlendColor), E(glLogicOp), E(glDepthFunc),
    E(glDepthMask), E(glColorMask), E(glCullFace), E(glFrontFace), E(glClearColor), E(glClearDepth), E(glClear),
    E(glDepthRange), E(glLineWidth), E(glPointSize), E(glFinish), E(glGenBuffers), E(glBindBuffer), E(glBufferData),
    E(glBufferSubData), E(glBindBufferBase), E(glBindBufferRange), E(glBufferStorage), E(glMapBufferRange),
    E(glUnmapBuffer), E(glDeleteBuffers), E(glGenVertexArrays), E(glBindVertexArray), E(glDeleteVertexArrays),
    E(glVertexAttribPointer), E(glVertexAttribIPointer), E(glEnableVertexAttribArray), E(glDisableVertexAttribArray),
    E(glVertexAttrib4f), E(glVertexAttribI4ui), E(glDrawElements), E(glDrawArrays), E(glDrawElementsBaseVertex),
    E(glMultiDrawElementsBaseVertex), E(glCreateShader), E(glShaderSource), E(glCompileShader), E(glGetShaderiv),
    E(glGetShaderInfoLog), E(glDeleteShader), E(glCreateProgram), E(glAttachShader), E(glLinkProgram),
    E(glGetProgramiv), E(glGetProgramInfoLog), E(glUseProgram), E(glDeleteProgram), E(glBindAttribLocation),
    E(glBindFragDataLocation), E(glGetUniformLocation), E(glGetUniformBlockIndex), E(glUniformBlockBinding),
    E(glUniform1i), E(glUniform1iv), E(glUniform2iv), E(glUniform4iv), E(glUniform1f), E(glUniform2f),
    E(glUniform1fv), E(glUniform2fv), E(glUniform4fv), E(glGetProgramBinary), E(glProgramBinary),
    E(glProgramParameteri), E(glMaxShaderCompilerThreadsKHR), E(glGenTextures), E(glDeleteTextures),
    E(glBindTexture), E(glActiveTexture), E(glTexImage2D), E(glTexSubImage2D), E(glCompressedTexImage2D),
    E(glCompressedTexSubImage2D), E(glTexParameteri), E(glTexParameterf), E(glGenerateMipmap), E(glGenSamplers),
    E(glDeleteSamplers), E(glBindSampler), E(glSamplerParameteri), E(glSamplerParameterf), E(glGenFramebuffers),
    E(glDeleteFramebuffers), E(glBindFramebuffer), E(glFramebufferTexture2D), E(glFramebufferRenderbuffer),
    E(glCheckFramebufferStatus), E(glGenRenderbuffers), E(glBindRenderbuffer), E(glRenderbufferStorage),
    E(glRenderbufferStorageMultisample), E(glBlitFramebuffer), E(glReadPixels), E(glPixelStorei), E(glGenQueries),
    E(glDeleteQueries), E(glBeginQuery), E(glEndQuery), E(glGetQueryObjectuiv), E(glFenceSync), E(glClientWaitSync),
    E(glDeleteSync),
};
#undef E

int s_swapInterval = 1;
bool s_ready = false;

}  // namespace

// --- dkgl.h ---------------------------------------------------------------------
bool init(int width, int height) {
    if (s_ready) return true;
    extern bool initDevice(int, int);
    if (!initDevice(width, height)) {
        logf("deko3d could not start");
        return false;
    }
    // the default framebuffer: the swapchain's images, as textures
    for (int i = 0; i < 3; i++) {
        Texture& t = g.swapTex[i];
        t.internalFormat = GL_RGBA8;
        t.format = DkImageFormat_RGBA8_Unorm;
        t.levels.resize(1);
        t.levels[0].w = uint32_t(width);
        t.levels[0].h = uint32_t(height);
        t.levels[0].defined = true;
        t.layout = g_dev.swapLayout;
        t.image = g_dev.swapImages[i];
        t.imageLevels = 1;
        t.realized = true;
    }
    g.viewport[2] = width;
    g.viewport[3] = height;
    g.scissor[2] = width;
    g.scissor[3] = height;
    DkMultisampleState ms;
    dkMultisampleStateDefaults(&ms);
    dkCmdBufBindMultisampleState(g_dev.cmd, &ms);
    dkSwapchainSetSwapInterval(g_dev.swapchain, uint32_t(s_swapInterval));
    s_ready = true;
    startWatchdog();
    return true;
}

bool ready() { return s_ready; }

void shutdown() {
    if (!s_ready) return;
    submit(true);
    extern void shutdownDevice();
    shutdownDevice();
    s_ready = false;
}

void* getProc(const char* name) {
    for (const Entry& e : kEntries)
        if (!strcmp(e.name, name)) return e.fn;
    return nullptr;
}

namespace {
uint64_t s_acquireTicks = 0;
// each image's acquire fence: the command buffer keeps a pointer to it until
// the commands are submitted (at the swap at the latest)
DkFence s_acquireFence[3];
}

Texture* defaultFramebuffer() {
    if (g.frontSlot < 0) {
        // the GPU waits (in the command stream, where the frame first uses
        // it) until the compositor has let go of the image
        const uint64_t t0 = armGetSystemTick();
        int slot = 0;
        DkFence ready;
        {
            Waiting w("acquiring a swapchain image");
            dkSwapchainAcquireImage(g_dev.swapchain, &slot, &ready);
        }
        s_acquireFence[slot] = ready;
        dkCmdBufWaitFence(g_dev.cmd, &s_acquireFence[slot]);
        s_acquireTicks += armGetSystemTick() - t0;
        g.frontSlot = slot;
    }
    return &g.swapTex[g.frontSlot];
}

}  // namespace dkgl
extern "C" bool GXPC_WindowShotSink(uint32_t frame, const uint8_t* rgba, int w, int h);
namespace dkgl {
uint64_t g_swapCount = 0;

// SMS_DKGL_TRACE_PIXEL=x,y[;x,y...] (GL coordinates of the draw target): each
// traced draw logs those pixels' colour, alpha and depth as the draws before
// it left them (a submit and wait each: tests only).
void tracePixels() {
    static std::vector<std::pair<int, int>> pts;
    static bool parsed = false;
    if (!parsed) {
        parsed = true;
        if (const char* e = getenv("SMS_DKGL_TRACE_PIXEL"))
            for (const char* p = e; *p;) {
                char* end;
                const long x = strtol(p, &end, 10);
                if (*end != ',') break;
                const long y = strtol(end + 1, &end, 10);
                pts.push_back({int(x), int(y)});
                p = end;
                while (*p == ';' || *p == ' ') p++;
            }
    }
    // SMS_DKGL_TRACE_DUMP=1: the whole target before each traced draw, to the
    // PC as captures (GXPC_WindowShotSink, tools/switch-dev.py run --shots):
    // colour with alpha as frame 100000 + 3n, alpha alone 100001 + 3n, depth
    // 100002 + 3n (its 24 bits as red, green, blue)
    static const bool dump = getenv("SMS_DKGL_TRACE_DUMP") && atoi(getenv("SMS_DKGL_TRACE_DUMP"));
    static uint32_t dumped = 0, traced = 0;
    static const int every = getenv("SMS_DKGL_TRACE_DUMP_EVERY") ? std::max(1, atoi(getenv("SMS_DKGL_TRACE_DUMP_EVERY"))) : 1;
    if (dump && traced++ % uint32_t(every) == 0) {
        const Framebuffer* f = g.framebuffers.get(g.drawFbo);
        const int w = g.targetW, h = g.targetH;
        if (f && w > 0 && h > 0) {
            const GLuint read = g.readFbo, pack = g.packBuffer;
            g.readFbo = g.drawFbo;
            g.packBuffer = 0;
            std::vector<uint8_t> c(size_t(w) * h * 4), a(size_t(w) * h * 4);
            std::vector<uint32_t> z(size_t(w) * h);
            readPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, c.data());
            readPixels(0, 0, w, h, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, z.data());
            g.readFbo = read;
            g.packBuffer = pack;
            for (size_t i = 0; i < z.size(); i++) {
                a[i * 4] = a[i * 4 + 1] = a[i * 4 + 2] = c[i * 4 + 3];
                a[i * 4 + 3] = 255;
                const uint32_t d = z[i] >> 8;
                uint8_t* o = reinterpret_cast<uint8_t*>(&z[i]);
                o[0] = uint8_t(d >> 16), o[1] = uint8_t(d >> 8), o[2] = uint8_t(d), o[3] = 255;
            }
            GXPC_WindowShotSink(100000 + dumped * 3, c.data(), w, h);
            GXPC_WindowShotSink(100001 + dumped * 3, a.data(), w, h);
            GXPC_WindowShotSink(100002 + dumped * 3, reinterpret_cast<const uint8_t*>(z.data()), w, h);
            logf("  dumped the target as captures %u-%u (before traced draw %u)", 100000 + dumped * 3,
                 100002 + dumped * 3, traced - 1);
            dumped++;
        }
    }
    if (pts.empty()) return;
    const GLuint read = g.readFbo;
    const GLuint pack = g.packBuffer;
    g.readFbo = g.drawFbo;
    g.packBuffer = 0;
    for (const auto& pt : pts) {
        uint8_t c[4] = {0, 0, 0, 0};
        uint32_t z = 0;
        readPixels(pt.first, pt.second, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, c);
        readPixels(pt.first, pt.second, 1, 1, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, &z);
        logf("  pixel %d,%d: %3d %3d %3d alpha %3d depth %06x", pt.first, pt.second, c[0], c[1], c[2], c[3], z >> 8);
    }
    g.readFbo = read;
    g.packBuffer = pack;
}

void swap() {
    g_swapCount++;
    if (!s_ready) return;
    static uint64_t presentTicks = 0, swapTicks = 0, lastSwap = 0;
    static uint32_t swaps = 0;
    // (the compositor shows the image flipped: its row 0, GL's y = 0, at the bottom)
    defaultFramebuffer();  // a frame that never drew into it still shows one
    const int slot = g.frontSlot;
    if (g.rendered) {
        dkCmdBufBarrier(g_dev.cmd, DkBarrier_Fragments, 0);
        g.rendered = false;
    }
    submit();
    const uint64_t t2 = armGetSystemTick();
    {
        Waiting w("presenting");
        dkQueuePresentImage(g_dev.queue, g_dev.swapchain, slot);
    }
    const uint64_t t3 = armGetSystemTick();
    g.frontSlot = -1;
    if (g.targetColor == &g.swapTex[slot]) {
        g.targetColor = nullptr;
        g.dirty |= D_TARGETS;
    }
    g_activity.swaps = g_activity.swaps + 1;
    s_drawsSinceSubmit = 0;
    // every 300 frames: where a swap's time goes (SMS_TIMING_LOG)
    presentTicks += t3 - t2;
    if (lastSwap) swapTicks += t3 - lastSwap;
    lastSwap = t3;
    if (++swaps == 300) {
        static const bool timing = getenv("SMS_TIMING_LOG") && atoi(getenv("SMS_TIMING_LOG"));
        if (timing)
            logf("swaps: acquiring %.2f ms, presenting %.2f ms, %.2f ms apart (avg); GPU memory %.1f MiB",
                 armTicksToNs(s_acquireTicks) / 300e6, armTicksToNs(presentTicks) / 300e6,
                 armTicksToNs(swapTicks) / 299e6,
                 double(g_dev.heaps[HEAP_DATA].used() + g_dev.heaps[HEAP_IMAGE].used()) / 1048576.0);
        s_acquireTicks = presentTicks = swapTicks = 0;
        swaps = 0;
        lastSwap = 0;
    }
}

void setSwapInterval(int interval) {
    s_swapInterval = interval < 0 ? 1 : interval;
    if (s_ready) dkSwapchainSetSwapInterval(g_dev.swapchain, uint32_t(s_swapInterval));
}

void setShaderCache(const char* dir, const char* pack) { setShaderCacheImpl(dir, pack); }

}  // namespace dkgl
