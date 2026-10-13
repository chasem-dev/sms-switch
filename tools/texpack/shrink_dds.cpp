// shrink_dds IN.dds OUT.dds MAX_SIDE
//
// Writes a BC7 DDS no larger than MAX_SIDE pixels on its long side, for the
// Switch's copy of an HD texture pack (tools/texpack/make-switch-pack.sh):
// a texture of 4096-16384 pixels costs 64-256 MiB of GPU memory, more than
// the console can spare. A file that has the smaller mip levels already is
// cut down to them unchanged; otherwise its top level is decoded, halved
// with a box filter until it fits, and encoded again with a full mip chain
// (bc7enc and bc7decomp, from Rich Geldreich's bc7enc_rdo, MIT). Files that
// already fit, or are not BC7, are copied as they are (exit status 2).
#include "bc7decomp.h"
#include "bc7enc.h"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {
struct Image {
	uint32_t w = 0, h = 0;
	std::vector<uint8_t> rgba;
};

uint32_t blocks(uint32_t n) { return (n + 3) / 4; }
uint32_t level_size(uint32_t w, uint32_t h) { return blocks(w) * blocks(h) * 16; }

bool read_file(const char* path, std::vector<uint8_t>& out)
{
	FILE* f = fopen(path, "rb");
	if (!f)
		return false;
	fseek(f, 0, SEEK_END);
	out.resize(size_t(ftell(f)));
	fseek(f, 0, SEEK_SET);
	const bool ok = out.empty() || fread(out.data(), out.size(), 1, f) == 1;
	fclose(f);
	return ok;
}

uint32_t le32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24; }
void put32(std::vector<uint8_t>& v, size_t at, uint32_t x)
{
	for (int i = 0; i < 4; i++)
		v[at + i] = uint8_t(x >> (8 * i));
}

// A DDS (DX10 header, BC7) holding `levels` (each level's blocks, in order).
bool write_dds(const char* path, uint32_t w, uint32_t h, uint32_t dxgi, const std::vector<std::vector<uint8_t>>& levels)
{
	std::vector<uint8_t> hdr(4 + 124 + 20, 0);
	memcpy(hdr.data(), "DDS ", 4);
	put32(hdr, 4, 124);
	put32(hdr, 8, 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | 0x80000); // caps height width pixelformat mipmapcount linearsize
	put32(hdr, 12, h);
	put32(hdr, 16, w);
	put32(hdr, 20, level_size(w, h));
	put32(hdr, 28, uint32_t(levels.size()));
	put32(hdr, 76, 32);    // pixel format size
	put32(hdr, 80, 0x4);   // DDPF_FOURCC
	memcpy(&hdr[84], "DX10", 4);
	put32(hdr, 108, 0x1000 | 0x8 | 0x400000); // texture complex mipmap
	put32(hdr, 128, dxgi);
	put32(hdr, 132, 3); // TEXTURE2D
	put32(hdr, 140, 1); // array size
	FILE* f = fopen(path, "wb");
	if (!f)
		return false;
	bool ok = fwrite(hdr.data(), hdr.size(), 1, f) == 1;
	for (const auto& l : levels)
		ok = ok && fwrite(l.data(), l.size(), 1, f) == 1;
	return fclose(f) == 0 && ok;
}

Image decode(const uint8_t* data, uint32_t w, uint32_t h)
{
	Image img;
	img.w = w;
	img.h = h;
	img.rgba.resize(size_t(w) * h * 4);
	for (uint32_t by = 0; by < blocks(h); by++)
		for (uint32_t bx = 0; bx < blocks(w); bx++) {
			bc7decomp::color_rgba px[16];
			bc7decomp::unpack_bc7(data + (size_t(by) * blocks(w) + bx) * 16, px);
			for (uint32_t y = 0; y < 4; y++)
				for (uint32_t x = 0; x < 4; x++) {
					const uint32_t ix = bx * 4 + x, iy = by * 4 + y;
					if (ix < w && iy < h)
						memcpy(&img.rgba[(size_t(iy) * w + ix) * 4], &px[y * 4 + x], 4);
				}
		}
	return img;
}

// Half size, each pixel the mean of up to 2x2 (an odd edge repeats).
Image half(const Image& in)
{
	Image out;
	out.w = std::max(1u, in.w / 2);
	out.h = std::max(1u, in.h / 2);
	out.rgba.resize(size_t(out.w) * out.h * 4);
	for (uint32_t y = 0; y < out.h; y++)
		for (uint32_t x = 0; x < out.w; x++)
			for (int c = 0; c < 4; c++) {
				uint32_t sum = 0;
				for (uint32_t dy = 0; dy < 2; dy++)
					for (uint32_t dx = 0; dx < 2; dx++) {
						const uint32_t sx = std::min(in.w - 1, x * 2 + dx), sy = std::min(in.h - 1, y * 2 + dy);
						sum += in.rgba[(size_t(sy) * in.w + sx) * 4 + c];
					}
				out.rgba[(size_t(y) * out.w + x) * 4 + c] = uint8_t((sum + 2) / 4);
			}
	return out;
}

std::vector<uint8_t> encode(const Image& img, const bc7enc_compress_block_params& params)
{
	std::vector<uint8_t> out(level_size(img.w, img.h));
	for (uint32_t by = 0; by < blocks(img.h); by++)
		for (uint32_t bx = 0; bx < blocks(img.w); bx++) {
			uint8_t px[64];
			for (uint32_t y = 0; y < 4; y++)
				for (uint32_t x = 0; x < 4; x++) {
					const uint32_t sx = std::min(img.w - 1, bx * 4 + x), sy = std::min(img.h - 1, by * 4 + y);
					memcpy(&px[(y * 4 + x) * 4], &img.rgba[(size_t(sy) * img.w + sx) * 4], 4);
				}
			bc7enc_compress_block(&out[(size_t(by) * blocks(img.w) + bx) * 16], px, &params);
		}
	return out;
}
} // namespace

int main(int argc, char** argv)
{
	if (argc != 4) {
		fprintf(stderr, "usage: shrink_dds IN.dds OUT.dds MAX_SIDE\n");
		return 1;
	}
	const uint32_t max_side = uint32_t(atoi(argv[3]));
	std::vector<uint8_t> f;
	if (!read_file(argv[1], f) || f.size() < 148 || memcmp(f.data(), "DDS ", 4) || memcmp(&f[84], "DX10", 4)) {
		fprintf(stderr, "shrink_dds: %s: not a DX10 DDS\n", argv[1]);
		return 2;
	}
	const uint32_t h = le32(&f[12]), w = le32(&f[16]), mips = std::max(1u, le32(&f[28])), dxgi = le32(&f[128]);
	if ((dxgi != 98 && dxgi != 99) || std::max(w, h) <= max_side)
		return 2; // not BC7, or already fits: the caller keeps the original
	const uint8_t* data = &f[148];
	size_t avail = f.size() - 148;

	// Mip levels already there: start at the first that fits.
	std::vector<std::vector<uint8_t>> levels;
	uint32_t lw = w, lh = h;
	size_t at = 0;
	for (uint32_t i = 0; i < mips && at + level_size(lw, lh) <= avail; i++) {
		if (std::max(lw, lh) <= max_side)
			levels.emplace_back(data + at, data + at + level_size(lw, lh));
		at += level_size(lw, lh);
		lw = std::max(1u, lw / 2);
		lh = std::max(1u, lh / 2);
	}
	if (!levels.empty()) {
		uint32_t tw = w, th = h;
		while (std::max(tw, th) > max_side) {
			tw = std::max(1u, tw / 2);
			th = std::max(1u, th / 2);
		}
		if (!write_dds(argv[2], tw, th, dxgi, levels))
			return 1;
		printf("%s: %ux%u -> %ux%u (its own mip levels)\n", argv[1], w, h, tw, th);
		return 0;
	}

	// Decode, halve until it fits, and encode with a full mip chain.
	if (avail < level_size(w, h)) {
		fprintf(stderr, "shrink_dds: %s: truncated\n", argv[1]);
		return 1;
	}
	Image img = decode(data, w, h);
	while (std::max(img.w, img.h) > max_side)
		img = half(img);
	bc7enc_compress_block_init();
	bc7enc_compress_block_params params;
	bc7enc_compress_block_params_init(&params);
	const uint32_t tw = img.w, th = img.h;
	for (;;) {
		levels.push_back(encode(img, params));
		if (img.w == 1 && img.h == 1)
			break;
		img = half(img);
	}
	if (!write_dds(argv[2], tw, th, dxgi, levels))
		return 1;
	printf("%s: %ux%u -> %ux%u (encoded again, %zu levels)\n", argv[1], w, h, tw, th, levels.size());
	return 0;
}
