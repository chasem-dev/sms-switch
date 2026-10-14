#!/usr/bin/env bash
# Build devkitPro's switch-mesa (Mesa 20.1.0-rc3 with their Switch patches)
# with the disk shader cache turned on (sms-switch-shader-cache.patch), into
# build/mesa/ (lib/, include/), which CMakeLists.txt links instead of the
# installed switch-mesa when it is there.
#
# Mesa compiles every shader the game uses (GLSL, then nouveau's GPU code);
# on the console that took 2-5 s per area. With the cache it happens once:
# entries go to XDG_CACHE_HOME/mesa_shader_cache (sdmc:/switch/sms/cache/).
#
#   tools/mesa/build-mesa.sh        (Docker; about 10 minutes the first time)
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
work="$here/build/mesa-src"
mesa_url=https://archive.mesa3d.org/older-versions/20.x/mesa-20.1.0-rc3.tar.xz
mesa_sha256=c90b75ea34302ebde9b81b87c5642fa864c40fe9c4ad34ce0793170c1413168d
dkp_commit=c7876f72c1f3353725ab8468f0ef9180280a8c31
dkp_raw=https://raw.githubusercontent.com/devkitPro/pacman-packages/$dkp_commit/switch/mesa
image=sms-switch-mesa:$(sha256sum "$here/tools/mesa/Dockerfile" | cut -c1-12)

mkdir -p "$work"; rm -f "$work/build.log"
cd "$work"
if [[ ! -f mesa.tar.xz ]] || ! echo "$mesa_sha256  mesa.tar.xz" | sha256sum -c --quiet; then
	curl -sfL -o mesa.tar.xz "$mesa_url"
	echo "$mesa_sha256  mesa.tar.xz" | sha256sum -c --quiet
fi
for f in switch-mesa-20.1.0-5.patch gl_XML.py.patch glX_XML.py.patch; do
	[[ -f "$f" ]] || curl -sfL -o "$f" "$dkp_raw/$f"
done
rm -rf src && mkdir src && tar -xf mesa.tar.xz -C src --strip-components=1
patch -d src -p1 -s -i ../switch-mesa-20.1.0-5.patch
patch -d src -p1 -s -i ../gl_XML.py.patch
patch -d src -p1 -s -i ../glX_XML.py.patch
patch -d src -p1 -s -i "$here/tools/mesa/sms-switch-shader-cache.patch"
# The cache's identity: this exact source (a rebuilt, different Mesa never
# reads another's entries).
id="sms-switch-mesa-20.1.0-rc3-dkp5-$(cat switch-mesa-20.1.0-5.patch "$here/tools/mesa/sms-switch-shader-cache.patch" | sha256sum | cut -c1-16)"
sed -i "s|= MESA_SWITCH_CACHE_ID;|= \"$id\";|" src/src/util/disk_cache.h
grep -q "\"$id\"" src/src/util/disk_cache.h

docker image inspect "$image" >/dev/null 2>&1 || docker build -q -t "$image" "$here/tools/mesa" >/dev/null
docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp -v "$here:/src" -w /src/build/mesa-src/src "$image" bash -c "
	set -e
	/opt/devkitpro/meson-cross.sh switch ../crossfile.txt build -Db_ndebug=true -Dshader-cache=true >>/src/build/mesa-src/build.log 2>&1
	ninja -C build -j$(nproc) >>/src/build/mesa-src/build.log 2>&1
	DESTDIR=/src/build/mesa-src/install ninja -C build install >>/src/build/mesa-src/build.log 2>&1
"
rm -rf "$here/build/mesa"
mkdir -p "$here/build/mesa"
cp -r "$work/install/opt/devkitpro/portlibs/switch/lib" "$work/install/opt/devkitpro/portlibs/switch/include" "$here/build/mesa/"
echo "$id" > "$here/build/mesa/cache-id"
echo "build-mesa: built build/mesa (cache id $id)"
ls "$here/build/mesa/lib"
