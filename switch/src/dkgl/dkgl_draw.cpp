// dkgl's draws: the GL state a draw needs, sent as deko3d commands where it
// changed since the last draw.
#include "dkgl_state.h"

#include <switch.h>
#include <stdio.h>
#include <stdlib.h>
#include <algorithm>

namespace dkgl {
extern bool g_tracing;  // below

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
Alloc s_defaultBound;           // (for SMS_DKGL_CHECK_INPUTS)
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
        color = defaultFramebuffer();
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
        {
            extern bool tiledFlush();
            if (tiledFlush()) dkCmdBufTiledCacheOp(g_dev.cmd, DkTiledCacheOp_Flush);
        }
        dkCmdBufBarrier(g_dev.cmd, DkBarrier_Fragments, DkInvalidateFlags_Image);
        g.rendered = false;
    }
    DkImageView cv, dv;
    if (color) cv = viewOf(color, colorLevel);
    if (depth) dv = viewOf(depth, depthLevel);
    const DkImageView* cvs[1] = {&cv};
    dkCmdBufBindRenderTargets(g_dev.cmd, cvs, color ? 1 : 0, depth ? &dv : nullptr);
    if (g_tracing) logf("  cmd: render targets");
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
        // (SMS_DKGL_NO_DEPTH_CLAMP=1: never, a test)
        static const bool noClamp = getenv("SMS_DKGL_NO_DEPTH_CLAMP") && atoi(getenv("SMS_DKGL_NO_DEPTH_CLAMP"));
        rs.depthClampEnable = g.depthClamp && !noClamp;
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
        // (SMS_DKGL_NOMASK_ALWAYS=1: draws that do not write depth pass the
        // depth test always, a test)
        static const bool always = getenv("SMS_DKGL_NOMASK_ALWAYS") && atoi(getenv("SMS_DKGL_NOMASK_ALWAYS"));
        if (always && !g.depthMask) ds.depthCompareOp = DkCompareOp_Always;
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
        // (SMS_DKGL_NO_FEEDBACK=1: a render target bound as a texture of the
        // same draw reads the dummy texture instead, a test)
        static const bool noFeedback = getenv("SMS_DKGL_NO_FEEDBACK") && atoi(getenv("SMS_DKGL_NO_FEEDBACK"));
        if (noFeedback && t && (t == g.targetColor || t == g.targetDepth)) t = nullptr;
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
        // the clip distances the program writes, and only those (deko3d
        // enables all eight: the others held what earlier programs left,
        // and dropped the title logo and part of its sky)
        static uint32_t clipMask = 0xFF;
        if (p->clipMask != clipMask) {
            clipMask = p->clipMask;
            const uint32_t word = 0x544u | (clipMask << 16) | (4u << 29);  // 3D ClipDistanceEnable, inline
            dkCmdBufReplayCmds(c, &word, 1);
        }
        if (g_tracing) logf("  cmd: shaders");
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
        // (SMS_DKGL_SAMPLER_EVERY=1: the sampler set bound again with every
        // texture binding, a test)
        static const int samplerMode = getenv("SMS_DKGL_SAMPLER_EVERY") ? atoi(getenv("SMS_DKGL_SAMPLER_EVERY")) : 2;
        if (samplerMode == 1) dkCmdBufBindSamplerDescriptorSet(c, g_dev.samplerDescs.gpu, 2048);
        if (samplerMode == 3) {
            dkCmdBufBindImageDescriptorSet(c, g_dev.imageDescs.gpu, 8192);
            dkCmdBufBindSamplerDescriptorSet(c, g_dev.samplerDescs.gpu, 2048);
        }
        if (samplerMode == 2) {
            // a sampler descriptor not read since the set was bound: bound
            // again first (test: on the console a sampler first read later
            // came out stale, the plaza's ground in solid colour)
            bool fresh = false;
            for (int k = 0; k < p->samplerCount; k++) {
                const uint32_t id = handles[k] >> 20;
                if (id < g_dev.samplerUsed.size() && !g_dev.samplerUsed[id]) fresh = true;
            }
            if (fresh) {
                if (g_tracing) logf("  cmd: sampler set (fresh)");
                dkCmdBufBindSamplerDescriptorSet(c, g_dev.samplerDescs.gpu, 2048);
                
                std::fill(g_dev.samplerUsed.begin(), g_dev.samplerUsed.end(), false);
            }
            for (int k = 0; k < p->samplerCount; k++) {
                const uint32_t id = handles[k] >> 20;
                if (id < g_dev.samplerUsed.size()) g_dev.samplerUsed[id] = true;
            }
            // (SMS_DKGL_IMAGE_FRESH=1: likewise the image set, a test)
            static const bool imageFresh = getenv("SMS_DKGL_IMAGE_FRESH") && atoi(getenv("SMS_DKGL_IMAGE_FRESH"));
            if (imageFresh) {
                bool freshImage = false;
                for (int k = 0; k < p->samplerCount; k++) {
                    const uint32_t id = handles[k] & 0xFFFFF;
                    if (id < g_dev.imageUsed.size() && !g_dev.imageUsed[id]) freshImage = true;
                }
                if (freshImage) {
                    dkCmdBufBindImageDescriptorSet(c, g_dev.imageDescs.gpu, 8192);
                    std::fill(g_dev.imageUsed.begin(), g_dev.imageUsed.end(), false);
                }
                for (int k = 0; k < p->samplerCount; k++) {
                    const uint32_t id = handles[k] & 0xFFFFF;
                    if (id < g_dev.imageUsed.size()) g_dev.imageUsed[id] = true;
                }
            }
        }
        dkCmdBufBindTextures(c, kStages[si], 0, handles, uint32_t(p->samplerCount));
        if (g_tracing) logf("  cmd: textures stage %d: %08x %08x %08x %08x", si, handles[0], p->samplerCount > 1 ? handles[1] : 0, p->samplerCount > 2 ? handles[2] : 0, p->samplerCount > 3 ? handles[3] : 0);
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
            if (g_tracing) logf("  cmd: default block stage %d at 0x%llx", si, (unsigned long long)a.gpu);
        }
        p->defaultDirty = false;
        s_uniformSerial = g_dev.serial;
        s_defaultBound = a;
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
            if (g_tracing) logf("  cmd: block %zu stage %d at 0x%llx", i, si, (unsigned long long)e.addr);
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
                // (SMS_DKGL_CONST_STRIDE0=1: read per vertex with a stride
                // of 0 instead of per instance, a test)
                static const bool stride0 = getenv("SMS_DKGL_CONST_STRIDE0") && atoi(getenv("SMS_DKGL_CONST_STRIDE0"));
                bufStates[loc].stride = stride0 ? 0 : 16;
                bufStates[loc].divisor = stride0 ? 0 : 1;
                bufs[loc] = {m.gpu, 16};
                s_constMask |= 1u << loc;
                s_constValue[loc] = cur;
            }
        }
        dkCmdBufBindVtxAttribState(c, attribs, n);
        dkCmdBufBindVtxBufferState(c, bufStates, n);
        if (n) dkCmdBufBindVtxBuffers(c, 0, bufs, n);
        if (g_tracing) for (uint32_t k = 0; k < n; k++) logf("  cmd: vertex buffer %u at 0x%llx size %u stride %u", k, (unsigned long long)bufs[k].addr, bufs[k].size, bufStates[k].stride);
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
            if (g_tracing) logf("  cmd: constant %d at 0x%llx", loc, (unsigned long long)m.gpu);
            was = cur;
        }
        g.boundCurrentVersion = g.currentVersion;
    }
    return true;
}

extern uint64_t g_swapCount;  // dkgl_api.cpp
}  // namespace dkgl
extern "C" bool GXPC_WindowShotSink(uint32_t frame, const uint8_t* rgba, int w, int h);
namespace dkgl {
bool g_tracing = false;       // within SMS_DKGL_TRACE's window: traceCall logs
void tracePixels();            // dkgl_api.cpp
void startList();              // dkgl_device.cpp
void readPixelsTest(GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, void* pixels);  // dkgl_api.cpp

// SMS_DKGL_TRACE=n: the first n draws' state in the log; with
// SMS_DKGL_TRACE_FROM=frame, the first n from that frame (swap) on.
void traceDraw(const char* what, GLenum mode, int count, uint32_t first, int base, DkGpuAddr indices) {
    static int left = -1, skip = 0;
    static uint64_t from = 0;
    if (left < 0) {
        const char* e = getenv("SMS_DKGL_TRACE");
        left = e ? atoi(e) : 0;
        const char* f = getenv("SMS_DKGL_TRACE_FROM");
        from = f ? strtoull(f, nullptr, 10) : 0;
        const char* k = getenv("SMS_DKGL_TRACE_SKIP");  // draws passed over first
        skip = k ? atoi(k) : 0;
    }
    // SMS_DKGL_WAIT_AT=n[,n...]: the GPU waited for before those draws of
    // the frame SMS_DKGL_TRACE_FROM (counted from its first), whatever is traced
    {
        static std::vector<int> waitAt;
        static bool parsed = false;
        static uint64_t frame = ~0ull;
        static int index = 0;
        if (!parsed) {
            parsed = true;
            if (const char* w = getenv("SMS_DKGL_WAIT_AT"))
                for (const char* q = w; *q;) {
                    char* end;
                    waitAt.push_back(int(strtol(q, &end, 10)));
                    q = *end ? end + 1 : end;
                }
        }
        if (!waitAt.empty() && g_swapCount == from) {
            if (frame != g_swapCount) frame = g_swapCount, index = 0;
            static const bool noWait = getenv("SMS_DKGL_WAIT_NOWAIT") && atoi(getenv("SMS_DKGL_WAIT_NOWAIT"));
            // SMS_DKGL_WAIT_ACTION: instead of a submission, only part of
            // what one does (uniforms: bound again; descsets; l2; full: a full
            // barrier)
            static const char* action = getenv("SMS_DKGL_WAIT_ACTION");
            if (std::find(waitAt.begin(), waitAt.end(), index) != waitAt.end()) {
                if (!action) submit(!noWait);
                else if (!strcmp(action, "uniforms")) s_uniformSerial = ~0ull;
                else if (!strcmp(action, "descsets")) {
                    dkCmdBufBindImageDescriptorSet(g_dev.cmd, g_dev.imageDescs.gpu, 8192);
                    dkCmdBufBindSamplerDescriptorSet(g_dev.cmd, g_dev.samplerDescs.gpu, 2048);
                } else if (!strcmp(action, "l2")) dkCmdBufBarrier(g_dev.cmd, DkBarrier_None, DkInvalidateFlags_L2Cache);
                else if (!strcmp(action, "full"))
                    dkCmdBufBarrier(g_dev.cmd, DkBarrier_Full, DkInvalidateFlags_L2Cache | DkInvalidateFlags_Image |
                                                                  DkInvalidateFlags_Shader | DkInvalidateFlags_Descriptors);
                else if (!strcmp(action, "textures")) memset(g.boundTex, 0, sizeof g.boundTex), g.boundTexCount[0] = g.boundTexCount[1] = 0;
                else if (!strcmp(action, "imageset")) dkCmdBufBindImageDescriptorSet(g_dev.cmd, g_dev.imageDescs.gpu, 8192);
                else if (!strcmp(action, "samplerset")) dkCmdBufBindSamplerDescriptorSet(g_dev.cmd, g_dev.samplerDescs.gpu, 2048);
                else if (!strcmp(action, "descinval")) dkCmdBufBarrier(g_dev.cmd, DkBarrier_None, DkInvalidateFlags_Descriptors);
                else if (!strcmp(action, "descwfi")) dkCmdBufBarrier(g_dev.cmd, DkBarrier_Primitives, DkInvalidateFlags_Descriptors);
                else if (!strcmp(action, "sleep2")) svcSleepThread(2000000);
                else if (!strcmp(action, "waitsleep2")) submit(true), svcSleepThread(2000000);
                else if (!strcmp(action, "readcolor") || !strcmp(action, "readdepth")) {
                    // what SMS_DKGL_TRACE_PIXEL does, one pixel of the draw target
                    const GLuint read = g.readFbo, pack = g.packBuffer;
                    g.readFbo = g.drawFbo;
                    g.packBuffer = 0;
                    uint32_t px = 0;
                    if (!strcmp(action, "readcolor")) readPixelsTest(840, 20, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, &px);
                    else readPixelsTest(840, 20, 1, 1, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, &px);
                    g.readFbo = read;
                    g.packBuffer = pack;
                }
                else if (!strcmp(action, "vtx")) g.boundVao = nullptr, s_vertexSerial = ~0ull;
                else if (!strcmp(action, "startlist")) startList(), s_vertexSerial = ~0ull, s_uniformSerial = ~0ull;
                else if (!strcmp(action, "tiles")) dkCmdBufBarrier(g_dev.cmd, DkBarrier_Tiles, 0);
                else if (!strcmp(action, "cmdfence") || !strcmp(action, "cmdfencenoflush")) {
                    static DkFence f;
                    dkCmdBufSignalFence(g_dev.cmd, &f, !strcmp(action, "cmdfence"));
                }
                else if (!strcmp(action, "wfi")) dkCmdBufBarrier(g_dev.cmd, DkBarrier_Primitives, 0);
                else if (!strcmp(action, "fragimg")) dkCmdBufBarrier(g_dev.cmd, DkBarrier_Fragments, DkInvalidateFlags_Image);
                else if (!strcmp(action, "flags")) g.unfinished = false, g.rendered = false;
                else if (!strcmp(action, "sleep")) svcSleepThread(8000000);  // 8 ms, nothing else
                else if (!strcmp(action, "submitsleep")) submit(false), svcSleepThread(40000000);
                else if (!strcmp(action, "submitsleeppoll")) submit(false), svcSleepThread(40000000), poll();
                else if (!strcmp(action, "submitsleepcount")) {
                    // the completed serial noticed, nothing freed: from now on
                    extern bool g_pollNoFree;
                    submit(false), svcSleepThread(40000000);
                    g_pollNoFree = true;
                    poll();
                }
                else if (!strcmp(action, "gpuwait")) {
                    // the GPU waited for without ending the list: a fence
                    // in the queue's own stream after what was submitted
                    submit(true);
                }
            }
            index++;
        }
    }
    // SMS_DKGL_COUNT_AT=n: the GPU's pipeline counters around draw n of the
    // frame SMS_DKGL_TRACE_FROM (reported after draws n-1 and n), in the log
    // at the next frame
    {
        static const int countAt = getenv("SMS_DKGL_COUNT_AT") ? atoi(getenv("SMS_DKGL_COUNT_AT")) : -1;
        static const DkCounter kinds[] = {DkCounter_InputVertices, DkCounter_InputPrimitives,
                                          DkCounter_VertexShaderInvocations, DkCounter_ClipperInputPrimitives,
                                          DkCounter_ClipperOutputPrimitives, DkCounter_FragmentShaderInvocations,
                                          DkCounter_SamplesPassed, DkCounter_PrimitivesGenerated};
        static const char* names[] = {"input vertices", "input primitives", "vertex shader runs", "clipper in",
                                      "clipper out", "fragment shader runs", "samples passed", "primitives generated"};
        const int nk = int(sizeof kinds / sizeof kinds[0]);
        static Alloc mem;
        static int index = 0, state = 0;  // 1: reported, 2: read
        static uint64_t frame = ~0ull;
        if (countAt >= 0 && g_swapCount == from) {
            if (frame != g_swapCount) frame = g_swapCount, index = 0;
            if (index == countAt - 1 || index == countAt) {
                if (!mem) mem = g_dev.heaps[HEAP_READBACK].alloc(uint32_t(2 * nk * 16), 32);
                const int set = index == countAt ? 1 : 0;
                for (int k = 0; k < nk; k++) dkCmdBufReportCounter(g_dev.cmd, kinds[k], mem.gpu + uint32_t((set * nk + k) * 16));
                if (set) state = 1;
            }
            index++;
        } else if (state == 1 && g_swapCount > from) {
            submit(true);
            dkMemBlockFlushCpuCache(mem.block, mem.offset, mem.size);
            for (int k = 0; k < nk; k++) {
                uint64_t a, b;
                memcpy(&a, mem.cpu + k * 16, 8);
                memcpy(&b, mem.cpu + (nk + k) * 16, 8);
                logf("count: draw %d: %s %llu", countAt, names[k], (unsigned long long)(b - a));
            }
            state = 2;
        }
    }
    // SMS_DKGL_SNAP_AT=n[,n...] (up to 4): after those draws of the frame
    // SMS_DKGL_TRACE_FROM, a GPU copy of the colour target, in the command
    // stream with no wait (unlike the dumps, it leaves the GPU's timing
    // alone); sent as captures 100100 + k at the next frame
    {
        static std::vector<int> snapAt;
        static bool parsed = false;
        static Texture* snaps[4] = {};
        static int taken = 0, index = 0;
        static uint64_t frame = ~0ull;
        if (!parsed) {
            parsed = true;
            if (const char* w = getenv("SMS_DKGL_SNAP_AT"))
                for (const char* q = w; *q && snapAt.size() < 4;) {
                    char* end;
                    snapAt.push_back(int(strtol(q, &end, 10)));
                    q = *end ? end + 1 : end;
                }
        }
        if (!snapAt.empty() && g_swapCount == from) {
            if (frame != g_swapCount) frame = g_swapCount, index = 0;
            for (size_t k = 0; k < snapAt.size(); k++) {
                if (snapAt[k] != index || !g.targetColor) continue;
                // (SMS_DKGL_SNAP_TEX=name: that texture instead of the target)
                static const GLuint snapTex = getenv("SMS_DKGL_SNAP_TEX") ? GLuint(atoi(getenv("SMS_DKGL_SNAP_TEX"))) : 0;
                Texture* src = snapTex ? g.textures.get(snapTex) : g.targetColor;
                if (!src || !src->realized) continue;
                for (size_t n = 0; n < g.textures.objs.size(); n++)
                    if (g.textures.objs[n].get() == g.targetColor) logf("snapshot %zu: the target is texture %zu", k, n);
                const uint32_t w = src->levels[0].w, h = src->levels[0].h;
                if (!snaps[k]) {
                    snaps[k] = new Texture();
                    defineLevel(snaps[k], 0, GL_RGBA8, w, h);
                }
                if (!realize(snaps[k])) continue;
                barrierBeforeTransfer();
                DkImageView sv = viewOf(src, 0), dv = viewOf(snaps[k], 0);
                DkImageRect r = {0, 0, 0, w, h, 1};
                dkCmdBufCopyImage(g_dev.cmd, &sv, &r, &dv, &r, 0);
                g.transferred = true;
                taken |= 1 << k;
            }
            index++;
        } else if (taken && g_swapCount > from) {
            // the frame after: read them out
            for (size_t k = 0; k < snapAt.size(); k++) {
                if (!(taken & (1 << k))) continue;
                Texture* t = snaps[k];
                const uint32_t w = t->levels[0].w, h = t->levels[0].h;
                Alloc tmp = g_dev.heaps[HEAP_READBACK].alloc(w * h * 4, 256);
                if (!tmp) continue;
                barrierBeforeTransfer();
                DkImageView v = viewOf(t, 0);
                DkImageRect r = {0, 0, 0, w, h, 1};
                DkCopyBuf dst = {tmp.gpu, w * 4, 0};
                dkCmdBufCopyImageToBuffer(g_dev.cmd, &v, &r, &dst, 0);
                submit(true);
                dkMemBlockFlushCpuCache(tmp.block, tmp.offset, tmp.size);
                std::vector<uint8_t> c(tmp.cpu, tmp.cpu + size_t(w) * h * 4);
                g_dev.heaps[HEAP_READBACK].free(tmp);
                GXPC_WindowShotSink(100100 + uint32_t(k), c.data(), int(w), int(h));
                logf("snapshot after draw %d of frame %llu sent as capture %u", snapAt[k], (unsigned long long)from,
                     100100 + unsigned(k));
            }
            taken = 0;
        }
    }
    // (with SMS_DKGL_TRACE: at the traced frame's first draw, every texture's memory)
    {
        static bool listed = false;
        if (!listed && left > 0 && g_swapCount >= from && from) {
            listed = true;
            for (size_t n = 0; n < g.textures.objs.size(); n++) {
                const Texture* t = g.textures.objs[n].get();
                if (!t || !t->mem) continue;
                logf("memory: texture %zu %ux%u format 0x%x levels %u gpu 0x%llx-0x%llx", n,
                     t->levels.empty() ? 0 : t->levels[0].w, t->levels.empty() ? 0 : t->levels[0].h, t->internalFormat,
                     t->imageLevels, (unsigned long long)t->mem.gpu, (unsigned long long)(t->mem.gpu + t->mem.size));
            }
            for (size_t n = 0; n < g.buffers.objs.size(); n++) {
                const Buffer* b = g.buffers.objs[n].get();
                if (!b || !b->mem) continue;
                logf("memory: buffer %zu gpu 0x%llx-0x%llx heap %d", n, (unsigned long long)b->mem.gpu,
                     (unsigned long long)(b->mem.gpu + b->mem.size), b->mem.heap);
            }
        }
    }
    g_tracing = left > 0 && g_swapCount >= from;
    if (left <= 0 || g_swapCount < from) return;
    if (skip > 0) {
        skip--;
        return;
    }
    left--;
    g_tracing = left > 0;
    tracePixels();
    const Program* p = g.programs.get(g.program);
    logf("draw %s: mode 0x%x count %d first %u base %d indices 0x%llx; fbo %u (%dx%d) viewport %d %d %d %d "
         "scissor %s %d %d %d %d; depth %d func 0x%x mask %d; cull %d 0x%x front 0x%x; blend %d; colormask %d%d%d%d",
         what, mode, count, first, base, (unsigned long long)indices, g.drawFbo, g.targetW, g.targetH,
         g.viewport[0], g.viewport[1], g.viewport[2], g.viewport[3], g.scissorTest ? "on" : "off", g.scissor[0],
         g.scissor[1], g.scissor[2], g.scissor[3], g.depthTest, g.depthFunc, g.depthMask, g.cullFace, g.cullMode,
         g.frontFace, g.blend, g.colorMask[0], g.colorMask[1], g.colorMask[2], g.colorMask[3]);
    logf("  frame %llu; blend eq 0x%x 0x%x func 0x%x 0x%x 0x%x 0x%x const %g %g %g %g; logic %d 0x%x",
         (unsigned long long)g_swapCount, g.blendEq[0], g.blendEq[1], g.blendFunc[0], g.blendFunc[1], g.blendFunc[2],
         g.blendFunc[3], g.blendColor[0], g.blendColor[1], g.blendColor[2], g.blendColor[3], g.logicOp, g.logicOpMode);
    if (!p) return;
    logf("  program %u (key %016llx): attribs 0x%x, %d samplers, %zu blocks, default block %u bytes, gs %d", g.program,
         (unsigned long long)p->key, p->attribMask, p->samplerCount, p->glsl.blocks.size(), p->glsl.defaultBlockSize,
         p->hasGs);
    // SMS_DKGL_TRACE_SRC=1: each traced program's adapted GLSL, once
    static const bool src = getenv("SMS_DKGL_TRACE_SRC") && atoi(getenv("SMS_DKGL_TRACE_SRC"));
    if (src && !const_cast<Program*>(p)->traced) {
        const_cast<Program*>(p)->traced = true;
        fprintf(stderr, "[dkgl] dump %016llx.vs\n%s\n[dkgl] dump end\n[dkgl] dump %016llx.fs\n%s\n[dkgl] dump end\n",
                (unsigned long long)p->key, p->glsl.vs.c_str(), (unsigned long long)p->key, p->glsl.fs.c_str());
    }
    const VertexArray* va = currentVao();
    for (int loc = 0; loc < 16; loc++) {
        if (!(p->attribMask & (1u << loc))) continue;
        const VertexAttrib& a = va->attribs[loc];
        const Buffer* b = g.buffers.get(a.buffer);
        if (a.enabled && b && b->mem) {
            float f[3] = {0, 0, 0};
            const uint32_t at = uint32_t(a.offset) + uint32_t(std::max(base, 0)) * uint32_t(a.stride);
            if (a.type == GL_FLOAT && at + 12 <= b->size) memcpy(f, b->mem.cpu + at, 12);
            // and the raw bytes of the attribute in vertices base .. base+3
            char raw[160] = "";
            const uint32_t abytes = a.type == GL_FLOAT ? 4u * uint32_t(a.size) : a.type == GL_UNSIGNED_SHORT || a.type == GL_SHORT ? 2u * uint32_t(a.size) : uint32_t(a.size);
            for (int v = 0, o = 0; v < 4; v++) {
                const uint32_t vat = at + uint32_t(v) * uint32_t(a.stride);
                if (vat + abytes > b->size || abytes > 16) break;
                o += snprintf(raw + o, sizeof raw - size_t(o), " [");
                for (uint32_t k = 0; k < abytes; k++) o += snprintf(raw + o, sizeof raw - size_t(o), "%02x", b->mem.cpu[vat + k]);
                o += snprintf(raw + o, sizeof raw - size_t(o), "]");
            }
            logf("  attrib %d: buffer %u at 0x%llx + %llu stride %d size %d type 0x%x%s%s; vertex %d: %g %g %g;%s", loc,
                 a.buffer, (unsigned long long)b->mem.gpu, (unsigned long long)a.offset, a.stride, a.size, a.type,
                 a.normalized ? " norm" : "", a.integer ? " int" : "", base, f[0], f[1], f[2], raw);
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
        // its contents: a hash, and the words that are not zero
        if (b && b->mem && b->mem.cpu && ub.offset + ub.size <= GLintptr(b->size)) {
            const uint8_t* d = b->mem.cpu + ub.offset;
            uint64_t h = 1469598103934665603ull;
            for (GLsizeiptr k = 0; k < ub.size; k++) h = (h ^ d[k]) * 1099511628211ull;
            std::string nz;
            char tmp[32];
            int shown = 0;
            for (GLsizeiptr k = 0; k + 4 <= ub.size && shown < 48; k += 4) {
                uint32_t w;
                memcpy(&w, d + k, 4);
                if (!w) continue;
                snprintf(tmp, sizeof tmp, " %lld:%08x", (long long)k, w);
                nz += tmp;
                shown++;
            }
            logf("  block %zu contents hash %016llx;%s", i, (unsigned long long)h, nz.c_str());
        }
    }
    for (const GlslUniform& u : p->glsl.uniforms) {
        if (u.binding >= 0) {
            for (int e = 0; e < std::max(u.arraySize, 1) && size_t(u.binding + e) < p->samplerUnits.size(); e++) {
                const int unit = p->samplerUnits[size_t(u.binding + e)];
                const GLuint name = unit >= 0 && unit < 32 ? g.tex2D[unit] : 0;
                const Texture* t = g.textures.get(name);
                if (t) {
                    const SamplerParams* sp = g.samplerObj[unit] ? g.samplers.get(g.samplerObj[unit]) : nullptr;
                    const SamplerParams& sm = sp ? *sp : t->sampler;
                    logf("    (defined levels %zu, base %d max %d; min 0x%x mag 0x%x lod %g..%g bias %g aniso %g%s)",
                         t->levels.size(), t->baseLevel, t->maxLevel, sm.minFilter, sm.magFilter, sm.minLod, sm.maxLod,
                         sm.lodBias, sm.maxAniso, sp ? ", sampler object" : "");
                }
                logf("  sampler %s[%d]: unit %d texture %u %ux%u format 0x%x levels %u handle image %u sampler %u; gpu 0x%llx-0x%llx",
                     u.name.c_str(), e, unit, name, t && !t->levels.empty() ? t->levels[0].w : 0,
                     t && !t->levels.empty() ? t->levels[0].h : 0, t ? t->internalFormat : 0, t ? t->imageLevels : 0,
                     t && t->handleValid ? t->handle & 0xFFFFF : ~0u, t && t->handleValid ? t->handle >> 20 : ~0u,
                     (unsigned long long)(t && t->mem ? t->mem.gpu : 0), (unsigned long long)(t && t->mem ? t->mem.gpu + t->mem.size : 0));
            }
            continue;
        }
        // each element of an array (up to 4), at its std140 stride
        const int n = std::min(std::max(u.arraySize, 1), 4);
        for (int e = 0; e < n; e++) {
            const uint32_t at = u.offset + uint32_t(e) * (u.stride ? u.stride : 16);
            uint32_t w[4] = {0, 0, 0, 0};
            if (at + 16 <= p->defaultBlock.size()) memcpy(w, &p->defaultBlock[at], 16);
            logf("  uniform %s[%d] @%u: %08x %08x %08x %08x", u.name.c_str(), e, at, w[0], w[1], w[2], w[3]);
        }
    }
}


// SMS_DKGL_CHECK_INPUTS=1 (a test): what each draw reads from memory the CPU
// writes (its indices, the vertices they reach, its uniform blocks, its
// texture and sampler descriptors) is hashed when the draw is recorded and
// again when its list is submitted; a draw whose inputs changed in between
// read what a later draw wrote, not its own, and goes in the log.
// SMS_DKGL_CHECK_FROM=frame: from that frame on.
namespace {
struct CheckedRange {
    uint32_t draw;
    uint64_t frame;
    const uint8_t* cpu;
    uint32_t size;
    uint64_t hash;
    const char* what;
    uint64_t gpu;
};
std::vector<CheckedRange> s_checked;
uint32_t s_checkDraw = 0;
uint64_t s_checkFrame = ~0ull;
int s_checkReports = 0;

uint64_t hashBytes(const uint8_t* p, uint32_t n) {
    uint64_t h = 1469598103934665603ull;
    uint32_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t v;
        memcpy(&v, p + i, 8);
        h = (h ^ v) * 1099511628211ull;
    }
    for (; i < n; i++) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

bool checkOn() {
    static int on = -1;
    static uint64_t from = 0;
    if (on < 0) {
        on = getenv("SMS_DKGL_CHECK_INPUTS") && atoi(getenv("SMS_DKGL_CHECK_INPUTS"));
        if (const char* f = getenv("SMS_DKGL_CHECK_FROM")) from = strtoull(f, nullptr, 10);
    }
    return on && g_swapCount >= from && s_checkReports < 400;
}

void checkRange(const char* what, const uint8_t* cpu, uint64_t gpu, uint64_t size) {
    if (!cpu || !size) return;
    if (size > (16u << 20)) size = 16u << 20;
    s_checked.push_back({s_checkDraw, g_swapCount, cpu, uint32_t(size), hashBytes(cpu, uint32_t(size)), what, gpu});
}
}  // namespace

void checkDrawInputs(GLenum idxType, const void* indices, GLsizei count, GLint first, GLint baseVertex) {
    if (!checkOn()) return;
    if (s_checkFrame != g_swapCount) s_checkFrame = g_swapCount, s_checkDraw = 0;
    const Program* p = g.programs.get(g.program);
    const VertexArray* va = currentVao();
    // the vertices the draw reaches
    int64_t lo = first, hi = int64_t(first) + count - 1;
    if (idxType) {
        const Buffer* ib = g.buffers.get(va->elementBuffer);
        if (ib && ib->mem) {
            const uint32_t size = idxType == GL_UNSIGNED_INT ? 4 : idxType == GL_UNSIGNED_SHORT ? 2 : 1;
            const uintptr_t off = reinterpret_cast<uintptr_t>(indices);
            const uint8_t* ip = ib->mem.cpu + off;
            if (off + uint64_t(count) * size <= ib->size) {
                checkRange("indices", ip, ib->mem.gpu + off, uint64_t(count) * size);
                lo = INT64_MAX, hi = INT64_MIN;
                for (GLsizei i = 0; i < count; i++) {
                    uint32_t v = size == 4 ? reinterpret_cast<const uint32_t*>(ip)[i]
                               : size == 2 ? reinterpret_cast<const uint16_t*>(ip)[i] : ip[i];
                    lo = std::min<int64_t>(lo, v), hi = std::max<int64_t>(hi, v);
                }
                lo += baseVertex, hi += baseVertex;
            }
        }
    }
    if (p && lo <= hi && lo >= 0) {
        for (int loc = 0; loc < 16; loc++) {
            if (!(p->attribMask & (1u << loc))) continue;
            const VertexAttrib& a = va->attribs[loc];
            const Buffer* b = a.enabled ? g.buffers.get(a.buffer) : nullptr;
            if (!b || !b->mem) continue;
            const uint64_t stride = a.stride ? uint64_t(a.stride) : 16;
            const uint64_t start = a.offset + uint64_t(lo) * stride, end = a.offset + uint64_t(hi) * stride + 16;
            if (end > b->size) continue;
            checkRange("vertices", b->mem.cpu + start, b->mem.gpu + start, end - start);
        }
    }
    if (p) {
        for (size_t i = 0; i < p->glsl.blocks.size(); i++) {
            const UboBinding& ub = g.ubo[p->blockBindings[i] & 15];
            const Buffer* b = g.buffers.get(ub.buffer);
            if (!b || !b->mem) continue;
            const uint64_t size = std::min<uint64_t>(ub.size ? uint64_t(ub.size) : b->size - ub.offset, 0x10000);
            checkRange("uniform block", b->mem.cpu + ub.offset, b->mem.gpu + ub.offset, size);
        }
        if (p->glsl.defaultBlockSize && s_defaultBound)
            checkRange("default block", s_defaultBound.cpu, s_defaultBound.gpu, p->glsl.defaultBlockSize);
        for (int k = 0; k < g.boundTexCount[1]; k++) {
            const DkResHandle h = g.boundTex[1][k];
            const uint32_t img = h & 0xFFFFF, smp = h >> 20;
            checkRange("image descriptor", g_dev.imageDescs.cpu + img * sizeof(DkImageDescriptor),
                       g_dev.imageDescs.gpu + img * sizeof(DkImageDescriptor), sizeof(DkImageDescriptor));
            checkRange("sampler descriptor", g_dev.samplerDescs.cpu + smp * sizeof(DkSamplerDescriptor),
                       g_dev.samplerDescs.gpu + smp * sizeof(DkSamplerDescriptor), sizeof(DkSamplerDescriptor));
        }
    }
    s_checkDraw++;
}

void checkInputsAtSubmit() {
    if (s_checked.empty()) return;
    static int summaries = 0;
    if (summaries < 12) {
        uint64_t bytes = 0;
        for (const CheckedRange& r : s_checked) bytes += r.size;
        summaries++;
        logf("check: frame %llu, %zu ranges of %u draws, %llu bytes", (unsigned long long)g_swapCount, s_checked.size(),
             s_checkDraw, (unsigned long long)bytes);
    }
    for (const CheckedRange& r : s_checked) {
        if (hashBytes(r.cpu, r.size) == r.hash) continue;
        if (s_checkReports++ >= 400) break;
        // where in the range it changed
        logf("check: frame %llu draw %u: its %s (%u bytes at gpu 0x%llx) changed before the GPU read them",
             (unsigned long long)r.frame, r.draw, r.what, r.size, (unsigned long long)r.gpu);
    }
    s_checked.clear();
}

}  // namespace dkgl
