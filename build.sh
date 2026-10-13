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
#   SMS_SWITCH_KEYS=/path/prod.keys ./build.sh
#                         also pack build/nsp/sms-switch.nsp, an installable title
#                         (HOME menu icon, full memory) that starts the NRO from
#                         SMS_SWITCH_NSP_NRO (default sdmc:/switch/sms/ and the
#                         standalone NRO if built, else sms.nro). The keys are your
#                         console's (Lockpick_RCM); only their path is remembered
#                         (.switch-keys); SMS_SWITCH_KEYS= turns it off.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# The build image, rebuilt whenever tools/docker/Dockerfile changes.
build_image="sms-switch-build:$(sha256sum "$here/tools/docker/Dockerfile" | cut -c1-12)"
image="${SMS_SWITCH_IMAGE:-$build_image}"
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

# The installable NSP (tools/make-nsp.sh), with your console's keys.
keys_memory="$here/.switch-keys"
if [[ -z "${SMS_SWITCH_KEYS+set}" && -f "$keys_memory" ]]; then
	SMS_SWITCH_KEYS="$(cat "$keys_memory")"
fi
nsp_cmd="" docker_keys=()
if [[ -n "${SMS_SWITCH_KEYS:-}" ]]; then
	keys="$(realpath "$SMS_SWITCH_KEYS" 2>/dev/null || true)"
	[[ -f "$keys" ]] || { echo "build.sh: no keys at $SMS_SWITCH_KEYS (SMS_SWITCH_KEYS= stops the NSP)" >&2; exit 1; }
	printf '%s\n' "$keys" > "$keys_memory"
	if [[ -n "${SMS_SWITCH_NSP_NRO:-}" ]]; then
		nro_path="$SMS_SWITCH_NSP_NRO"
	elif [[ -n "${SMS_SWITCH_DISC:-}" ]]; then
		nro_path="sdmc:/switch/sms/sms-standalone.nro"
	else
		nro_path="sdmc:/switch/sms/sms.nro"
	fi
	version="$(sed -n 's/^project(sms_switch VERSION \([0-9.]*\).*/\1/p' "$here/CMakeLists.txt")"
	if [[ "${SMS_SWITCH_NATIVE:-0}" == 1 ]]; then
		keys_in="$keys" icon_default="$DEVKITPRO/libnx/default_icon.jpg" src="$here"
	else
		keys_in=/keys/prod.keys icon_default=/opt/devkitpro/libnx/default_icon.jpg src=/src
		docker_keys=(-v "$keys:/keys/prod.keys:ro")
	fi
	nsp_cmd="icon=$src/build/icon.jpg; [ -f \$icon ] || icon=$icon_default; \
		$src/tools/make-nsp.sh ${keys_in@Q} ${nro_path@Q} \$icon ${version@Q}"
elif [[ -n "${SMS_SWITCH_KEYS+set}" ]]; then
	rm -f "$keys_memory" "$here/build/nsp/sms-switch.nsp"
fi

# ccache, where the toolchain has it (the sms-switch-build image does).
inner="set -e
cache=\$(command -v ccache || true)
cmake -S /src -B /src/build -G Ninja -DCMAKE_TOOLCHAIN_FILE=/opt/devkitpro/cmake/Switch.cmake -DCMAKE_BUILD_TYPE=Release \\
	-DSMS_COMPILER_CACHE=\$cache ${disc_args[*]@Q} $*
cmake --build /src/build -j $jobs
[ -z \"\$cache\" ] || ccache -s | grep -E 'Hits|Misses' | head -2
$nsp_cmd"

if [[ "${SMS_SWITCH_NATIVE:-0}" == 1 ]]; then
	: "${DEVKITPRO:?set DEVKITPRO to your devkitPro install}"
	cmake -S "$here" -B "$here/build" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/Switch.cmake" \
		-DCMAKE_BUILD_TYPE=Release -DSMS_COMPILER_CACHE="$(command -v ccache || true)" "${disc_args[@]}" "$@"
	cmake --build "$here/build" -j "$jobs"
	[[ -z "$nsp_cmd" ]] || bash -c "$nsp_cmd"
else
	if [[ "$image" == "$build_image" ]] && ! docker image inspect "$image" >/dev/null 2>&1; then
		echo "build.sh: building the $image image (devkitA64 + ccache + Pillow)"
		docker build -q -t "$image" "$here/tools/docker" >/dev/null
	fi
	mkdir -p "$ccache_dir"
	docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp \
		-v "$ccache_dir:/ccache" -e CCACHE_DIR=/ccache -e CCACHE_BASEDIR=/src -e CCACHE_MAXSIZE=5G \
		${SMS_SWITCH_DOCKER_ARGS:-} "${docker_disc[@]}" "${docker_keys[@]}" \
		-v "$here:/src" -w /src "$image" bash -c "$inner"
fi
echo "build.sh: built $here/build/sms.nro"
if [[ -n "$nsp_cmd" && -f "$here/build/nsp/sms-switch.nsp" ]]; then
	echo "build.sh: built $here/build/nsp/sms-switch.nsp (starts $nro_path; install it with Goldleaf or DBI)"
fi
if [[ -f "$here/build/sms-standalone.nro" && -f "$remember" ]]; then
	echo "build.sh: built $here/build/sms-standalone.nro with $(cat "$remember") inside (personal use only)"
fi
