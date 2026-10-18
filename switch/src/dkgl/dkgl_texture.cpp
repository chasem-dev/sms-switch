// dkgl's textures: formats, images made at first use, uploads, descriptors,
// and the barriers between rendering, copies and sampling.
#include "dkgl_state.h"

#include <algorithm>

namespace dkgl {

namespace {

struct FormatInfo {
    DkImageFormat format;
    uint32_t bytes;  // per pixel, or per 4x4 block
    bool compressed, depth;
};

FormatInfo formatInfo(GLenum internalFormat) {
    switch (internalFormat) {
    case GL_RGBA:
    case GL_RGBA8: return {DkImageFormat_RGBA8_Unorm, 4, false, false};
    case GL_RGB:
    case GL_RGB8: return {DkImageFormat_RGBX8_Unorm, 4, false, false};
    case GL_RGBA32F: return {DkImageFormat_RGBA32_Float, 16, false, false};
    case GL_RGBA16F: return {DkImageFormat_RGBA16_Float, 8, false, false};
    case GL_R8:
    case GL_RED: return {DkImageFormat_R8_Unorm, 1, false, false};
    case GL_RG8: return {DkImageFormat_RG8_Unorm, 2, false, false};
    case GL_DEPTH_COMPONENT:
    case GL_DEPTH_COMPONENT16:
    case GL_DEPTH_COMPONENT24:
    case GL_DEPTH24_STENCIL8:
    case GL_DEPTH_STENCIL: return {DkImageFormat_Z24S8, 4, false, true};
    case GL_DEPTH_COMPONENT32F: return {DkImageFormat_ZF32, 4, false, true};
    case 0x83F0: return {DkImageFormat_RGB_BC1, 8, true, false};   // GL_COMPRESSED_RGB_S3TC_DXT1_EXT
    case 0x83F1: return {DkImageFormat_RGBA_BC1, 8, true, false};  // GL_COMPRESSED_RGBA_S3TC_DXT1_EXT
    case 0x83F2: return {DkImageFormat_RGBA_BC2, 16, true, false}; // DXT3
    case 0x83F3: return {DkImageFormat_RGBA_BC3, 16, true, false}; // DXT5
    case GL_COMPRESSED_RGBA_BPTC_UNORM: return {DkImageFormat_RGBA_BC7_Unorm, 16, true, false};
    default: return {DkImageFormat_None, 0, false, false};
    }
}

DkImageSwizzle swizzleOf(GLint s) {
    switch (s) {
    case GL_RED: return DkImageSwizzle_Red;
    case GL_GREEN: return DkImageSwizzle_Green;
    case GL_BLUE: return DkImageSwizzle_Blue;
    case GL_ALPHA: return DkImageSwizzle_Alpha;
    case GL_ZERO: return DkImageSwizzle_Zero;
    default: return DkImageSwizzle_One;
    }
}

DkWrapMode wrapOf(GLenum w) {
    switch (w) {
    case GL_CLAMP_TO_EDGE: return DkWrapMode_ClampToEdge;
    case GL_MIRRORED_REPEAT: return DkWrapMode_MirroredRepeat;
    case GL_CLAMP_TO_BORDER: return DkWrapMode_ClampToBorder;
    default: return DkWrapMode_Repeat;
    }
}

uint32_t levelBytes(const FormatInfo& f, uint32_t w, uint32_t h) {
    if (f.compressed) return ((w + 3) / 4) * ((h + 3) / 4) * f.bytes;
    return w * h * f.bytes;
}

// Records a copy of `data` into level `level` of a realized texture.
void copyIn(Texture* t, int level, int x, int y, uint32_t w, uint32_t h, const void* data, uint32_t rowBytes,
            uint32_t rows) {
    const uint32_t bytes = rowBytes * rows;
    Alloc staging = transient(bytes, 0x100);
    memcpy(staging.cpu, data, bytes);
    barrierBeforeTransfer();
    DkImageView view = viewOf(t, level);
    DkCopyBuf src = {staging.gpu, rowBytes, 0};
    DkImageRect rect = {uint32_t(x), uint32_t(y), 0, w, h, 1};
    dkCmdBufCopyBufferToImage(g_dev.cmd, &src, &view, &rect, 0);
    g.transferred = true;
}

}  // namespace

// A copy (the copy engine) reads what draws wrote only once they are complete:
// the Fragments barrier a change of render targets makes only orders the 3D
// pipeline's own work, so it does not count (an EFB copy after the targets
// changed read half-drawn pixels: the plaza's ground came out in solid
// shadow colour on the console).
void barrierBeforeTransfer() {
    if (g.unfinished) {
        dkCmdBufBarrier(g_dev.cmd, DkBarrier_Primitives, 0);
        g.unfinished = false;
        g.rendered = false;
    }
}

void barrierBeforeDraw() {
    uint32_t inval = 0;
    DkBarrier mode = DkBarrier_None;
    // (SMS_DKGL_INVAL_EVERY=1: a full barrier and every cache invalidated
    // before each draw, a test; =2 the texture cache only, waiting for idle)
    static const int every = getenv("SMS_DKGL_INVAL_EVERY") ? atoi(getenv("SMS_DKGL_INVAL_EVERY")) : 0;
    if (every == 1) {
        mode = DkBarrier_Full;
        inval |= DkInvalidateFlags_Image | DkInvalidateFlags_Shader | DkInvalidateFlags_Descriptors | DkInvalidateFlags_L2Cache;
    } else if (every == 2) {
        mode = DkBarrier_Fragments;  // with DkInvalidateFlags_Image: the waiting texture cache invalidate
        inval |= DkInvalidateFlags_Image;
    }
    if (g.transferred) {
        // copies wrote images and buffers: the texture cache and the
        // shaders' data (uniform) cache may hold what was there before
        mode = DkBarrier_Full;
        g.unfinished = false;  // a full barrier completes the draws too
        inval |= DkInvalidateFlags_Image | DkInvalidateFlags_Shader;
        g.transferred = false;
    }
    if (g_dev.descriptorsDirty) {
        // (SMS_DKGL_DESC_L2=1: and the GPU's L2, a test)
        static const bool l2 = getenv("SMS_DKGL_DESC_L2") && atoi(getenv("SMS_DKGL_DESC_L2"));
        inval |= DkInvalidateFlags_Descriptors | (l2 ? DkInvalidateFlags_L2Cache : 0);
        g_dev.descriptorsDirty = false;
        // (SMS_DKGL_DESC_REBIND=1: the descriptor sets bound again; =2 with a
        // wait for idle first; tests)
        static const int rebind = getenv("SMS_DKGL_DESC_REBIND") ? atoi(getenv("SMS_DKGL_DESC_REBIND")) : 0;
        if (rebind == 2 && mode == DkBarrier_None) mode = DkBarrier_Primitives;
        if (rebind == 1) {
            dkCmdBufBindImageDescriptorSet(g_dev.cmd, g_dev.imageDescs.gpu, 8192);
            dkCmdBufBindSamplerDescriptorSet(g_dev.cmd, g_dev.samplerDescs.gpu, 2048);
        }
    }
    // (SMS_DKGL_SAMPLER_REBIND=1: the sampler set bound again after a new
    // sampler descriptor, a test)
    static const bool samplerRebind = getenv("SMS_DKGL_SAMPLER_REBIND") && atoi(getenv("SMS_DKGL_SAMPLER_REBIND"));
    if (g_dev.samplersAdded) {
        g_dev.samplersAdded = false;
        if (samplerRebind) dkCmdBufBindSamplerDescriptorSet(g_dev.cmd, g_dev.samplerDescs.gpu, 2048);
    }
    if (mode != DkBarrier_None || inval) dkCmdBufBarrier(g_dev.cmd, mode, inval);
}

void defineLevel(Texture* t, int level, GLenum internalFormat, uint32_t w, uint32_t h) {
    const FormatInfo f = formatInfo(internalFormat);
    if (f.format == DkImageFormat_None) {
        logf("texture format 0x%x is not supported", internalFormat);
        g.error = GL_INVALID_ENUM;
        return;
    }
    if (level < 0 || level > 15) return;
    if (size_t(level) >= t->levels.size()) t->levels.resize(size_t(level) + 1);
    // a new level 0 size or format: a new image (the old one is freed once
    // the GPU is done with it)
    const bool reshape = level == 0 && (t->levels[0].w != w || t->levels[0].h != h || t->format != f.format);
    const bool grows = t->realized && uint32_t(level) >= t->imageLevels;
    if (t->realized && (reshape || grows)) {
        if (grows && !reshape) {
            // keep the levels' data: the image is made again with more
            // levels and the old ones copied over (rare: a mip chain given
            // after the texture was first drawn)
            Texture old = *t;
            old.levels.clear();
            t->realized = false;
            t->levels[size_t(level)] = {w, h, true, {}, false};
            t->internalFormat = internalFormat;
            if (realize(t)) {
                barrierBeforeTransfer();
                for (uint32_t l = 0; l < old.imageLevels && l < t->imageLevels; l++) {
                    DkImageView sv = viewOf(&old, int(l)), dv = viewOf(t, int(l));
                    const uint32_t lw = std::max(1u, t->levels[0].w >> l), lh = std::max(1u, t->levels[0].h >> l);
                    DkImageRect r = {0, 0, 0, lw, lh, 1};
                    dkCmdBufCopyImage(g_dev.cmd, &sv, &r, &dv, &r, 0);
                }
                g.transferred = true;
            }
            retire(old.mem);
            return;
        }
        retire(t->mem);
        t->mem = Alloc();
        t->realized = false;
        t->descDirty = true;
        for (size_t l = 1; l < t->levels.size(); l++) t->levels[l].defined = false;
    }
    t->internalFormat = internalFormat;
    t->format = f.format;
    t->compressed = f.compressed;
    t->depth = f.depth;
    Texture::Level& L = t->levels[size_t(level)];
    L.w = w;
    L.h = h;
    L.defined = true;
    L.pending.clear();
    t->descDirty = true;
}

bool realize(Texture* t) {
    if (t->realized) return true;
    if (t->levels.empty() || !t->levels[0].defined) return false;
    const FormatInfo f = formatInfo(t->internalFormat);
    uint32_t count = 1;
    const uint32_t w0 = t->levels[0].w, h0 = t->levels[0].h;
    // every level defined so far, whatever GL_TEXTURE_MAX_LEVEL is now: that
    // limits sampling (the image view), not storage (the HD texture packs
    // raise it once their levels have arrived, and levels past the image's
    // were dropped, the textures sampled at full size from any distance)
    while (count < t->levels.size() && t->levels[count].defined &&
           t->levels[count].w == std::max(1u, w0 >> count) && t->levels[count].h == std::max(1u, h0 >> count))
        count++;
    DkImageLayoutMaker lm;
    dkImageLayoutMakerDefaults(&lm, g_dev.dev);
    lm.format = f.format;
    lm.dimensions[0] = w0;
    lm.dimensions[1] = h0;
    lm.mipLevels = count;
    if (t->samples > 1) {
        lm.type = DkImageType_2DMS;
        lm.msMode = t->samples >= 8 ? DkMsMode_8x : t->samples >= 4 ? DkMsMode_4x : DkMsMode_2x;
    }
    // (depth too may go through the 2D engine: a scaled depth blit)
    lm.flags = f.compressed ? 0 : DkImageFlags_UsageRender | DkImageFlags_Usage2DEngine;
    // Render targets the size of the EFB and up are compressed by the GPU's
    // memory controller: at 2x the plaza's GPU time went from 11.3 to 9.5 ms,
    // and the game's thread, sharing the memory, ran 1.5 ms faster.
    // SMS_DKGL_HWCOMP=0: never.
    static const bool hwComp = !getenv("SMS_DKGL_HWCOMP") || getenv("SMS_DKGL_HWCOMP")[0] != '0';
    t->hwCompressed = hwComp && t->renderTarget && !f.compressed && count == 1 && w0 * h0 >= 512u * 512u;
    if (t->hwCompressed) lm.flags |= DkImageFlags_HwCompression;
    dkImageLayoutInitialize(&t->layout, &lm);
    t->mem = g_dev.heaps[HEAP_IMAGE].alloc(uint32_t(dkImageLayoutGetSize(&t->layout)), dkImageLayoutGetAlignment(&t->layout));
    if (!t->mem) {
        logf("out of image memory for a %ux%u texture (%llu MiB used)", w0, h0,
             (unsigned long long)(g_dev.heaps[HEAP_IMAGE].used() >> 20));
        return false;
    }
    dkImageInitialize(&t->image, &t->layout, t->mem.block, t->mem.offset);
    t->imageLevels = count;
    t->realized = true;
    t->descDirty = true;
    // the data given before the image existed
    for (uint32_t l = 0; l < count; l++) {
        Texture::Level& L = t->levels[l];
        if (L.pending.empty()) continue;
        const uint32_t rowBytes = f.compressed ? ((L.w + 3) / 4) * f.bytes : L.w * f.bytes;
        const uint32_t rows = f.compressed ? (L.h + 3) / 4 : L.h;
        copyIn(t, int(l), 0, 0, L.w, L.h, L.pending.data(), rowBytes, rows);
        std::vector<uint8_t>().swap(L.pending);
    }
    return true;
}

DkImageView viewOf(Texture* t, int level) {
    DkImageView v;
    dkImageViewDefaults(&v, &t->image);
    if (level > 0 || t->imageLevels > 1) {
        v.mipLevelOffset = uint8_t(level);
        v.mipLevelCount = 1;
    }
    return v;
}

void uploadLevel(Texture* t, int level, int x, int y, uint32_t w, uint32_t h, const void* data, uint32_t /*bytes*/,
                 uint32_t rowLength, bool compressed) {
    if (!data || size_t(level) >= t->levels.size() || !t->levels[size_t(level)].defined) return;
    const FormatInfo f = formatInfo(t->internalFormat);
    Texture::Level& L = t->levels[size_t(level)];
    const uint32_t blockW = compressed ? 4 : 1;
    const uint32_t srcRowPixels = rowLength ? rowLength : w;
    const uint32_t rowBytes = compressed ? ((w + 3) / 4) * f.bytes : w * f.bytes;
    const uint32_t srcRowBytes = compressed ? ((srcRowPixels + 3) / 4) * f.bytes : srcRowPixels * f.bytes;
    const uint32_t rows = compressed ? (h + 3) / 4 : h;
    (void)blockW;
    // GL_UNPACK_ALIGNMENT: rows start at its multiples
    const uint32_t align = uint32_t(std::max(1, g.unpackAlign));
    const uint32_t srcStride = compressed ? srcRowBytes : (srcRowBytes + align - 1) / align * align;
    const uint8_t* src = static_cast<const uint8_t*>(data);
    std::vector<uint8_t> packed;
    if (srcStride != rowBytes) {
        packed.resize(size_t(rowBytes) * rows);
        for (uint32_t r = 0; r < rows; r++) memcpy(&packed[size_t(r) * rowBytes], src + size_t(r) * srcStride, rowBytes);
        src = packed.data();
    }
    if (!t->realized && x == 0 && y == 0 && w == L.w && h == L.h) {
        L.pending.assign(src, src + size_t(rowBytes) * rows);
        L.pendingCompressed = compressed;
        return;
    }
    if (!realize(t) || uint32_t(level) >= t->imageLevels) return;
    copyIn(t, level, x, y, w, h, src, rowBytes, rows);
}

DkResHandle textureHandle(Texture* t, const SamplerParams& s) {
    if (t->handleValid && t->realized && !t->descDirty && !memcmp(&s, &t->handleParams, sizeof s)) return t->handle;
    if (!realize(t)) return 0;
    if (t->descDirty || t->desc == ~0u) {
        retireImageDescriptor(t->desc);
        t->desc = newImageDescriptor();
        DkImageView v;
        dkImageViewDefaults(&v, &t->image);
        for (int i = 0; i < 4; i++) v.swizzle[i] = swizzleOf(t->swizzle[i]);
        const int base = std::min(std::max(t->baseLevel, 0), int(t->imageLevels) - 1);
        const int last = std::min(t->maxLevel, int(t->imageLevels) - 1);
        v.mipLevelOffset = uint8_t(base);
        v.mipLevelCount = uint8_t(std::max(1, last - base + 1));
        if (t->depth) v.dsSource = DkDsSource_Depth;
        DkImageDescriptor d;
        dkImageDescriptorInitialize(&d, &v, false, false);
        writeImageDescriptor(t->desc, d);
        t->descDirty = false;
    }
    // zeroed first: samplerDescriptor knows samplers by all their bytes,
    // padding included, which dkSamplerDefaults leaves as it found it (built
    // with -O3, every texture bound made a new sampler until none were left)
    DkSampler ds;
    memset(&ds, 0, sizeof ds);
    dkSamplerDefaults(&ds);
    switch (s.minFilter) {
    case GL_NEAREST: ds.minFilter = DkFilter_Nearest; ds.mipFilter = DkMipFilter_None; break;
    case GL_LINEAR: ds.minFilter = DkFilter_Linear; ds.mipFilter = DkMipFilter_None; break;
    case GL_NEAREST_MIPMAP_NEAREST: ds.minFilter = DkFilter_Nearest; ds.mipFilter = DkMipFilter_Nearest; break;
    case GL_LINEAR_MIPMAP_NEAREST: ds.minFilter = DkFilter_Linear; ds.mipFilter = DkMipFilter_Nearest; break;
    case GL_NEAREST_MIPMAP_LINEAR: ds.minFilter = DkFilter_Nearest; ds.mipFilter = DkMipFilter_Linear; break;
    default: ds.minFilter = DkFilter_Linear; ds.mipFilter = DkMipFilter_Linear; break;
    }
    if (t->imageLevels <= 1) ds.mipFilter = DkMipFilter_None;
    ds.magFilter = s.magFilter == GL_NEAREST ? DkFilter_Nearest : DkFilter_Linear;
    ds.wrapMode[0] = wrapOf(s.wrapS);
    ds.wrapMode[1] = wrapOf(s.wrapT);
    ds.wrapMode[2] = wrapOf(s.wrapR);
    ds.lodClampMin = std::max(0.0f, s.minLod);
    ds.lodClampMax = std::min(s.maxLod, 15.0f);
    ds.lodBias = s.lodBias;
    ds.maxAnisotropy = std::max(1.0f, std::min(s.maxAniso, 16.0f));
    t->handle = dkMakeTextureHandle(t->desc, samplerDescriptor(ds));
    t->handleParams = s;
    t->handleValid = true;
    return t->handle;
}

Texture* attachmentTexture(const Attachment& a) {
    return a.texture ? g.textures.get(a.texture) : nullptr;
}

void destroyTexture(Texture* t) {
    if (!t) return;
    retire(t->mem);
    retireImageDescriptor(t->desc);
    if (g.targetColor == t || g.targetDepth == t) {
        g.targetColor = g.targetDepth = nullptr;
        g.dirty |= D_TARGETS;
    }
}

}  // namespace dkgl
