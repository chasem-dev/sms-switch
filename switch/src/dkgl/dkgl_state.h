// dkgl's GL context state (one context), shared by its source files.
#ifndef SMS_SWITCH_DKGL_STATE_H
#define SMS_SWITCH_DKGL_STATE_H

#include "dkgl_internal.h"

namespace dkgl {

enum : uint32_t {
    D_TARGETS = 1u << 0,    // render targets
    D_VIEWPORT = 1u << 1,
    D_SCISSOR = 1u << 2,
    D_RASTER = 1u << 3,     // cull, front face, depth clamp
    D_COLOR = 1u << 4,      // blend enable, logic op
    D_COLORWRITE = 1u << 5,
    D_BLEND = 1u << 6,
    D_BLENDCONST = 1u << 7,
    D_DEPTH = 1u << 8,
    D_LINE = 1u << 9,
    D_POINT = 1u << 10,
    D_SHADERS = 1u << 11,
    D_VERTEX = 1u << 12,
    D_TEXTURES = 1u << 13,
    D_UNIFORMS = 1u << 14,
    D_ALL = 0x7FFF,
};

// Object names: index into a vector of owned objects (0 is no object).
template <class T> struct Names {
    std::vector<std::unique_ptr<T>> objs{1};
    std::vector<GLuint> freeNames;
    GLuint gen() {
        GLuint n;
        if (!freeNames.empty()) {
            n = freeNames.back();
            freeNames.pop_back();
        } else {
            n = GLuint(objs.size());
            objs.emplace_back();
        }
        objs[n].reset(new T());
        return n;
    }
    T* get(GLuint n) const { return n < objs.size() ? objs[n].get() : nullptr; }
    void del(GLuint n) {
        if (n && n < objs.size() && objs[n]) {
            objs[n].reset();
            freeNames.push_back(n);
        }
    }
};

struct UboBinding {
    GLuint buffer = 0;
    GLintptr offset = 0;
    GLsizeiptr size = 0;
};

struct State {
    Names<Buffer> buffers;
    Names<Texture> textures;  // and renderbuffers
    Names<Framebuffer> framebuffers;
    Names<VertexArray> vertexArrays;
    Names<Shader> shaders;
    Names<Program> programs;  // shaders and programs share GL's name space: kept apart here
    Names<SamplerParams> samplers;
    Names<Query> queries;

    GLuint arrayBuffer = 0, packBuffer = 0, unpackBuffer = 0, uniformBuffer = 0, elementFallback = 0;
    UboBinding ubo[16];
    GLuint vao = 0, program = 0, readFbo = 0, drawFbo = 0, renderbuffer = 0;
    int activeUnit = 0;
    GLuint tex2D[32] = {}, samplerObj[32] = {};
    VertexArray defaultVao;

    bool blend = false, depthTest = false, cullFace = false, scissorTest = false, logicOp = false;
    bool clip0 = false, clip1 = false, depthClamp = false, stencilTest = false;
    GLenum blendEq[2] = {GL_FUNC_ADD, GL_FUNC_ADD};
    GLenum blendFunc[4] = {GL_ONE, GL_ZERO, GL_ONE, GL_ZERO};  // src rgb, dst rgb, src a, dst a
    float blendColor[4] = {0, 0, 0, 0};
    GLenum logicOpMode = GL_COPY, depthFunc = GL_LESS, cullMode = GL_BACK, frontFace = GL_CCW;
    bool depthMask = true;
    bool colorMask[4] = {true, true, true, true};
    float clearColor[4] = {0, 0, 0, 0};
    double clearDepth = 1.0;
    GLint viewport[4] = {0, 0, 0, 0};
    double depthRange[2] = {0.0, 1.0};
    GLint scissor[4] = {0, 0, 0, 0};
    float lineWidth = 1.0f, pointSize = 1.0f;
    GLint packAlign = 4, packRowLength = 0, unpackAlign = 4, unpackRowLength = 0;
    struct CurrentAttrib {
        uint32_t v[4] = {0, 0, 0, 0x3F800000};  // (0, 0, 0, 1.0f)
        bool integer = false;
    } current[16];
    uint32_t currentVersion = 1;

    uint32_t dirty = D_ALL;
    GLenum error = GL_NO_ERROR;

    // what the command buffer has bound
    bool rendered = false;      // drawn since the last barrier (an RT may be sampled next)
    bool transferred = false;   // a copy wrote since the last barrier
    const Program* boundProgram = nullptr;
    uint32_t boundProgramVersion = 0;
    DkResHandle boundTex[2][kMaxSamplerBindings];
    int boundTexCount[2] = {0, 0};
    DkBufExtents boundUbo[2][16];
    const VertexArray* boundVao = nullptr;
    uint32_t boundVaoVersion = 0, boundCurrentVersion = 0, boundAttribMask = ~0u;
    Texture* targetColor = nullptr;   // the bound render targets
    Texture* targetDepth = nullptr;
    int targetW = 0, targetH = 0;
    Texture backbuffer;  // the default framebuffer
    Alloc defaultUbo;    // the bound program's loose uniforms, as last uploaded
};
extern State g;

inline VertexArray* currentVao() { return g.vao ? g.vertexArrays.get(g.vao) : &g.defaultVao; }

// dkgl_texture.cpp
bool realize(Texture* t);
DkImageView viewOf(Texture* t, int level);
void defineLevel(Texture* t, int level, GLenum internalFormat, uint32_t w, uint32_t h);
void uploadLevel(Texture* t, int level, int x, int y, uint32_t w, uint32_t h, const void* data, uint32_t bytes,
                 uint32_t rowLength, bool compressed);
DkResHandle textureHandle(Texture* t, const SamplerParams& s);
void barrierBeforeTransfer();
void barrierBeforeDraw();
Texture* attachmentTexture(const Attachment& a);
void destroyTexture(Texture* t);

// dkgl_program.cpp
bool linkProgram(Program* p);
void destroyProgram(Program* p);

// dkgl_draw.cpp
bool validateDraw();
bool validateTargets();
void traceDraw(const char* what, GLenum mode, int count, uint32_t first, int base, DkGpuAddr indices);

}  // namespace dkgl

#endif
