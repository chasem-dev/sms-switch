#!/usr/bin/env bash
# Build deko3d (release libdeko3d.a and checked libdeko3dd.a) from source at
# the version devkitPro ships, with tools/deko3d/*.patch, into build/deko3d/
# for CMakeLists.txt to link in place of the installed one.
#
# deko3d-alpha-dst-factor.patch: its blend state took the colour's
# destination factor for alpha (the game's shadows drew solid).
# deko3d-texture-cache-invalidate.patch: its texture cache invalidation
# with a wait for idle wrote a register the GPU does not have.
#
# Building deko3d takes fincs' dekotools (dekodef: the GPU register
# definitions to headers), built here too.
#
#   tools/deko3d/build-deko3d.sh        (Docker; about a minute)
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
work="$here/build/deko3d-src"
url=https://github.com/devkitPro/deko3d.git
commit=5cc144dcf5606fbcd78430f6c62923546e5f9cf4 # v0.5.0
tools_url=https://github.com/fincs/dekotools.git
tools_commit=c15523cea963c04fad77a46f23c3a96d26a86ab9
# devkitPro's compilers, and meson, flex and bison for dekotools (uam's image)
image=sms-switch-mesa:$(sha256sum "$here/tools/mesa/Dockerfile" | cut -c1-12)
docker image inspect "$image" >/dev/null 2>&1 || docker build -q -t "$image" "$here/tools/mesa" >/dev/null

mkdir -p "$work"
if [[ ! -d "$work/src/.git" ]]; then
	git clone -q "$url" "$work/src"
fi
git -C "$work/src" fetch -q origin "$commit" 2>/dev/null || true
git -C "$work/src" checkout -q -f "$commit"
git -C "$work/src" clean -qfdx
for p in "$here"/tools/deko3d/*.patch; do
	git -C "$work/src" apply "$p"
done
if [[ ! -d "$work/dekotools/.git" ]]; then
	git clone -q "$tools_url" "$work/dekotools"
fi
git -C "$work/dekotools" fetch -q origin "$tools_commit" 2>/dev/null || true
git -C "$work/dekotools" checkout -q -f "$tools_commit"
docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp -v "$here:/src" -w /src/build/deko3d-src "$image" bash -c '
	set -e
	rm -rf tools-build
	meson setup tools-build dekotools >build.log 2>&1
	ninja -C tools-build >>build.log 2>&1
	export PATH=/src/build/deko3d-src/tools-build:/opt/devkitpro/tools/bin:$PATH DEVKITPRO=/opt/devkitpro
	make -C src -j"$(nproc)" >>build.log 2>&1'
rm -rf "$here/build/deko3d"
mkdir -p "$here/build/deko3d/lib"
cp "$work/src/lib/libdeko3d.a" "$work/src/lib/libdeko3dd.a" "$here/build/deko3d/lib/"
echo "build-deko3d: built build/deko3d (deko3d $commit with $(ls "$here"/tools/deko3d/*.patch | wc -l) patch(es))"
