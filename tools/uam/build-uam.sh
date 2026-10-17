#!/usr/bin/env bash
# Build libuam.a, deko3d's shader compiler (devkitPro's uam) as a library, for
# the Switch into build/uam/ (lib/, include/) and for this machine into
# build/uam-host/ (the shader tests), which CMakeLists.txt links for the
# deko3d renderer.
#
# The game makes its GX shaders while it runs, and deko3d takes only compiled
# GPU code, so the compiler runs in the game: uam-library.patch builds uam
# without its command line and adds uam_api.h (GLSL in, a DKSH module in
# memory out). uam-scheduling.patch fixes the Maxwell scheduler's stalls at
# the ends of blocks (code that read registers too early on the console).
#
#   tools/uam/build-uam.sh        (Docker; about 2 minutes)
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
work="$here/build/uam-src"
uam_url=https://github.com/devkitPro/uam.git
uam_commit=5a5afc2bae8b55409ab36ba45be63fcb73f68993 # 1.1.0
image=sms-switch-mesa:$(sha256sum "$here/tools/mesa/Dockerfile" | cut -c1-12)

mkdir -p "$work"
cd "$work"
if [[ ! -d src/.git ]]; then
	git clone -q "$uam_url" src
fi
git -C src fetch -q origin "$uam_commit" 2>/dev/null || true
git -C src checkout -q -f "$uam_commit"
git -C src clean -qfdx
git -C src apply "$here/tools/uam/uam-library.patch"
git -C src apply "$here/tools/uam/uam-scheduling.patch"
git -C src apply "$here/tools/uam/uam-no-dual-issue.patch"

docker image inspect "$image" >/dev/null 2>&1 || docker build -q -t "$image" "$here/tools/mesa" >/dev/null
rm -rf "$work/switch" "$work/host"
docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp -v "$here:/src" -w /src/build/uam-src/src "$image" bash -c "
	set -e
	/opt/devkitpro/meson-cross.sh switch ../crossfile.txt ../switch -Dlibrary=true -Dstrip=false >../build.log 2>&1
	ninja -C ../switch >>../build.log 2>&1
	# uam carries its own Mesa (19.0), and the game links devkitPro's (20.1):
	# one object in which every symbol uam defines but uam_compile_dksh is
	# renamed (uam__...), so the two never meet, not even in the inline
	# functions both define, which the linker would otherwise take once
	cd ../switch
	bin=/opt/devkitpro/devkitA64/bin/aarch64-none-elf
	\$bin-ld -r --whole-archive libuam.a -o uam-all.o
	\$bin-nm --defined-only uam-all.o | awk '\$3 != \"\" && \$3 != \"uam_compile_dksh\" { print \$3 \" uam__\" \$3 }' | sort -u > rename.txt
	\$bin-objcopy --redefine-syms=rename.txt uam-all.o uam-private.o
	rm -f libuam.a
	\$bin-ar rcs libuam.a uam-private.o
	cd ../src
	meson setup ../host -Dlibrary=true >>../build.log 2>&1
	ninja -C ../host >>../build.log 2>&1
"
for t in switch host; do
	out="$here/build/uam"
	[[ $t == host ]] && out="$here/build/uam-host"
	rm -rf "$out"
	mkdir -p "$out/lib" "$out/include"
	cp "$work/$t/libuam.a" "$out/lib/"
	cp "$work/src/source/uam_api.h" "$out/include/"
done
echo "build-uam: built build/uam (Switch) and build/uam-host (this machine), uam $uam_commit"
