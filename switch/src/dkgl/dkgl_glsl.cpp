// The GLSL adapter (dkgl_glsl.h). It reads only what the declarations at a
// shader's top level need: statements are split at the top level, the
// declarations among them rewritten, functions and everything else kept.
#include "dkgl_glsl.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <algorithm>
#include <set>

namespace dkgl {

namespace {

// GL type enums (glcorearb.h), so the adapter builds without GL headers.
enum : uint32_t {
    T_FLOAT = 0x1406, T_VEC2 = 0x8B50, T_VEC3 = 0x8B51, T_VEC4 = 0x8B52,
    T_INT = 0x1404, T_IVEC2 = 0x8B53, T_IVEC3 = 0x8B54, T_IVEC4 = 0x8B55,
    T_UINT = 0x1405, T_UVEC2 = 0x8DC6, T_UVEC3 = 0x8DC7, T_UVEC4 = 0x8DC8,
    T_BOOL = 0x8B56, T_BVEC2 = 0x8B57, T_BVEC3 = 0x8B58, T_BVEC4 = 0x8B59,
    T_MAT2 = 0x8B5A, T_MAT3 = 0x8B5B, T_MAT4 = 0x8B5C,
    T_SAMPLER_2D = 0x8B5E, T_SAMPLER_3D = 0x8B5F, T_SAMPLER_CUBE = 0x8B60, T_SAMPLER_2D_SHADOW = 0x8B62,
    T_SAMPLER_2D_ARRAY = 0x8DC1, T_SAMPLER_BUFFER = 0x8DC2, T_ISAMPLER_2D = 0x8DCA, T_USAMPLER_2D = 0x8DD2,
    T_SAMPLER_2D_MS = 0x9108,
};

struct TypeInfo {
    const char* name;
    uint32_t type;
    uint32_t align, size;  // std140 base alignment and size of one element
    int locations;         // as a varying or vertex input
    bool sampler;
};
const TypeInfo kTypes[] = {
    {"float", T_FLOAT, 4, 4, 1, false},      {"vec2", T_VEC2, 8, 8, 1, false},
    {"vec3", T_VEC3, 16, 12, 1, false},      {"vec4", T_VEC4, 16, 16, 1, false},
    {"int", T_INT, 4, 4, 1, false},          {"ivec2", T_IVEC2, 8, 8, 1, false},
    {"ivec3", T_IVEC3, 16, 12, 1, false},    {"ivec4", T_IVEC4, 16, 16, 1, false},
    {"uint", T_UINT, 4, 4, 1, false},        {"uvec2", T_UVEC2, 8, 8, 1, false},
    {"uvec3", T_UVEC3, 16, 12, 1, false},    {"uvec4", T_UVEC4, 16, 16, 1, false},
    {"bool", T_BOOL, 4, 4, 1, false},        {"bvec2", T_BVEC2, 8, 8, 1, false},
    {"bvec3", T_BVEC3, 16, 12, 1, false},    {"bvec4", T_BVEC4, 16, 16, 1, false},
    {"mat2", T_MAT2, 16, 32, 2, false},      {"mat3", T_MAT3, 16, 48, 3, false},
    {"mat4", T_MAT4, 16, 64, 4, false},
    {"sampler2D", T_SAMPLER_2D, 0, 0, 0, true},          {"sampler3D", T_SAMPLER_3D, 0, 0, 0, true},
    {"samplerCube", T_SAMPLER_CUBE, 0, 0, 0, true},      {"sampler2DShadow", T_SAMPLER_2D_SHADOW, 0, 0, 0, true},
    {"sampler2DArray", T_SAMPLER_2D_ARRAY, 0, 0, 0, true}, {"samplerBuffer", T_SAMPLER_BUFFER, 0, 0, 0, true},
    {"isampler2D", T_ISAMPLER_2D, 0, 0, 0, true},        {"usampler2D", T_USAMPLER_2D, 0, 0, 0, true},
    {"sampler2DMS", T_SAMPLER_2D_MS, 0, 0, 0, true},
};

const TypeInfo* findType(const std::string& name) {
    for (const TypeInfo& t : kTypes)
        if (name == t.name) return &t;
    return nullptr;
}

// A top-level piece of a shader.
enum Kind { K_TEXT, K_PREPROC, K_DECL, K_BLOCK, K_FUNCTION };
struct Segment {
    Kind kind;
    std::string text;
};

bool isIdent(char c) { return isalnum(uint8_t(c)) || c == '_'; }

// Splits a shader into whitespace and comments (K_TEXT), preprocessor lines,
// and top-level statements: declarations (up to their ';'), interface blocks
// and structs (up to the ';' after their '}'), and functions (up to their
// closing '}').
std::vector<Segment> split(const std::string& s) {
    std::vector<Segment> out;
    size_t i = 0, n = s.size();
    bool lineStart = true;
    while (i < n) {
        const char c = s[i];
        if (isspace(uint8_t(c))) {
            size_t j = i;
            while (j < n && isspace(uint8_t(s[j]))) {
                if (s[j] == '\n') lineStart = true;
                j++;
            }
            out.push_back({K_TEXT, s.substr(i, j - i)});
            i = j;
            continue;
        }
        if (c == '#' && lineStart) {
            size_t j = i;
            while (j < n && s[j] != '\n') j += (s[j] == '\\' && j + 1 < n) ? 2 : 1;
            out.push_back({K_PREPROC, s.substr(i, j - i)});
            i = j;
            continue;
        }
        lineStart = false;
        if (c == '/' && i + 1 < n && (s[i + 1] == '/' || s[i + 1] == '*')) {
            size_t j = s[i + 1] == '/' ? s.find('\n', i) : s.find("*/", i + 2);
            j = j == std::string::npos ? n : (s[i + 1] == '/' ? j : j + 2);
            out.push_back({K_TEXT, s.substr(i, j - i)});
            i = j;
            continue;
        }
        // a statement
        size_t j = i;
        int depth = 0;
        Kind kind = K_DECL;
        while (j < n) {
            const char d = s[j];
            if (d == '/' && j + 1 < n && s[j + 1] == '/') {
                j = s.find('\n', j);
                if (j == std::string::npos) j = n;
                continue;
            }
            if (d == '/' && j + 1 < n && s[j + 1] == '*') {
                size_t e = s.find("*/", j + 2);
                j = e == std::string::npos ? n : e + 2;
                continue;
            }
            if (d == '{') {
                if (depth == 0) {
                    // a function's head has a parameter list; a block's or
                    // struct's has parentheses only in its layout( )
                    std::string head = s.substr(i, j - i);
                    size_t at = head.find("layout");
                    if (at != std::string::npos) {
                        const size_t close = head.find(')', at);
                        head.erase(at, close == std::string::npos ? std::string::npos : close - at + 1);
                    }
                    kind = head.find('(') == std::string::npos ? K_BLOCK : K_FUNCTION;
                }
                depth++;
            } else if (d == '}') {
                depth--;
                if (depth == 0 && kind == K_FUNCTION) {
                    j++;
                    break;
                }
            } else if (d == ';' && depth == 0) {
                j++;
                break;
            }
            j++;
        }
        out.push_back({kind, s.substr(i, j - i)});
        i = j;
    }
    return out;
}

// The text without comments.
std::string stripComments(const std::string& s) {
    std::string r;
    for (size_t i = 0; i < s.size();) {
        if (s[i] == '/' && i + 1 < s.size() && s[i + 1] == '/') {
            size_t e = s.find('\n', i);
            i = e == std::string::npos ? s.size() : e;
        } else if (s[i] == '/' && i + 1 < s.size() && s[i + 1] == '*') {
            size_t e = s.find("*/", i + 2);
            i = e == std::string::npos ? s.size() : e + 2;
            r += ' ';
        } else {
            r += s[i++];
        }
    }
    return r;
}

std::vector<std::string> tokenize(const std::string& s) {
    std::vector<std::string> t;
    for (size_t i = 0; i < s.size();) {
        if (isspace(uint8_t(s[i]))) {
            i++;
        } else if (isIdent(s[i])) {
            size_t j = i;
            while (j < s.size() && (isIdent(s[j]) || s[j] == '.')) j++;
            t.push_back(s.substr(i, j - i));
            i = j;
        } else {
            t.push_back(std::string(1, s[i++]));
        }
    }
    return t;
}

struct Declarator {
    std::string name;
    int arraySize;  // 1 if not an array, 0 for "[]"
    bool init;
};

// A top-level declaration: qualifiers, layout, a type and its declarators.
struct Decl {
    std::string layout;             // what is inside layout( ), or ""
    std::vector<std::string> quals;  // storage, interpolation, precision ...
    std::string storage;            // uniform, in, out, const, or ""
    std::string type;
    std::vector<Declarator> names;
    bool ok = false;
};

bool isQualifier(const std::string& t) {
    static const std::set<std::string> q = {"uniform", "in",     "out",      "inout",     "const",   "flat",
                                            "smooth",  "noperspective", "centroid", "sample", "invariant", "precise",
                                            "highp",   "mediump", "lowp",    "attribute", "varying", "patch"};
    return q.count(t) != 0;
}

Decl parseDecl(const std::string& text) {
    Decl d;
    std::vector<std::string> t = tokenize(stripComments(text));
    size_t i = 0;
    while (i < t.size()) {
        if (t[i] == "layout" && i + 1 < t.size() && t[i + 1] == "(") {
            size_t j = i + 2;
            int depth = 1;
            std::string inner;
            for (; j < t.size() && depth; j++) {
                if (t[j] == "(") depth++;
                if (t[j] == ")" && --depth == 0) break;
                if (!inner.empty() && isIdent(t[j][0]) && isIdent(inner.back())) inner += ' ';
                inner += t[j];
            }
            d.layout = inner;
            i = j + 1;
        } else if (isQualifier(t[i])) {
            if (t[i] == "uniform" || t[i] == "in" || t[i] == "out" || t[i] == "const" || t[i] == "attribute" ||
                t[i] == "varying")
                d.storage = t[i] == "attribute" ? "in" : t[i];
            else
                d.quals.push_back(t[i]);
            i++;
        } else {
            break;
        }
    }
    if (i >= t.size() || !isIdent(t[i][0])) return d;
    d.type = t[i++];
    while (i < t.size()) {
        if (!isIdent(t[i][0])) return d;
        Declarator dc{t[i++], 1, false};
        if (i < t.size() && t[i] == "[") {
            dc.arraySize = 0;
            if (i + 1 < t.size() && t[i + 1] != "]") dc.arraySize = atoi(t[i + 1].c_str());
            while (i < t.size() && t[i] != "]") i++;
            i++;
        }
        if (i < t.size() && t[i] == "=") {
            dc.init = true;
            int depth = 0;
            while (i < t.size() && !(depth == 0 && (t[i] == "," || t[i] == ";"))) {
                if (t[i] == "(") depth++;
                if (t[i] == ")") depth--;
                i++;
            }
        }
        d.names.push_back(dc);
        if (i < t.size() && t[i] == ",") {
            i++;
            continue;
        }
        if (i < t.size() && t[i] == ";") {
            d.ok = true;
            break;
        }
        return d;
    }
    return d;
}

// The layout qualifier's text with `location=...` (or binding) replaced.
std::string layoutWith(const std::string& layout, const char* key, int value) {
    std::vector<std::string> parts;
    std::string cur;
    for (char c : layout + ",") {
        if (c == ',') {
            std::string p = cur;
            p.erase(0, p.find_first_not_of(' '));
            if (!p.empty() && p.compare(0, strlen(key), key) != 0) parts.push_back(p);
            cur.clear();
        } else {
            cur += c;
        }
    }
    char buf[64];
    snprintf(buf, sizeof buf, "%s = %d", key, value);
    parts.push_back(buf);
    std::string r;
    for (size_t i = 0; i < parts.size(); i++) r += (i ? ", " : "") + parts[i];
    return r;
}

std::string declText(const Decl& d, const std::string& layout, const Declarator& n) {
    std::string r;
    if (!layout.empty()) r += "layout(" + layout + ") ";
    for (const std::string& q : d.quals) r += q + " ";
    if (!d.storage.empty()) r += d.storage + " ";
    r += d.type + " " + n.name;
    if (n.arraySize != 1) r += "[" + (n.arraySize ? std::to_string(n.arraySize) : std::string()) + "]";
    return r + ";";
}

int locationsOf(const std::string& type, int arraySize) {
    const TypeInfo* t = findType(type);
    return (t && t->locations ? t->locations : 1) * std::max(arraySize, 1);
}

// An explicit location=N in a layout qualifier, or -1.
int explicitLocation(const std::string& layout) {
    size_t at = layout.find("location");
    if (at == std::string::npos) return -1;
    at = layout.find('=', at);
    return at == std::string::npos ? -1 : atoi(layout.c_str() + at + 1);
}

uint32_t alignUp(uint32_t v, uint32_t a) { return (v + a - 1) / a * a; }

}  // namespace

uint32_t glslTypeSize(uint32_t type) {
    for (const TypeInfo& t : kTypes)
        if (t.type == type) return t.size;
    return 0;
}

bool adaptProgram(const std::string& vsIn, const std::string& gsIn, const std::string& fsIn,
                  const std::map<std::string, int>& attribBindings, const std::map<std::string, int>& fragBindings,
                  GlslProgram* out) {
    *out = GlslProgram();
    const bool hasGs = !gsIn.empty();
    std::vector<Segment> stages[3] = {split(vsIn), hasGs ? split(gsIn) : std::vector<Segment>(), split(fsIn)};

    // Loose uniforms and samplers, by name: the same in every stage.
    std::map<std::string, size_t> uniformIndex;
    uint32_t stageBit = 1;  // the stage being adapted
    int nextSampler = 0;
    uint32_t blockSize = 0;
    auto addUniform = [&](const Decl& d, const Declarator& n, std::string* err) -> const GlslUniform* {
        auto it = uniformIndex.find(n.name);
        if (it != uniformIndex.end()) {
            out->uniforms[it->second].stages |= stageBit;
            return &out->uniforms[it->second];
        }
        const TypeInfo* t = findType(d.type);
        if (!t) {
            *err = "uniform " + n.name + " has a type the adapter does not know: " + d.type;
            return nullptr;
        }
        if (n.arraySize == 0) {
            *err = "uniform " + n.name + " is an array without a size";
            return nullptr;
        }
        if (n.init) {
            *err = "uniform " + n.name + " has an initializer";
            return nullptr;
        }
        GlslUniform u;
        u.name = n.name;
        u.type = t->type;
        u.arraySize = n.arraySize;
        u.offset = 0;
        u.stride = 0;
        u.binding = -1;
        u.stages = stageBit;
        if (t->sampler) {
            u.binding = nextSampler;
            nextSampler += n.arraySize;
            if (nextSampler > kMaxSamplerBindings) {
                *err = "more than 32 samplers";
                return nullptr;
            }
        } else {
            // std140: array elements and matrices on 16-byte boundaries
            const bool padded = n.arraySize > 1 || t->size > 16;
            const uint32_t align = padded ? 16 : t->align;
            u.stride = padded ? alignUp(t->size, 16) : t->size;
            u.offset = alignUp(blockSize, align);
            blockSize = u.offset + (n.arraySize > 1 ? u.stride * uint32_t(n.arraySize) : t->size);
        }
        uniformIndex[n.name] = out->uniforms.size();
        out->uniforms.push_back(u);
        return &out->uniforms.back();
    };

    std::map<std::string, int> blockIndex;
    // varyings by name, per interface: vertex -> next stage, geometry -> fragment
    std::map<std::string, int> varyings[2];
    int nextVarying[2] = {0, 0};

    for (int st = 0; st < 3; st++) {
        if (st == 1 && !hasGs) continue;
        const bool vertex = st == 0, geometry = st == 1, fragment = st == 2;
        stageBit = vertex ? kStageVertex : geometry ? kStageGeometry : kStageFragment;
        const int inIf = fragment && hasGs ? 1 : 0;  // the interface this stage's inputs come by
        const int outIf = geometry ? 1 : 0;
        std::set<int> usedInputs;  // vertex input locations taken
        if (vertex)
            for (const auto& kv : attribBindings) usedInputs.insert(kv.second);
        int nextFragOut = 0;
        // object-like #defines: a varying renamed by one pairs by its real name
        std::map<std::string, std::string> defines;
        auto realName = [&defines](const std::string& n) {
            auto it = defines.find(n);
            return it != defines.end() ? it->second : n;
        };
        for (Segment& seg : stages[st]) {
            if (seg.kind == K_PREPROC) {
                const std::string p = seg.text;
                size_t at = p.find_first_not_of(" \t#");
                if (at != std::string::npos && p.compare(at, 7, "version") == 0) seg.text = "#version 460";
                if (at != std::string::npos && p.compare(at, 6, "define") == 0) {
                    const std::vector<std::string> t = tokenize(p.substr(at + 6));
                    if (t.size() == 2 && isIdent(t[0][0]) && isIdent(t[1][0])) defines[t[0]] = t[1];
                }
                continue;
            }
            if (seg.kind == K_BLOCK) {
                const std::string text = stripComments(seg.text);
                const size_t brace = text.find('{');
                Decl head = parseDecl(text.substr(0, brace) + " _block;");
                if (head.storage != "uniform") continue;  // structs; in/out blocks are left alone
                // the block's name is the type parsed above
                const std::string name = head.type;
                int index;
                auto it = blockIndex.find(name);
                if (it != blockIndex.end()) {
                    index = it->second;
                } else {
                    index = int(out->blocks.size());
                    blockIndex[name] = index;
                    out->blocks.push_back(name);
                    out->blockStages.push_back(0);
                }
                out->blockStages[size_t(index)] |= stageBit;
                if (index >= kDefaultBlockBinding) {
                    out->error = "too many uniform blocks";
                    return false;
                }
                std::string layout = head.layout;
                if (layout.find("std140") == std::string::npos) layout = layout.empty() ? "std140" : "std140, " + layout;
                layout = layoutWith(layout, "binding", index);
                seg.text = "layout(" + layout + ") uniform " + name + " " + text.substr(brace);
                continue;
            }
            if (seg.kind != K_DECL) continue;
            Decl d = parseDecl(seg.text);
            if (!d.ok || d.storage.empty() || d.storage == "const") continue;
            if (d.names.empty()) continue;
            std::string text;
            for (const Declarator& n : d.names) {
                if (n.name.compare(0, 3, "gl_") == 0) {
                    text += declText(d, d.layout, n);
                    continue;
                }
                if (d.storage == "uniform") {
                    std::string err;
                    const GlslUniform* u = addUniform(d, n, &err);
                    if (!u) {
                        out->error = err;
                        return false;
                    }
                    if (u->binding >= 0) text += declText(d, layoutWith(d.layout, "binding", u->binding), n);
                    // others: members of the default block
                    continue;
                }
                int loc = explicitLocation(d.layout);
                const std::string name = realName(n.name);
                // a geometry shader's inputs are arrays of one per vertex
                const int count = locationsOf(d.type, geometry && d.storage == "in" ? 1 : n.arraySize);
                if (vertex && d.storage == "in") {
                    if (loc < 0) {
                        auto it = attribBindings.find(n.name);
                        if (it != attribBindings.end()) {
                            loc = it->second;
                        } else {
                            loc = 0;
                            while (usedInputs.count(loc)) loc++;
                        }
                    }
                    for (int k = 0; k < count; k++) usedInputs.insert(loc + k);
                    out->attribs[n.name] = loc;
                } else if (!fragment && d.storage == "out") {
                    if (loc < 0) {
                        loc = nextVarying[outIf];
                        nextVarying[outIf] += count;
                    }
                    varyings[outIf][name] = loc;
                } else if (!vertex && d.storage == "in") {
                    if (loc < 0) {
                        auto it = varyings[inIf].find(name);
                        if (it != varyings[inIf].end()) {
                            loc = it->second;
                        } else {
                            loc = nextVarying[inIf];
                            nextVarying[inIf] += count;
                        }
                    }
                } else if (fragment && d.storage == "out") {
                    if (loc < 0) {
                        auto it = fragBindings.find(n.name);
                        loc = it != fragBindings.end() ? it->second : nextFragOut;
                    }
                    nextFragOut = loc + count;
                } else {
                    text += declText(d, d.layout, n);
                    continue;
                }
                text += declText(d, layoutWith(d.layout, "location", loc), n);
            }
            seg.text = text;
        }
    }

    // The default block, before each stage's first statement.
    out->defaultBlockSize = alignUp(blockSize, 16);
    std::string block;
    if (blockSize) {
        char buf[96];
        snprintf(buf, sizeof buf, "layout(std140, binding = %d) uniform _dkgl_Default {\n", int(kDefaultBlockBinding));
        block = buf;
        for (const GlslUniform& u : out->uniforms) {
            if (u.binding >= 0) continue;
            const char* tn = "float";
            for (const TypeInfo& t : kTypes)
                if (t.type == u.type) tn = t.name;
            block += "  " + std::string(tn) + " " + u.name;
            if (u.arraySize > 1) block += "[" + std::to_string(u.arraySize) + "]";
            block += ";\n";
        }
        block += "};\n";
    }
    for (int st = 0; st < 3; st++) {
        if (st == 1 && !hasGs) continue;
        std::string s;
        bool placed = block.empty();
        for (const Segment& seg : stages[st]) {
            if (!placed && (seg.kind == K_DECL || seg.kind == K_BLOCK || seg.kind == K_FUNCTION)) {
                s += block;
                placed = true;
            }
            s += seg.text;
        }
        if (!placed) s += block;
        (st == 0 ? out->vs : st == 1 ? out->gs : out->fs) = s;
    }
    return true;
}

}  // namespace dkgl
