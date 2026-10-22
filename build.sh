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
#   SMS_SWITCH_TEXTURES=/path/pack ./build.sh
#                         with SMS_SWITCH_DISC, also pack that HD texture pack
#                         (tools/texpack/make-switch-pack.sh's output) into
#                         sms-standalone.nro; remembered (.bundled-textures),
#                         SMS_SWITCH_TEXTURES= turns it off.
#   SMS_SWITCH_ECLIPSE=1 ./build.sh
#                         also build build/eclipse/sms-eclipse.nro: Super Mario
#                         Eclipse's code built into the game (fetched at its
#                         pinned revisions the first time), for the Eclipse disc
#                         (GMSE04). Remembered (.eclipse); SMS_SWITCH_ECLIPSE=0 stops it.
#   SMS_SWITCH_ECLIPSE_DISC=/path/eclipse.iso ./build.sh
#                         that, and build/eclipse/sms-eclipse-standalone.nro with
#                         that image inside (personal use only, as above).
#                         Remembered (.bundled-eclipse-disc); SMS_SWITCH_ECLIPSE_DISC= stops it.
#   SMS_SWITCH_ECLIPSE_ICON=/path/icon.jpg
#                         a 256x256 JPEG for sms-eclipse.nro and its NSP (else the
#                         Eclipse disc's memory-card icon). Remembered
#                         (.eclipse-icon); SMS_SWITCH_ECLIPSE_ICON= stops it.
#   SMS_SWITCH_KEYS=/path/prod.keys ./build.sh
#                         also pack build/nsp/sms-switch.nsp, an installable title
#                         (HOME menu icon, full memory) that starts the NRO from
#                         SMS_SWITCH_NSP_NRO (default sdmc:/switch/sms/ and the
#                         standalone NRO if built, else sms.nro). The keys are your
#                         console's (Lockpick_RCM); only their path is remembered
#                         (.switch-keys); SMS_SWITCH_KEYS= turns it off. With the
#                         Eclipse build, also build/nsp/sms-eclipse.nsp, starting
#                         SMS_SWITCH_ECLIPSE_NSP_NRO (default sdmc:/switch/sms/
#                         sms-eclipse-standalone.nro if bundled, else
#                         sdmc:/switch/sms-eclipse/sms-eclipse.nro).
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

# The texture pack for sms-standalone.nro, remembered in .bundled-textures
# (Docker sees it at /textures).
textures_memory="$here/.bundled-textures"
if [[ -z "${SMS_SWITCH_TEXTURES+set}" && -f "$textures_memory" ]]; then
	SMS_SWITCH_TEXTURES="$(cat "$textures_memory")"
fi
docker_textures=()
if [[ -n "${SMS_SWITCH_TEXTURES:-}" ]]; then
	textures="$(realpath "$SMS_SWITCH_TEXTURES" 2>/dev/null || true)"
	[[ -d "$textures" ]] || { echo "build.sh: no texture pack at $SMS_SWITCH_TEXTURES (SMS_SWITCH_TEXTURES= stops bundling it)" >&2; exit 1; }
	if [[ "${SMS_SWITCH_NATIVE:-0}" == 1 ]]; then
		disc_args+=(-DSMS_SWITCH_BUNDLE_TEXTURES="$textures")
	else
		disc_args+=(-DSMS_SWITCH_BUNDLE_TEXTURES=/textures)
		docker_textures=(-v "$textures:/textures:ro")
	fi
	printf '%s\n' "$textures" > "$textures_memory"
elif [[ -n "${SMS_SWITCH_TEXTURES+set}" ]]; then
	disc_args+=(-DSMS_SWITCH_BUNDLE_TEXTURES=)
	rm -f "$textures_memory"
fi

# sms-eclipse.nro, remembered in .eclipse, and the disc image for
# sms-eclipse-standalone.nro, in .bundled-eclipse-disc (Docker sees it at
# /eclipse-disc/disc.<ext>).
eclipse_memory="$here/.eclipse" eclipse_remember="$here/.bundled-eclipse-disc"
if [[ -z "${SMS_SWITCH_ECLIPSE_DISC+set}" && -f "$eclipse_remember" ]]; then
	SMS_SWITCH_ECLIPSE_DISC="$(cat "$eclipse_remember")"
fi
if [[ -z "${SMS_SWITCH_ECLIPSE+set}" && ( -f "$eclipse_memory" || -n "${SMS_SWITCH_ECLIPSE_DISC:-}" ) ]]; then
	SMS_SWITCH_ECLIPSE=1
fi
eclipse_disc_args=() docker_eclipse_disc=()
eclipse_icon_memory="$here/.eclipse-icon"
if [[ -z "${SMS_SWITCH_ECLIPSE_ICON+set}" && -f "$eclipse_icon_memory" ]]; then
	SMS_SWITCH_ECLIPSE_ICON="$(cat "$eclipse_icon_memory")"
fi
if [[ "${SMS_SWITCH_ECLIPSE:-0}" == 1 ]]; then
	touch "$eclipse_memory"
	if [[ -n "${SMS_SWITCH_ECLIPSE_DISC:-}" ]]; then
		edisc="$(realpath "$SMS_SWITCH_ECLIPSE_DISC" 2>/dev/null || true)"
		[[ -f "$edisc" ]] || { echo "build.sh: no disc image at $SMS_SWITCH_ECLIPSE_DISC (SMS_SWITCH_ECLIPSE_DISC= stops bundling)" >&2; exit 1; }
		eext="${edisc##*.}"
		eext="${eext,,}"
		if [[ "${SMS_SWITCH_NATIVE:-0}" == 1 ]]; then
			eclipse_disc_args=(-DSMS_SWITCH_BUNDLE_DISC="$edisc")
		else
			eclipse_disc_args=(-DSMS_SWITCH_BUNDLE_DISC="/eclipse-disc/disc.$eext")
			docker_eclipse_disc=(-v "$edisc:/eclipse-disc/disc.$eext:ro")
		fi
		printf '%s\n' "$edisc" > "$eclipse_remember"
	else
		eclipse_disc_args=(-DSMS_SWITCH_BUNDLE_DISC=)
		rm -f "$eclipse_remember"
	fi
	if [[ -n "${SMS_SWITCH_ECLIPSE_ICON:-}" ]]; then
		eicon="$(realpath "$SMS_SWITCH_ECLIPSE_ICON" 2>/dev/null || true)"
		[[ -f "$eicon" && "${eicon,,}" =~ \.jpe?g$ ]] || { echo "build.sh: no JPEG at $SMS_SWITCH_ECLIPSE_ICON (SMS_SWITCH_ECLIPSE_ICON= stops it)" >&2; exit 1; }
		if [[ "${SMS_SWITCH_NATIVE:-0}" == 1 ]]; then
			eclipse_disc_args+=(-DSMS_SWITCH_ICON="$eicon")
		else
			eclipse_disc_args+=(-DSMS_SWITCH_ICON=/eclipse-icon/icon.jpg)
			docker_eclipse_disc+=(-v "$eicon:/eclipse-icon/icon.jpg:ro")
		fi
		printf '%s\n' "$eicon" > "$eclipse_icon_memory"
	else
		eclipse_disc_args+=(-DSMS_SWITCH_ICON=steamgriddb)
		rm -f "$eclipse_icon_memory"
	fi
else
	rm -f "$eclipse_memory" "$eclipse_remember"
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
	# And Super Mario Eclipse's, another title, with the Eclipse build: its
	# standalone NRO beside Sunshine's (the disc is inside it), else
	# sms-eclipse.nro in its own folder (beside the Eclipse disc image).
	if [[ "${SMS_SWITCH_ECLIPSE:-0}" == 1 ]]; then
		if [[ -n "${SMS_SWITCH_ECLIPSE_NSP_NRO:-}" ]]; then
			eclipse_nro_path="$SMS_SWITCH_ECLIPSE_NSP_NRO"
		elif [[ -n "${SMS_SWITCH_ECLIPSE_DISC:-}" ]]; then
			eclipse_nro_path="sdmc:/switch/sms/sms-eclipse-standalone.nro"
		else
			eclipse_nro_path="sdmc:/switch/sms-eclipse/sms-eclipse.nro"
		fi
		nsp_cmd+="; icon=$src/build/eclipse/icon.jpg; [ -f \$icon ] || icon=$icon_default; \
			$src/tools/make-nsp.sh ${keys_in@Q} ${eclipse_nro_path@Q} \$icon ${version@Q} \
			'Super Mario Eclipse' 05534D5345000000 sms-eclipse"
	fi
elif [[ -n "${SMS_SWITCH_KEYS+set}" ]]; then
	rm -f "$keys_memory" "$here/build/nsp/sms-switch.nsp" "$here/build/nsp/sms-eclipse.nsp"
fi

# The build's name, the log's first line and on the screen at start: git
# describe (the last tag, commits since, the commit), + when built with
# changes not committed (the sms-port submodule's own patches aside), and the
# date (UTC)
build_name="$(git -C "$here" describe --tags --always 2>/dev/null || echo unknown)"
if [[ -n "$(git -C "$here" status --porcelain --ignore-submodules=dirty 2>/dev/null)" ]]; then
	build_name+="+"
fi
build_name+=" $(date -u +%Y-%m-%d)"
disc_args+=(-DSMS_SWITCH_BUILD_NAME="$build_name")
eclipse_disc_args+=(-DSMS_SWITCH_BUILD_NAME="$build_name")

# The Eclipse build: its own tree, build/eclipse, beside the plain one, with
# the same Mesa (build/mesa from tools/mesa/build-mesa.sh, when it is there).
eclipse_cmd=""
if [[ "${SMS_SWITCH_ECLIPSE:-0}" == 1 ]]; then
	eclipse_cmd="cmake -S /src -B /src/build/eclipse -G Ninja -DCMAKE_TOOLCHAIN_FILE=/opt/devkitpro/cmake/Switch.cmake \\
	-DCMAKE_BUILD_TYPE=Release -DSMS_ECLIPSE=ON -DSMS_SWITCH_MESA_DIR=/src/build/mesa -DSMS_COMPILER_CACHE=\$cache \\
	${eclipse_disc_args[*]@Q} $*
cmake --build /src/build/eclipse -j $jobs"
fi

# ccache, where the toolchain has it (the sms-switch-build image does).
inner="set -e
cache=\$(command -v ccache || true)
cmake -S /src -B /src/build -G Ninja -DCMAKE_TOOLCHAIN_FILE=/opt/devkitpro/cmake/Switch.cmake -DCMAKE_BUILD_TYPE=Release \\
	-DSMS_COMPILER_CACHE=\$cache ${disc_args[*]@Q} $*
cmake --build /src/build -j $jobs
$eclipse_cmd
[ -z \"\$cache\" ] || ccache -s | grep -E 'Hits|Misses' | head -2
$nsp_cmd"

if [[ "${SMS_SWITCH_NATIVE:-0}" == 1 ]]; then
	: "${DEVKITPRO:?set DEVKITPRO to your devkitPro install}"
	cmake -S "$here" -B "$here/build" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/Switch.cmake" \
		-DCMAKE_BUILD_TYPE=Release -DSMS_COMPILER_CACHE="$(command -v ccache || true)" "${disc_args[@]}" "$@"
	cmake --build "$here/build" -j "$jobs"
	if [[ "${SMS_SWITCH_ECLIPSE:-0}" == 1 ]]; then
		cmake -S "$here" -B "$here/build/eclipse" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/Switch.cmake" \
			-DCMAKE_BUILD_TYPE=Release -DSMS_ECLIPSE=ON -DSMS_SWITCH_MESA_DIR="$here/build/mesa" \
			-DSMS_COMPILER_CACHE="$(command -v ccache || true)" \
			"${eclipse_disc_args[@]}" "$@"
		cmake --build "$here/build/eclipse" -j "$jobs"
	fi
	[[ -z "$nsp_cmd" ]] || bash -c "$nsp_cmd"
else
	if [[ "$image" == "$build_image" ]] && ! docker image inspect "$image" >/dev/null 2>&1; then
		echo "build.sh: building the $image image (devkitA64 + ccache + Pillow)"
		docker build -q -t "$image" "$here/tools/docker" >/dev/null
	fi
	mkdir -p "$ccache_dir"
	docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp \
		-v "$ccache_dir:/ccache" -e CCACHE_DIR=/ccache -e CCACHE_BASEDIR=/src -e CCACHE_MAXSIZE=5G \
		${SMS_SWITCH_DOCKER_ARGS:-} "${docker_disc[@]}" "${docker_textures[@]}" "${docker_eclipse_disc[@]}" "${docker_keys[@]}" \
		-v "$here:/src" -w /src "$image" bash -c "$inner"
fi
echo "build.sh: built $here/build/sms.nro"
if [[ -n "$nsp_cmd" && -f "$here/build/nsp/sms-switch.nsp" ]]; then
	echo "build.sh: built $here/build/nsp/sms-switch.nsp (starts $nro_path; install it with Goldleaf or DBI)"
fi
if [[ -f "$here/build/sms-standalone.nro" && -f "$remember" ]]; then
	echo "build.sh: built $here/build/sms-standalone.nro with $(cat "$remember") inside (personal use only)"
fi
if [[ -f "$here/build/eclipse/sms-eclipse.nro" && "${SMS_SWITCH_ECLIPSE:-0}" == 1 ]]; then
	echo "build.sh: built $here/build/eclipse/sms-eclipse.nro (Super Mario Eclipse; copy it to sdmc:/switch/sms-eclipse/)"
fi
if [[ -f "$here/build/eclipse/sms-eclipse-standalone.nro" && -f "$eclipse_remember" ]]; then
	echo "build.sh: built $here/build/eclipse/sms-eclipse-standalone.nro with $(cat "$eclipse_remember") inside (personal use only)"
fi
if [[ -n "$nsp_cmd" && "${SMS_SWITCH_ECLIPSE:-0}" == 1 && -f "$here/build/nsp/sms-eclipse.nsp" ]]; then
	echo "build.sh: built $here/build/nsp/sms-eclipse.nsp (starts $eclipse_nro_path; install it with Goldleaf or DBI)"
fi
