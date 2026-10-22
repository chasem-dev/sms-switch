#!/usr/bin/env bash
# Build make-pack against build/uam-host (tools/uam/build-uam.sh) and pack the
# programs a run dumped (SMS_DKGL_DUMP=dir, SMS_DKGL_DUMP_ONLY=1 and
# SMS_GX_SHADER_WARMUP_MS=100000000 in env.txt write all the known ones):
#
#   tools/dkgl/make-pack.sh <dump dir> [pack]   (default build/dkgl-programs.pack)
#
# Copy the pack next to the game's NRO (sdmc:/switch/sms/dkgl-programs.pack).
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
[[ $# -ge 1 ]] || { echo "usage: $0 <dump dir> [pack]" >&2; exit 2; }
uam="$here/build/uam-host"
[[ -f $uam/lib/libuam.a ]] || { echo "make-pack: run tools/uam/build-uam.sh first" >&2; exit 1; }
tool="$here/build/make-pack"
if [[ ! -x $tool || $tool -ot $here/tools/dkgl/make-pack.cpp || $tool -ot $uam/lib/libuam.a ]]; then
	g++ -O2 -std=c++17 -I"$uam/include" "$here/tools/dkgl/make-pack.cpp" "$uam/lib/libuam.a" -lpthread -o "$tool"
fi
"$tool" "$1" "${2:-$here/build/dkgl-programs.pack}"
