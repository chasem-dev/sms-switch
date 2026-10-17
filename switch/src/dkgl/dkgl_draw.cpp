// dkgl's draws: the GL state a draw needs, sent as deko3d commands where it
// changed since the last draw.
#include "dkgl_state.h"

#include <stdlib.h>
#include <algorithm>

namespace dkgl {

namespace {

DkCompareOp compareOf(GLenum f) {
    switch (f) {
    case GL_NEVER: return DkCompareOp_Never;
    case GL_LESS: return DkCompareOp_Less;
    case GL_EQUAL: return DkCompareOp_Equal;
    case GL_LEQUAL: return DkCompareOp_Lequal;
    case GL_GREATER: return DkCompareOp_Greater;
    case GL_NOTEQUAL: return DkCompareOp_NotEqual;
    case GL_GEQUAL: return DkCompareOp_Gequal;
    default: return DkCompareOp_Always;
    }
}

DkBlendFactor factorOf(GLenum f) {
    switch (f) {
    case GL_ZERO: return DkBlendFactor_Zero;
    case GL_ONE: return DkBlendFactor_One;
    case GL_SRC_COLOR: return DkBlendFactor_SrcColor;
    case GL_ONE_MINUS_SRC_COLOR: return DkBlendFactor_InvSrcColor;
    case GL_SRC_ALPHA: return DkBlendFactor_SrcAlpha;
    case GL_ONE_MINUS_SRC_ALPHA: return DkBlendFactor_InvSrcAlpha;
    case GL_DST_ALPHA: return DkBlendFactor_DstAlpha;
    case GL_ONE_MINUS_DST_ALPHA: return DkBlendFactor_InvDstAlpha;
    case GL_DST_COLOR: return DkBlendFactor_DstColor;
    case GL_ONE_MINUS_DST_COLOR: return DkBlendFactor_InvDstColor;
    case GL_SRC_ALPHA_SATURATE: return DkBlendFactor_SrcAlphaSaturate;
    case GL_CONSTANT_COLOR: return DkBlendFactor_ConstColor;
    case GL_ONE_MINUS_CONSTANT_COLOR: return DkBlendFactor_InvConstColor;
    case GL_CONSTANT_ALPHA: return DkBlendFactor_ConstAlpha;
    case GL_ONE_MINUS_CONSTANT_ALPHA: return DkBlendFactor_InvConstAlpha;
    case GL_SRC1_COLOR: return DkBlendFactor_Src1Color;
    case GL_ONE_MINUS_SRC1_COLOR: return DkBlendFactor_InvSrc1Color;
    case GL_SRC1_ALPHA: return DkBlendFactor_Src1Alpha;
    case GL_ONE_MINUS_SRC1_ALPHA: return DkBlendFactor_InvSrc1Alpha;
    default: return DkBlendFactor_One;
    }
}

DkBlendOp blendOpOf(GLenum e) {
    switch (e) {
    case GL_FUNC_SUBTRACT: return DkBlendOp_Sub;
    case GL_FUNC_REVERSE_SUBTRACT: return DkBlendOp_RevSub;
    case GL_MIN: return DkBlendOp_Min;
    case GL_MAX: return DkBlendOp_Max;
    default: return DkBlendOp_Add;
    }
}

DkLogicOp logicOf(GLenum op) {
    switch (op) {
    case GL_CLEAR: return DkLogicOp_Clear;
    case GL_AND: return DkLogicOp_And;
    case GL_AND_REVERSE: return DkLogicOp_AndReverse;
    case GL_AND_INVERTED: return DkLogicOp_AndInverted;
    case GL_NOOP: return DkLogicOp_NoOp;
    case GL_XOR: return DkLogicOp_Xor;
    case GL_OR: return DkLogicOp_Or;
    case GL_NOR: return DkLogicOp_Nor;
    case GL_EQUIV: return DkLogicOp_Equivalent;
    case GL_INVERT: return DkLogicOp_Invert;
    case GL_OR_REVERSE: return DkLogicOp_OrReverse;
    case GL_COPY_INVERTED: return DkLogicOp_CopyInverted;
    case GL_OR_INVERTED: return DkLogicOp_OrInverted;
    case GL_NAND: return DkLogicOp_Nand;
    case GL_SET: return DkLogicOp_Set;
    default: return DkLogicOp_Copy;
    }
}

// A vertex attribute's deko3d format.
bool attribFormat(const VertexAttrib& a, DkVtxAttribSize* size, DkVtxAttribType* type, uint32_t* bytes) {
    const int n = std::min(std::max(a.size, 1), 4);
    static const DkVtxAttribSize s32[4] = {DkVtxAttribSize_1x32, DkVtxAttribSize_2x32, DkVtxAttribSize_3x32, DkVtxAttribSize_4x32};
    static const DkVtxAttribSize s16[4] = {DkVtxAttribSize_1x16, DkVtxAttribSize_2x16, DkVtxAttribSize_3x16, DkVtxAttribSize_4x16};
    static const DkVtxAttribSize s8[4] = {DkVtxAttribSize_1x8, DkVtxAttribSize_2x8, DkVtxAttribSize_3x8, DkVtxAttribSize_4x8};
    switch (a.type) {
    case GL_FLOAT: *size = s32[n - 1]; *type = DkVtxAttribType_Float; *bytes = 4u * n; return true;
    case GL_UNSIGNED_INT: *size = s32[n - 1]; *type = a.integer ? DkVtxAttribType_Uint : a.normalized ? DkVtxAttribType_Unorm : DkVtxAttribType_Uscaled; *bytes = 4u * n; return true;
    case GL_INT: *size = s32[n - 1]; *type = a.integer ? DkVtxAttribType_Sint : a.normalized ? DkVtxAttribType_Snorm : DkVtxAttribType_Sscaled; *bytes = 4u * n; return true;
    case GL_UNSIGNED_SHORT: *size = s16[n - 1]; *type = a.integer ? DkVtxAttribType_Uint : a.normalized ? DkVtxAttribType_Unorm : DkVtxAttribType_Uscaled; *bytes = 2u * n; return true;
    case GL_SHORT: *size = s16[n - 1]; *type = a.integer ? DkVtxAttribType_Sint : a.normalized ? DkVtxAttribType_Snorm : DkVtxAttribType_Sscaled; *bytes = 2u * n; return true;
    case GL_UNSIGNED_BYTE: *size = s8[n - 1]; *type = a.integer ? DkVtxAttribType_Uint : a.normalized ? DkVtxAttribType_Unorm : DkVtxAttribType_Uscaled; *bytes = 1u * n; return true;
    case GL_BYTE: *size = s8[n - 1]; *type = a.integer ? DkVtxAttribType_Sint : a.normalized ? DkVtxAttribType_Snorm : DkVtxAttribType_Sscaled; *bytes = 1u * n; return true;
    default: return false;
    }
}

uint64_t s_vertexSerial = 0;    // the serial the bound constant attributes were written in
uint32_t s_constMask = 0;       // the bound layout's constant attributes
State::CurrentAttrib s_constValue[16];  // and their values as bound
uint64_t s_uniformSerial = 0;   // likewise the bound default block
DkResHandle s_dummyHandle = 0;  // a 1x1 texture for units with none
Texture* s_dummy = nullptr;

DkResHandle dummyTexture() {
    if (s_dummy && s_dummy->handleValid) return s_dummy->handle;
    if (!s_dummy) {
        s_dummy = new Texture();
        defineLevel(s_dummy, 0, GL_RGBA8, 1, 1);
        const uint32_t black = 0xFF000000u;
        uploadLevel(s_dummy, 0, 0, 0, 1, 1, &black, 4, 0, false);
    }
    SamplerParams s;
    s.minFilter = GL_NEAREST;
    s.magFilter = GL_NEAREST;
    return textureHandle(s_dummy, s);
}

const DkStage kStages[3] = {DkStage_Vertex, DkStage_Geometry, DkStage_Fragment};
const uint32_t kStageBits[3] = {kStageVertex, kStageGeometry, kStageFragment};

}  // namespace

bool validateTargets() {
    Texture* color = nullptr;
    Texture* depth = nullptr;
    int colorLevel = 0, depthLevel = 0;
    if (g.drawFbo == 0) {
        color = &g.backbuffer;
    } else if (const Framebuffer* f = g.framebuffers.get(g.drawFbo)) {
        color = attachmentTexture(f->color);
        depth = attachmentTexture(f->depth);
        colorLevel = f->color.level;
        depthLevel = f->depth.level;
    }
    if (color && !realize(color)) color = nullptr;
    if (depth && !realize(depth)) depth = nullptr;
    if (!(g.dirty & D_TARGETS) && color == g.targetColor && depth == g.targetDepth) return color || depth;
    g.dirty &= ~D_TARGETS;
    if (g.rendered) {
        // what was drawn into the last targets is complete before any of
        // them is sampled
        dkCmdBufBarrier(g_dev.cmd, DkBarrier_Fragments, DkInvalidateFlags_Image);
        g.rendered = false;
    }
    DkImageView cv, dv;
    if (color) cv = viewOf(color, colorLevel);
    if (depth) dv = viewOf(depth, depthLevel);
    const DkImageView* cvs[1] = {&cv};
    dkCmdBufBindRenderTargets(g_dev.cmd, cvs, color ? 1 : 0, depth ? &dv : nullptr);
    g.targetColor = color;
    g.targetDepth = depth;
    Texture* t = color ? color : depth;
    const int level = color ? colorLevel : depthLevel;
    g.targetW = t ? int(std::max(1u, t->levels[0].w >> level)) : 0;
    g.targetH = t ? int(std::max(1u, t->levels[0].h >> level)) : 0;
    // a new target size changes the full-target scissor
    g.dirty |= D_SCISSOR;
    return color || depth;
}

static void validateFixed() {
    DkCmdBuf c = g_dev.cmd;
    const uint32_t d = g.dirty;
    if (d & D_VIEWPORT) {
        DkViewport v = {float(g.viewport[0]), float(g.viewport[1]), float(g.viewport[2]), float(g.viewport[3]),
                        float(g.depthRange[0]), float(g.depthRange[1])};
        dkCmdBufSetViewports(c, 0, &v, 1);
    }
    if (d & D_SCISSOR) {
        DkScissor s;
        if (g.scissorTest) {
            const int x = std::max(0, g.scissor[0]), y = std::max(0, g.scissor[1]);
            const int x1 = std::max(x, g.scissor[0] + g.scissor[2]), y1 = std::max(y, g.scissor[1] + g.scissor[3]);
            s = {uint32_t(x), uint32_t(y), uint32_t(x1 - x), uint32_t(y1 - y)};
        } else {
            s = {0, 0, 16384, 16384};
        }
        dkCmdBufSetScissors(c, 0, &s, 1);
    }
    if (d & D_RASTER) {
        DkRasterizerState rs;
        dkRasterizerStateDefaults(&rs);
        rs.cullMode = !g.cullFace ? DkFace_None
                      : g.cullMode == GL_FRONT ? DkFace_Front
                      : g.cullMode == GL_FRONT_AND_BACK ? DkFace_FrontAndBack
                                                        : DkFace_Back;
        rs.frontFace = g.frontFace == GL_CW ? DkFrontFace_CW : DkFrontFace_CCW;
        rs.depthClampEnable = g.depthClamp;
        dkCmdBufBindRasterizerState(c, &rs);
    }
    if (d & D_COLOR) {
        DkColorState cs;
        dkColorStateDefaults(&cs);
        cs.blendEnableMask = g.blend ? 1 : 0;
        cs.logicOp = g.logicOp ? logicOf(g.logicOpMode) : DkLogicOp_Copy;
        dkCmdBufBindColorState(c, &cs);
    }
    if (d & D_COLORWRITE) {
        DkColorWriteState cw;
        dkColorWriteStateDefaults(&cw);
        dkColorWriteStateSetMask(&cw, 0,
                                 (g.colorMask[0] ? DkColorMask_R : 0) | (g.colorMask[1] ? DkColorMask_G : 0) |
                                     (g.colorMask[2] ? DkColorMask_B : 0) | (g.colorMask[3] ? DkColorMask_A : 0));
        dkCmdBufBindColorWriteState(c, &cw);
    }
    if (d & D_BLEND) {
        DkBlendState bs;
        dkBlendStateDefaults(&bs);
        dkBlendStateSetOps(&bs, blendOpOf(g.blendEq[0]), blendOpOf(g.blendEq[1]));
        dkBlendStateSetFactors(&bs, factorOf(g.blendFunc[0]), factorOf(g.blendFunc[1]), factorOf(g.blendFunc[2]),
                               factorOf(g.blendFunc[3]));
        dkCmdBufBindBlendState(c, 0, &bs);
    }
    if (d & D_BLENDCONST) dkCmdBufSetBlendConst(c, g.blendColor[0], g.blendColor[1], g.blendColor[2], g.blendColor[3]);
    if (d & D_DEPTH) {
        DkDepthStencilState ds;
        dkDepthStencilStateDefaults(&ds);
        ds.depthTestEnable = g.depthTest;
        ds.depthWriteEnable = g.depthTest && g.depthMask;  // GL writes depth only with the test on
        ds.depthCompareOp = compareOf(g.depthFunc);
        ds.stencilTestEnable = false;
        dkCmdBufBindDepthStencilState(c, &ds);
    }
    if (d & D_LINE) dkCmdBufSetLineWidth(c, g.lineWidth);
    if (d & D_POINT) dkCmdBufSetPointSize(c, g.pointSize);
    g.dirty &= ~(D_VIEWPORT | D_SCISSOR | D_RASTER | D_COLOR | D_COLORWRITE | D_BLEND | D_BLENDCONST | D_DEPTH |
                 D_LINE | D_POINT);
}

bool validateDraw() {
    g_activity.draws = g_activity.draws + 1;
    Program* p = g.programs.get(g.program);
    if (!p || !p->linked) return false;
    if (!validateTargets()) return false;

    // textures first: making one may record its upload
    DkResHandle handles[kMaxSamplerBindings];
    for (int k = 0; k < p->samplerCount; k++) {
        const int unit = p->samplerUnits[size_t(k)];
        Texture* t = unit >= 0 && unit < 32 ? g.textures.get(g.tex2D[unit]) : nullptr;
        if (t && !t->levels.empty() && t->levels[0].defined) {
            const SamplerParams* sp = g.samplerObj[unit] ? g.samplers.get(g.samplerObj[unit]) : nullptr;
            handles[k] = textureHandle(t, sp ? *sp : t->sampler);
        } else {
            handles[k] = dummyTexture();
        }
    }
    barrierBeforeDraw();
    validateFixed();
    DkCmdBuf c = g_dev.cmd;

    const bool programChanged = g.boundProgram != p || g.boundProgramVersion != p->version;
    if (programChanged) {
        const DkShader* shaders[3] = {&p->vs, &p->fs, nullptr};
        int n = 2;
        if (p->hasGs) {
            shaders[1] = &p->gs;
            shaders[2] = &p->fs;
            n = 3;
        }
        dkCmdBufBindShaders(c, DkStageFlag_GraphicsMask, shaders, uint32_t(n));
        g.boundProgram = p;
        g.boundProgramVersion = p->version;
        g.boundTexCount[0] = g.boundTexCount[1] = 0;
        memset(g.boundUbo, 0, sizeof g.boundUbo);
    }

    // textures, per stage that samples
    for (int si = 0; si < 3; si++) {
        if (si == 1 && !p->hasGs) continue;
        const uint32_t bit = kStageBits[si];
        bool any = false;
        for (int k = 0; k < p->samplerCount && !any; k++) any = (p->samplerStages[k] & bit) != 0;
        if (!any) continue;
        // the bound handles are remembered for the vertex and fragment stages
        const int slot = si == 2 ? 1 : 0;
        if (si != 1 && g.boundTexCount[slot] == p->samplerCount &&
            !memcmp(g.boundTex[slot], handles, sizeof(DkResHandle) * size_t(p->samplerCount)))
            continue;
        dkCmdBufBindTextures(c, kStages[si], 0, handles, uint32_t(p->samplerCount));
        if (si != 1) {
            memcpy(g.boundTex[slot], handles, sizeof(DkResHandle) * size_t(p->samplerCount));
            g.boundTexCount[slot] = p->samplerCount;
        }
    }

    // the loose uniforms (transient memory: bound again in each new serial)
    if (p->glsl.defaultBlockSize &&
        (programChanged || p->defaultDirty || s_uniformSerial != g_dev.serial || (g.dirty & D_UNIFORMS))) {
        Alloc a = transient(p->glsl.defaultBlockSize, DK_UNIFORM_BUF_ALIGNMENT);
        memcpy(a.cpu, p->defaultBlock.data(), p->glsl.defaultBlockSize);
        for (int si = 0; si < 3; si++) {
            if (si == 1 && !p->hasGs) continue;
            dkCmdBufBindUniformBuffer(c, kStages[si], kDefaultBlockBinding, a.gpu, p->glsl.defaultBlockSize);
        }
        p->defaultDirty = false;
        s_uniformSerial = g_dev.serial;
    }
    // named uniform blocks, from their GL binding points
    for (size_t i = 0; i < p->glsl.blocks.size(); i++) {
        const UboBinding& ub = g.ubo[p->blockBindings[i] & 15];
        const Buffer* b = g.buffers.get(ub.buffer);
        if (!b || !b->mem) continue;
        DkBufExtents e = {b->mem.gpu + uint64_t(ub.offset),
                          uint32_t(std::min<GLsizeiptr>(ub.size ? ub.size : b->size - ub.offset, DK_UNIFORM_BUF_MAX_SIZE))};
        for (int si = 0; si < 3; si++) {
            if (!(p->glsl.blockStages[i] & kStageBits[si])) continue;
            if (si != 1) {
                DkBufExtents& bound = g.boundUbo[si == 2 ? 1 : 0][i];
                if (bound.addr == e.addr && bound.size == e.size) continue;
                bound = e;
            }
            dkCmdBufBindUniformBuffers(c, kStages[si], uint32_t(i), &e, 1);
        }
    }
    g.dirty &= ~D_UNIFORMS;

    // vertex inputs: the layout when the vertex array, the program's inputs
    // or the serial changed; else only the constants whose values changed
    // (GX sets the matrix indices as one for each batch)
    const VertexArray* va = currentVao();
    const bool layout = va != g.boundVao || va->version != g.boundVaoVersion || p->attribMask != g.boundAttribMask ||
                        s_vertexSerial != g_dev.serial;
    if (layout) {
        DkVtxAttribState attribs[16];
        DkVtxBufferState bufStates[16];
        DkBufExtents bufs[16];
        uint32_t n = 0;
        s_constMask = 0;
        for (int loc = 0; loc < 16; loc++)
            if (p->attribMask & (1u << loc)) n = uint32_t(loc + 1);
        for (uint32_t loc = 0; loc < n; loc++) {
            memset(&attribs[loc], 0, sizeof attribs[loc]);
            bufStates[loc] = {0, 0};
            bufs[loc] = {0, 0};
            if (!(p->attribMask & (1u << loc))) {
                attribs[loc].isFixed = 1;
                attribs[loc].size = DkVtxAttribSize_1x32;
                attribs[loc].type = DkVtxAttribType_Float;
                continue;
            }
            const VertexAttrib& a = va->attribs[loc];
            const Buffer* b = a.enabled ? g.buffers.get(a.buffer) : nullptr;
            DkVtxAttribSize size;
            DkVtxAttribType type;
            uint32_t bytes;
            if (b && b->mem && attribFormat(a, &size, &type, &bytes)) {
                attribs[loc].bufferId = loc;
                attribs[loc].size = size;
                attribs[loc].type = type;
                bufStates[loc].stride = a.stride ? uint32_t(a.stride) : bytes;
                bufs[loc] = {b->mem.gpu + a.offset, uint32_t(b->size > a.offset ? b->size - a.offset : 0)};
            } else {
                // GL's current value for the attribute: one element, read per
                // instance (the draws have one), so by every vertex
                const State::CurrentAttrib& cur = g.current[loc];
                Alloc m = transient(16, 16);
                memcpy(m.cpu, cur.v, 16);
                attribs[loc].bufferId = loc;
                attribs[loc].size = DkVtxAttribSize_4x32;
                attribs[loc].type = cur.integer ? DkVtxAttribType_Uint : DkVtxAttribType_Float;
                bufStates[loc].stride = 16;
                bufStates[loc].divisor = 1;
                bufs[loc] = {m.gpu, 16};
                s_constMask |= 1u << loc;
                s_constValue[loc] = cur;
            }
        }
        dkCmdBufBindVtxAttribState(c, attribs, n);
        dkCmdBufBindVtxBufferState(c, bufStates, n);
        if (n) dkCmdBufBindVtxBuffers(c, 0, bufs, n);
        g.boundVao = va;
        g.boundVaoVersion = va->version;
        g.boundCurrentVersion = g.currentVersion;
        g.boundAttribMask = p->attribMask;
        s_vertexSerial = g_dev.serial;
    } else if (g.currentVersion != g.boundCurrentVersion) {
        for (int loc = 0; loc < 16; loc++) {
            if (!(s_constMask & (1u << loc))) continue;
            const State::CurrentAttrib& cur = g.current[loc];
            State::CurrentAttrib& was = s_constValue[loc];
            if (cur.integer != was.integer) {
                // another type: the layout again, at the next draw
                g.boundVao = nullptr;
                return validateDraw();
            }
            if (!memcmp(cur.v, was.v, 16)) continue;
            Alloc m = transient(16, 16);
            memcpy(m.cpu, cur.v, 16);
            dkCmdBufBindVtxBuffer(c, uint32_t(loc), m.gpu, 16);
            was = cur;
        }
        g.boundCurrentVersion = g.currentVersion;
    }
    return true;
}

// SMS_DKGL_TRACE=n: the first n draws' state in the log.
void traceDraw(const char* what, GLenum mode, int count, uint32_t first, int base, DkGpuAddr indices) {
    static int left = -1;
    if (left < 0) {
        const char* e = getenv("SMS_DKGL_TRACE");
        left = e ? atoi(e) : 0;
    }
    if (left <= 0) return;
    left--;
    const Program* p = g.programs.get(g.program);
    logf("draw %s: mode 0x%x count %d first %u base %d indices 0x%llx; fbo %u (%dx%d) viewport %d %d %d %d "
         "scissor %s %d %d %d %d; depth %d func 0x%x mask %d; cull %d 0x%x front 0x%x; blend %d; colormask %d%d%d%d",
         what, mode, count, first, base, (unsigned long long)indices, g.drawFbo, g.targetW, g.targetH,
         g.viewport[0], g.viewport[1], g.viewport[2], g.viewport[3], g.scissorTest ? "on" : "off", g.scissor[0],
         g.scissor[1], g.scissor[2], g.scissor[3], g.depthTest, g.depthFunc, g.depthMask, g.cullFace, g.cullMode,
         g.frontFace, g.blend, g.colorMask[0], g.colorMask[1], g.colorMask[2], g.colorMask[3]);
    if (!p) return;
    logf("  program %u: attribs 0x%x, %d samplers, %zu blocks, default block %u bytes, gs %d", g.program,
         p->attribMask, p->samplerCount, p->glsl.blocks.size(), p->glsl.defaultBlockSize, p->hasGs);
    const VertexArray* va = currentVao();
    for (int loc = 0; loc < 16; loc++) {
        if (!(p->attribMask & (1u << loc))) continue;
        const VertexAttrib& a = va->attribs[loc];
        const Buffer* b = g.buffers.get(a.buffer);
        if (a.enabled && b && b->mem) {
            float f[3] = {0, 0, 0};
            const uint32_t at = uint32_t(a.offset) + uint32_t(std::max(base, 0)) * uint32_t(a.stride);
            if (a.type == GL_FLOAT && at + 12 <= b->size) memcpy(f, b->mem.cpu + at, 12);
            logf("  attrib %d: buffer %u at 0x%llx + %llu stride %d size %d type 0x%x%s%s; vertex %d: %g %g %g", loc,
                 a.buffer, (unsigned long long)b->mem.gpu, (unsigned long long)a.offset, a.stride, a.size, a.type,
                 a.normalized ? " norm" : "", a.integer ? " int" : "", base, f[0], f[1], f[2]);
        } else {
            const State::CurrentAttrib& c = g.current[loc];
            logf("  attrib %d: constant %s %08x %08x %08x %08x", loc, c.integer ? "uint" : "float", c.v[0], c.v[1],
                 c.v[2], c.v[3]);
        }
    }
    for (size_t i = 0; i < p->glsl.blocks.size(); i++) {
        const UboBinding& ub = g.ubo[p->blockBindings[i] & 15];
        const Buffer* b = g.buffers.get(ub.buffer);
        logf("  block %zu %s: binding %d buffer %u offset %lld size %lld (gpu 0x%llx)", i, p->glsl.blocks[i].c_str(),
             p->blockBindings[i], ub.buffer, (long long)ub.offset, (long long)ub.size,
             (unsigned long long)(b && b->mem ? b->mem.gpu : 0));
    }
    for (const GlslUniform& u : p->glsl.uniforms) {
        if (u.binding >= 0) {
            logf("  sampler %s: binding %d unit %d", u.name.c_str(), u.binding, p->samplerUnits[size_t(u.binding)]);
            continue;
        }
        uint32_t w[4] = {0, 0, 0, 0};
        if (u.offset + 16 <= p->defaultBlock.size()) memcpy(w, &p->defaultBlock[u.offset], 16);
        logf("  uniform %s @%u: %08x %08x %08x %08x", u.name.c_str(), u.offset, w[0], w[1], w[2], w[3]);
    }
}

}  // namespace dkgl
