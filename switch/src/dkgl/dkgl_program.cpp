// dkgl's programs: the GLSL adapter, deko3d's compiler (uam), and the cache
// of compiled programs.
#include "dkgl_state.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <uam_api.h>

namespace dkgl {

namespace {

// --- the cache of compiled programs --------------------------------------------
// A program's two DKSH modules, by a hash of the adapted GLSL (and the
// compiler's version): from a pack built ahead of time (every program the game
// is known to draw with, tools/dkgl/make-pack) and from the SD card, where
// programs compiled here are written.
const char kPackMagic[8] = {'D', 'K', 'G', 'L', 'P', 'K', '0', '1'};
std::string s_cacheDir;
// key -> [u32 vertex, geometry, fragment sizes][the three DKSH modules]
std::unordered_map<uint64_t, std::string> s_cached;
const size_t kRecHead = 12;
uint32_t s_compiled = 0, s_fromCache = 0;
double s_compileSeconds = 0;

uint64_t fnv(const std::string& s, uint64_t h = 1469598103934665603ull) {
    for (unsigned char c : s) h = (h ^ c) * 1099511628211ull;
    return h;
}

uint64_t keyOf(const GlslProgram& p) {
    return fnv(p.fs, fnv(p.gs + '\x02', fnv("uam 1.1.0 dkgl 3\n" + p.vs + '\x01')));
}

uint64_t recordSize(const std::string& rec) {
    if (rec.size() < kRecHead) return 0;
    uint32_t sizes[3];
    memcpy(sizes, rec.data(), kRecHead);
    return kRecHead + uint64_t(sizes[0]) + sizes[1] + sizes[2];
}

double now() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return double(ts.tv_sec) + double(ts.tv_nsec) * 1e-9;
}

void loadPack(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return;
    char magic[8];
    uint32_t count = 0;
    if (fread(magic, 8, 1, f) != 1 || memcmp(magic, kPackMagic, 8) != 0 || fread(&count, 4, 1, f) != 1) {
        fclose(f);
        logf("%s is not a program pack", path);
        return;
    }
    uint32_t loaded = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint64_t key;
        uint32_t sizes[3];
        if (fread(&key, 8, 1, f) != 1 || fread(sizes, kRecHead, 1, f) != 1) break;
        std::string rec(kRecHead + size_t(sizes[0]) + sizes[1] + sizes[2], '\0');
        memcpy(&rec[0], sizes, kRecHead);
        if (fread(&rec[kRecHead], rec.size() - kRecHead, 1, f) != 1) break;
        s_cached.emplace(key, std::move(rec));
        loaded++;
    }
    fclose(f);
    logf("program pack: %u programs (%s)", loaded, path);
}

std::string cachePath(uint64_t key) {
    char name[32];
    snprintf(name, sizeof name, "/%016llx.dk", (unsigned long long)key);
    return s_cacheDir + name;
}

// SMS_DKGL_NO_CACHE=1: every program compiled here, none taken from the pack
// or the cache (what the compiler makes now, for tests); still stored.
bool cacheLookup(uint64_t key, std::string* rec) {
    static const bool off = getenv("SMS_DKGL_NO_CACHE") && atoi(getenv("SMS_DKGL_NO_CACHE"));
    if (off) return false;
    auto it = s_cached.find(key);
    if (it != s_cached.end()) {
        *rec = it->second;
        return true;
    }
    if (s_cacheDir.empty()) return false;
    FILE* f = fopen(cachePath(key).c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    rewind(f);
    rec->resize(size_t(n > 0 ? n : 0));
    const bool ok = n > long(kRecHead) && fread(&(*rec)[0], size_t(n), 1, f) == 1;
    fclose(f);
    return ok && recordSize(*rec) == rec->size();
}

void cacheStore(uint64_t key, const std::string& rec) {
    s_cached[key] = rec;
    if (s_cacheDir.empty()) return;
    const std::string path = cachePath(key), part = path + ".part";
    FILE* f = fopen(part.c_str(), "wb");
    bool ok = f && fwrite(rec.data(), rec.size(), 1, f) == 1;
    if (f && fclose(f) != 0) ok = false;
    if (ok && rename(part.c_str(), path.c_str()) != 0) ok = false;
    if (!ok) {
        static bool told = false;
        if (!told) logf("the program cache cannot be written (%s: %s)", path.c_str(), strerror(errno));
        told = true;
        remove(part.c_str());
    }
}

// SMS_DKGL_DUMP=dir writes each program's adapted GLSL there (<key>.vs, .gs,
// .fs), for tools/dkgl/make-pack to compile ahead of time; with
// SMS_DKGL_DUMP_ONLY=1 programs are only written, not compiled (they then
// fail to link), so the shader warm-up writes all the known ones quickly.
// SMS_DKGL_DUMP=- puts them in the log instead, between "[dkgl] dump
// <key>.<stage>" and "[dkgl] dump end" lines (a run on the console, read back
// by tools/switch-dev.py).
void dumpProgram(uint64_t key, const GlslProgram& p) {
    static const char* dir = getenv("SMS_DKGL_DUMP");
    if (!dir || !*dir) return;
    const bool toLog = !strcmp(dir, "-");
    static bool made = toLog || (mkdir(dir, 0777), true);
    (void)made;
    const std::string* src[3] = {&p.vs, &p.gs, &p.fs};
    const char* ext[3] = {"vs", "gs", "fs"};
    for (int i = 0; i < 3; i++) {
        if (src[i]->empty()) continue;
        if (toLog) {
            fprintf(stderr, "[dkgl] dump %016llx.%s\n%s\n[dkgl] dump end\n", (unsigned long long)key, ext[i],
                    src[i]->c_str());
            continue;
        }
        char name[64];
        snprintf(name, sizeof name, "/%016llx.%s", (unsigned long long)key, ext[i]);
        if (FILE* f = fopen((std::string(dir) + name).c_str(), "wb")) {
            fwrite(src[i]->data(), src[i]->size(), 1, f);
            fclose(f);
        }
    }
}

// Makes the DkShader for one DKSH module in code memory.
// The clip distances a shader writes (gl_ClipDistance[n] = ...), one bit
// each: the GPU discards pixels by every enabled clip distance, and one the
// last stage does not write holds whatever an earlier program left there.
uint32_t clipDistanceMask(const std::string& src) {
    uint32_t mask = 0;
    for (size_t at = src.find("gl_ClipDistance["); at != std::string::npos; at = src.find("gl_ClipDistance[", at + 1)) {
        const size_t close = src.find(']', at);
        if (close == std::string::npos) break;
        const char* e = src.c_str() + close + 1;
        while (*e == ' ') e++;
        if (*e != '=' || e[1] == '=') continue;  // a write, not a read or comparison
        const int n = atoi(src.c_str() + at + 16);
        if (n >= 0 && n < 8) mask |= 1u << n;
    }
    return mask;
}

bool loadShader(const uint8_t* dksh, uint32_t size, DkShader* out, Alloc* code, uint32_t* codeUsed) {
    if (size < 24) return false;
    uint32_t hdr[6];
    memcpy(hdr, dksh, sizeof hdr);
    const uint32_t controlSize = hdr[2], codeSize = hdr[3];
    if (hdr[0] != 0x48534B44u || controlSize + codeSize > size) return false;
    // the most scratch (local) memory per warp a loaded shader needs, in the
    // log as it grows: the queue gives each warp a fixed amount
    {
        static uint32_t most = 0;
        uint32_t ph[6];
        if (hdr[4] + sizeof ph <= controlSize) {
            memcpy(ph, dksh + hdr[4], sizeof ph);
            if (ph[5] > most) {
                most = ph[5];
                logf("shader scratch: %u bytes per warp (%u registers)", ph[5], ph[2]);
            }
        }
    }
    const uint32_t at = (*codeUsed + DK_SHADER_CODE_ALIGNMENT - 1) & ~(DK_SHADER_CODE_ALIGNMENT - 1);
    if (at + codeSize > code->size) return false;
    memcpy(code->cpu + at, dksh + controlSize, codeSize);
    *codeUsed = at + codeSize;
    DkShaderMaker m;
    dkShaderMakerDefaults(&m, code->block, code->offset + at);
    m.control = dksh;
    m.programId = 0;
    dkShaderInitialize(out, &m);
    return dkShaderIsValid(out);
}

// switch_main.cpp names the cache from a constructor that runs before this
// file's (s_cacheDir, s_cached), so the names are kept in plain arrays until
// the first program is linked.
char s_cacheDirName[512], s_packName[512];
bool s_cacheReady = false;

void setUpCache() {
    if (s_cacheReady) return;
    s_cacheReady = true;
    s_cacheDir = s_cacheDirName;
    for (size_t at = 1; at <= s_cacheDir.size(); at++)  // with its parents
        if (at == s_cacheDir.size() || s_cacheDir[at] == '/') mkdir(s_cacheDir.substr(0, at).c_str(), 0777);
    if (s_packName[0]) loadPack(s_packName);
}

}  // namespace

void setShaderCacheImpl(const char* dir, const char* pack) {
    snprintf(s_cacheDirName, sizeof s_cacheDirName, "%s", dir ? dir : "");
    snprintf(s_packName, sizeof s_packName, "%s", pack ? pack : "");
    s_cacheReady = false;
}

bool linkProgram(Program* p) {
    destroyProgram(p);
    p->linked = false;
    p->infoLog.clear();
    std::string vs, gs, fs;
    for (GLuint s : p->shaders) {
        const Shader* sh = g.shaders.get(s);
        if (!sh) continue;
        (sh->type == GL_VERTEX_SHADER ? vs : sh->type == GL_GEOMETRY_SHADER ? gs : fs) = sh->source;
    }
    setUpCache();
    if (vs.empty() || fs.empty()) {
        p->infoLog = "dkgl: a program needs a vertex and a fragment shader";
        return false;
    }
    if (!adaptProgram(vs, gs, fs, p->attribBindings, p->fragBindings, &p->glsl)) {
        p->infoLog = "dkgl: " + p->glsl.error;
        logf("program not adapted: %s", p->glsl.error.c_str());
        return false;
    }
    p->hasGs = !p->glsl.gs.empty();
    p->clipMask = clipDistanceMask(p->hasGs ? p->glsl.gs : p->glsl.vs);
    const uint64_t key = keyOf(p->glsl);
    p->key = key;
    dumpProgram(key, p->glsl);
    static const bool dumpOnly = getenv("SMS_DKGL_DUMP_ONLY") && atoi(getenv("SMS_DKGL_DUMP_ONLY"));
    std::string rec;
    if (dumpOnly) {
        p->infoLog = "dkgl: SMS_DKGL_DUMP_ONLY";
        return false;
    } else if (cacheLookup(key, &rec)) {
        s_fromCache++;
    } else {
        const double t0 = now();
        const std::string* src[3] = {&p->glsl.vs, &p->glsl.gs, &p->glsl.fs};
        const uam_stage stage[3] = {UAM_STAGE_VERTEX, UAM_STAGE_GEOMETRY, UAM_STAGE_FRAGMENT};
        void* out[3] = {nullptr, nullptr, nullptr};
        size_t size[3] = {0, 0, 0};
        bool ok = true;
        for (int i = 0; i < 3 && ok; i++)
            if (!src[i]->empty()) ok = uam_compile_dksh(src[i]->c_str(), stage[i], &out[i], &size[i]) != 0;
        s_compileSeconds += now() - t0;
        if (!ok) {
            for (void* o : out) free(o);
            p->infoLog = "dkgl: the shader did not compile (the log has uam's messages)";
            logf("program did not compile; its GLSL:\n--- vertex\n%s\n--- geometry\n%s\n--- fragment\n%s",
                 p->glsl.vs.c_str(), p->glsl.gs.c_str(), p->glsl.fs.c_str());
            return false;
        }
        const uint32_t sizes[3] = {uint32_t(size[0]), uint32_t(size[1]), uint32_t(size[2])};
        rec.assign(reinterpret_cast<const char*>(sizes), kRecHead);
        for (int i = 0; i < 3; i++) {
            if (out[i]) rec.append(static_cast<const char*>(out[i]), size[i]);
            free(out[i]);
        }
        cacheStore(key, rec);
        if (++s_compiled % 50 == 0)
            logf("%u programs compiled (%.1f ms each), %u from the cache", s_compiled,
                 s_compileSeconds * 1000 / s_compiled, s_fromCache);
    }
    uint32_t sizes[3];
    memcpy(sizes, rec.data(), kRecHead);
    const uint8_t* bin[3];
    bin[0] = reinterpret_cast<const uint8_t*>(rec.data()) + kRecHead;
    bin[1] = bin[0] + sizes[0];
    bin[2] = bin[1] + sizes[1];
    uint32_t codeTotal = 0;
    for (int i = 0; i < 3; i++) {
        if (!sizes[i]) continue;
        uint32_t c;
        memcpy(&c, bin[i] + 12, 4);
        codeTotal += c + DK_SHADER_CODE_ALIGNMENT;
    }
    // (the last DK_SHADER_CODE_UNUSABLE_SIZE bytes of code memory are not for code)
    p->code = g_dev.heaps[HEAP_CODE].alloc(codeTotal + DK_SHADER_CODE_UNUSABLE_SIZE, DK_SHADER_CODE_ALIGNMENT);
    uint32_t used = 0;
    DkShader* shaders[3] = {&p->vs, &p->gs, &p->fs};
    bool loaded = bool(p->code);
    for (int i = 0; i < 3 && loaded; i++)
        if (sizes[i]) loaded = loadShader(bin[i], sizes[i], shaders[i], &p->code, &used);
    if (!loaded) {
        p->infoLog = "dkgl: the compiled program could not be loaded";
        logf("%s", p->infoLog.c_str());
        return false;
    }
    p->defaultBlock.assign(p->glsl.defaultBlockSize, 0);
    p->defaultDirty = true;
    p->samplerCount = 0;
    p->samplerUnits.assign(kMaxSamplerBindings, 0);
    memset(p->samplerStages, 0, sizeof p->samplerStages);
    for (const GlslUniform& u : p->glsl.uniforms) {
        if (u.binding < 0) continue;
        for (int i = 0; i < u.arraySize; i++) p->samplerStages[u.binding + i] = u.stages;
        p->samplerCount = std::max(p->samplerCount, u.binding + u.arraySize);
    }
    p->blockBindings.assign(p->glsl.blocks.size(), 0);
    p->attribMask = 0;
    for (const auto& kv : p->glsl.attribs) p->attribMask |= 1u << kv.second;
    p->linked = true;
    p->version++;
    return true;
}

void destroyProgram(Program* p) {
    if (p->code) {
        retire(p->code);
        p->code = Alloc();
    }
    if (g.boundProgram == p) g.boundProgram = nullptr;
}

}  // namespace dkgl
