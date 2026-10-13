#!/usr/bin/env bash
# Build build/sms.nro for the Nintendo Switch (libnx homebrew), inside the
# devkitpro/devkita64 Docker image, so the host needs only Docker.
#
#   ./build.sh            configure (first time) and build
#   ./build.sh --clean    delete build/ first
#   JOBS=2 ./build.sh     limit parallel compiler jobs (default: all cores)
#   SMS_SWITCH_NATIVE=1   use an installed devkitPro (DEVKITPRO) instead of Docker
#   SMS_SWITCH_CCACHE_DIR compiler cache (default ~/.cache/sms-switch/ccache)
#   SMS_SWITCH_DISC=/path/GMSE01.iso ./build.sh
#                         also build build/sms-standalone.nro with that disc image
#                         inside (personal use only: never share it). Once given,
#                         it is remembered (.bundled-disc, kept by --clean);
#                         SMS_SWITCH_DISC= (empty) turns it off.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
image="${SMS_SWITCH_IMAGE:-sms-switch-build}"
# Compiled objects are cached here across builds (and ./build.sh --clean).
ccache_dir="${SMS_SWITCH_CCACHE_DIR:-${XDG_CACHE_HOME:-$HOME/.cache}/sms-switch/ccache}"
jobs="${JOBS:-$(nproc)}"

if [[ "${1:-}" == "--clean" ]]; then
	rm -rf "$here/build"
	shift
fi

if [[ ! -f "$here/sms-port/CMakeLists.txt" || ! -f "$here/sms-port/decomp/configure.py" ]]; then
	echo "build.sh: fetching submodules"
	git -C "$here" submodule update --init --recursive
fi
"$here/tools/apply-patches.sh"

# The disc image for sms-standalone.nro, remembered in .bundled-disc (kept by --clean)
# (Docker sees it at /disc/disc.<ext>).
remember="$here/.bundled-disc"
if [[ -z "${SMS_SWITCH_DISC+set}" && -f "$remember" ]]; then
	SMS_SWITCH_DISC="$(cat "$remember")"
fi
disc_args=() docker_disc=()
if [[ -n "${SMS_SWITCH_DISC:-}" ]]; then
	disc="$(realpath "$SMS_SWITCH_DISC" 2>/dev/null || true)"
	[[ -f "$disc" ]] || { echo "build.sh: no disc image at $SMS_SWITCH_DISC (SMS_SWITCH_DISC= stops bundling)" >&2; exit 1; }
	ext="${disc##*.}"
	ext="${ext,,}"
	if [[ "${SMS_SWITCH_NATIVE:-0}" == 1 ]]; then
		disc_args=(-DSMS_SWITCH_BUNDLE_DISC="$disc")
	else
		disc_args=(-DSMS_SWITCH_BUNDLE_DISC="/disc/disc.$ext")
		docker_disc=(-v "$disc:/disc/disc.$ext:ro")
	fi
	mkdir -p "$here/build"
	printf '%s\n' "$disc" > "$remember"
elif [[ -n "${SMS_SWITCH_DISC+set}" ]]; then
	disc_args=(-DSMS_SWITCH_BUNDLE_DISC=)
	rm -f "$remember"
fi

# ccache, where the toolchain has it (the sms-switch-build image does).
inner="cache=\$(command -v ccache || true); \
  cmake -S /src -B /src/build -G Ninja -DCMAKE_TOOLCHAIN_FILE=/opt/devkitpro/cmake/Switch.cmake -DCMAKE_BUILD_TYPE=Release \
    -DSMS_COMPILER_CACHE=\$cache ${disc_args[*]@Q} $* \
  && cmake --build /src/build -j $jobs && { [ -z \"\$cache\" ] || ccache -s | grep -E 'Hits|Misses' | head -2; }"

if [[ "${SMS_SWITCH_NATIVE:-0}" == 1 ]]; then
	: "${DEVKITPRO:?set DEVKITPRO to your devkitPro install}"
	cmake -S "$here" -B "$here/build" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/Switch.cmake" \
		-DCMAKE_BUILD_TYPE=Release -DSMS_COMPILER_CACHE="$(command -v ccache || true)" "${disc_args[@]}" "$@"
	cmake --build "$here/build" -j "$jobs"
else
	if [[ "$image" == sms-switch-build ]] && ! docker image inspect sms-switch-build >/dev/null 2>&1; then
		echo "build.sh: building the sms-switch-build image (devkitA64 + ccache), once"
		docker build -q -t sms-switch-build "$here/tools/docker" >/dev/null
	fi
	mkdir -p "$ccache_dir"
	docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp \
		-v "$ccache_dir:/ccache" -e CCACHE_DIR=/ccache -e CCACHE_BASEDIR=/src -e CCACHE_MAXSIZE=5G \
		${SMS_SWITCH_DOCKER_ARGS:-} "${docker_disc[@]}" \
		-v "$here:/src" -w /src "$image" bash -c "$inner"
fi
echo "build.sh: built $here/build/sms.nro"
if [[ -f "$here/build/sms-standalone.nro" && -f "$remember" ]]; then
	echo "build.sh: built $here/build/sms-standalone.nro with $(cat "$remember") inside (personal use only)"
fi
