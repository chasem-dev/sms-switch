// make-pack: compiles the programs a run wrote with SMS_DKGL_DUMP (each
// program's adapted GLSL, <key>.vs/.gs/.fs) with deko3d's compiler, into the
// pack switch_main.cpp gives dkgl (sdmc:/switch/sms/dkgl-programs.pack), so
// the console starts with every known program compiled.
//
//   make-pack <dump dir> <pack>          (tools/dkgl/make-pack.sh builds it)
//
// The record format and key are dkgl_program.cpp's. It also names programs
// whose code calls a subroutine (CAL/PRET: integer division, for one), which
// Ryujinx-based emulators cannot run.
#include "uam_api.h"

#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {

const char kPackMagic[8] = {'D', 'K', 'G', 'L', 'P', 'K', '0', '1'};

uint64_t fnv(const std::string& s, uint64_t h = 1469598103934665603ull) {
    for (unsigned char c : s) h = (h ^ c) * 1099511628211ull;
    return h;
}

std::string slurp(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

// A subroutine call or return in a DKSH module's code (after its 0x80-byte
// program header, in groups of a scheduling word and three instructions).
bool callsSubroutine(const void* dksh, size_t size) {
    uint32_t hdr[6];
    if (size < sizeof hdr) return false;
    memcpy(hdr, dksh, sizeof hdr);
    const uint32_t control = hdr[2], code = hdr[3];
    if (control + code > size) return false;
    const uint8_t* at = static_cast<const uint8_t*>(dksh) + control;
    for (uint32_t off = 0x80, i = 0; off + 8 <= code; off += 8, i++) {
        if (i % 4 == 0) continue;  // scheduling
        uint64_t w;
        memcpy(&w, at + off, 8);
        const uint32_t op = uint32_t(w >> 52);
        if (op == 0xE26 || op == 0xE27) return true;  // CAL, PRET
    }
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: make-pack <dump dir> <pack>\n");
        return 2;
    }
    const std::string dir = argv[1];
    std::vector<std::string> keys;
    DIR* d = opendir(dir.c_str());
    if (!d) {
        perror(dir.c_str());
        return 1;
    }
    while (dirent* e = readdir(d)) {
        const std::string n = e->d_name;
        if (n.size() == 19 && n.compare(16, 3, ".vs") == 0) keys.push_back(n.substr(0, 16));
    }
    closedir(d);
    std::sort(keys.begin(), keys.end());

    std::map<uint64_t, std::string> records;
    int failed = 0, subroutines = 0;
    for (const std::string& name : keys) {
        const std::string base = dir + "/" + name;
        const std::string vs = slurp(base + ".vs"), gs = slurp(base + ".gs"), fs = slurp(base + ".fs");
        const uint64_t key = fnv(fs, fnv(gs + '\x02', fnv("uam 1.1.0 dkgl 2\n" + vs + '\x01')));
        if (key != strtoull(name.c_str(), nullptr, 16)) {
            fprintf(stderr, "%s: the sources do not give its key (another dkgl version?)\n", name.c_str());
            failed++;
            continue;
        }
        const std::string* src[3] = {&vs, &gs, &fs};
        const uam_stage stage[3] = {UAM_STAGE_VERTEX, UAM_STAGE_GEOMETRY, UAM_STAGE_FRAGMENT};
        const char* ext[3] = {"vs", "gs", "fs"};
        void* out[3] = {nullptr, nullptr, nullptr};
        size_t size[3] = {0, 0, 0};
        bool ok = true;
        for (int i = 0; i < 3 && ok; i++) {
            if (src[i]->empty()) continue;
            ok = uam_compile_dksh(src[i]->c_str(), stage[i], &out[i], &size[i]) != 0;
            if (!ok) fprintf(stderr, "%s.%s did not compile\n", name.c_str(), ext[i]);
            else if (callsSubroutine(out[i], size[i])) {
                printf("%s.%s calls a subroutine\n", name.c_str(), ext[i]);
                subroutines++;
            }
        }
        if (ok) {
            const uint32_t sizes[3] = {uint32_t(size[0]), uint32_t(size[1]), uint32_t(size[2])};
            std::string rec(reinterpret_cast<const char*>(sizes), sizeof sizes);
            for (int i = 0; i < 3; i++)
                if (out[i]) rec.append(static_cast<const char*>(out[i]), size[i]);
            records[key] = std::move(rec);
        } else {
            failed++;
        }
        for (void* o : out) free(o);
    }

    FILE* f = fopen(argv[2], "wb");
    if (!f) {
        perror(argv[2]);
        return 1;
    }
    const uint32_t count = uint32_t(records.size());
    fwrite(kPackMagic, 8, 1, f);
    fwrite(&count, 4, 1, f);
    for (const auto& r : records) {
        fwrite(&r.first, 8, 1, f);
        fwrite(r.second.data(), r.second.size(), 1, f);
    }
    fclose(f);
    printf("%s: %u programs, %d failed, %d stages calling subroutines\n", argv[2], count, failed, subroutines);
    return failed ? 1 : 0;
}
