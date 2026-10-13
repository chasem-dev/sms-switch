#!/usr/bin/env bash
# Make the Switch's copy of an HD texture pack: every file as it is (hard
# links where the file system allows, else copies), except BC7 textures
# larger than MAX pixels (default 2048), which shrink_dds.cpp cuts down. The
# UHD pack's 157 textures of 4096-16384 pixels are 2.1 GiB of its 3 GiB, and
# up to 256 MiB of GPU memory each.
#
#   tools/texpack/make-switch-pack.sh SRC_PACK_DIR OUT_DIR [MAX]
#   e.g. tools/texpack/make-switch-pack.sh ~/sms-port/mods/textures build/switch-textures
# Then copy OUT_DIR's contents to sdmc:/switch/sms/mods/textures/.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
src="$(cd "$1" && pwd)"; out="$(mkdir -p "$2" && cd "$2" && pwd)"; max="${3:-2048}"
jobs="${JOBS:-$(nproc)}"
tool="$here/build/texpack/shrink_dds"

# The tool, with bc7enc_rdo at a pinned commit (fetched once into build/).
if [[ ! -x "$tool" ]]; then
	mkdir -p "$here/build/texpack"
	if [[ ! -d "$here/build/texpack/bc7enc_rdo" ]]; then
		git clone -q https://github.com/richgel999/bc7enc_rdo.git "$here/build/texpack/bc7enc_rdo"
		git -C "$here/build/texpack/bc7enc_rdo" checkout -q b943862
	fi
	b="$here/build/texpack/bc7enc_rdo"
	g++ -O2 -std=c++17 -I"$b" -o "$tool" "$here/tools/texpack/shrink_dds.cpp" "$b/bc7enc.cpp" "$b/bc7decomp.cpp" -lpthread
fi

echo "make-switch-pack: linking $src -> $out"
cp -al "$src/." "$out/" 2>/dev/null || cp -a "$src/." "$out/"
# Shrink the large ones (in parallel); a file that fits stays the link.
export tool max
find "$out" -name '*.dds' -print0 | xargs -0 -P "$jobs" -I{} bash -c '
	f="$1"; t="$f.shrunk"
	if "$tool" "$f" "$t" "$max"; then mv -f "$t" "$f"; else rc=$?; rm -f "$t"; [ "$rc" = 2 ] || echo "failed: $f" >&2; fi' _ {}
du -sh "$src" "$out"
