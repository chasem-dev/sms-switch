#!/usr/bin/env bash
# Build build/sms.nro for the Nintendo Switch (libnx homebrew), inside the
# devkitpro/devkita64 Docker image, so the host needs only Docker.
#
#   ./build.sh            configure (first time) and build
#   ./build.sh --clean    delete build/ first
#   JOBS=2 ./build.sh     limit parallel compiler jobs (default: all cores)
#   SMS_SWITCH_NATIVE=1   use an installed devkitPro (DEVKITPRO) instead of Docker
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
image="${SMS_SWITCH_IMAGE:-devkitpro/devkita64:latest}"
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

inner="cmake -S /src -B /src/build -G Ninja -DCMAKE_TOOLCHAIN_FILE=/opt/devkitpro/cmake/Switch.cmake -DCMAKE_BUILD_TYPE=Release $* \
  && cmake --build /src/build -j $jobs"

if [[ "${SMS_SWITCH_NATIVE:-0}" == 1 ]]; then
	: "${DEVKITPRO:?set DEVKITPRO to your devkitPro install}"
	cmake -S "$here" -B "$here/build" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/Switch.cmake" \
		-DCMAKE_BUILD_TYPE=Release "$@"
	cmake --build "$here/build" -j "$jobs"
else
	docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp \
		${SMS_SWITCH_DOCKER_ARGS:-} \
		-v "$here:/src" -w /src "$image" bash -c "$inner"
fi
echo "build.sh: built $here/build/sms.nro"
