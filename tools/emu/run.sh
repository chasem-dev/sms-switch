#!/usr/bin/env bash
# Run build/sms.nro in Eden (yuzu-derived emulator) inside the sms-switch-emu
# container, on a virtual X display, with software rendering. No window
# appears on the host. Screenshots land in $WORK/shots every few seconds.
#
#   tools/emu/run.sh EDEN_DIR DISC_IMAGE WORK_DIR [SECONDS]
#
# EDEN_DIR: an extracted Eden AppImage (squashfs-root). WORK_DIR keeps the
# emulator's home (virtual SD card, config, logs) between runs.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
eden="$(cd "$1" && pwd)"; disc="$(realpath "$2")"; work="$(mkdir -p "$3" && cd "$3" && pwd)"
secs="${4:-120}"
sd="$work/home/.local/share/eden/sdmc/switch/sms"
mkdir -p "$sd" "$work/shots"
cp "$here/build/sms.nro" "$sd/sms.nro"
# The disc image goes on the virtual SD card as a hard link (no copy) when it
# is on the same file system, else as a copy.
if [[ ! "$sd/game.iso" -ef "$disc" ]]; then
	rm -f "$sd/game.iso"
	ln "$disc" "$sd/game.iso" 2>/dev/null || cp "$disc" "$sd/game.iso"
fi
rm -f "$sd/sms.log" "$work/shots/"*.png
docker run --rm --name sms-switch-emu --memory 6g --cpus 4 \
	--user "$(id -u):$(id -g)" -e HOME=/work/home \
	-v "$eden:/emu:ro" -v "$work:/work" \
	sms-switch-emu bash -c '
		Xvfb :99 -screen 0 1280x720x24 -nolisten tcp >/work/xvfb.log 2>&1 &
		export DISPLAY=:99 SDL_AUDIODRIVER=dummy LIBGL_ALWAYS_SOFTWARE=1 MESA_GL_VERSION_OVERRIDE=4.6COMPAT MESA_GLSL_VERSION_OVERRIDE=460
		sleep 1
		mkdir -p /work/home/.config/eden
		/emu/AppRun eden-cli /work/home/.local/share/eden/sdmc/switch/sms/sms.nro >/work/eden.log 2>&1 &
		pid=$!
		for i in $(seq 1 '"$secs"'); do
			sleep 1
			kill -0 $pid 2>/dev/null || { echo "emulator exited at ${i}s"; break; }
			if (( i % 5 == 0 )); then xwd -root -silent | convert xwd:- /work/shots/t$(printf %04d $i).png 2>/dev/null || true; fi
		done
		kill $pid 2>/dev/null; wait $pid 2>/dev/null; true'
echo "log: $sd/sms.log  emulator: $work/eden.log  shots: $work/shots"
