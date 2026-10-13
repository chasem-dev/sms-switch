#!/usr/bin/env bash
# Run build/sms.nro in Eden on this machine's GPU with no window (SDL's
# offscreen driver over EGL), memory-capped. Pictures come from the game
# itself: put SMS_SHOTS=... in WORK/.../sdmc/switch/sms/env.txt (see run.sh
# for the layout); they are written as PPM to sdmc/switch/sms/shots.
#
#   tools/emu/run-host.sh EDEN_DIR DISC_IMAGE WORK_DIR [SECONDS]
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
eden="$(cd "$1" && pwd)"; disc="$(realpath "$2")"; work="$(mkdir -p "$3" && cd "$3" && pwd)"
secs="${4:-120}"
sd="$work/home/.local/share/eden/sdmc/switch/sms"
mkdir -p "$sd/shots" "$work/home/.config/eden"
cp "$here/build/sms.nro" "$sd/sms.nro"
if [[ ! "$sd/game.iso" -ef "$disc" ]]; then
	rm -f "$sd/game.iso"
	ln "$disc" "$sd/game.iso" 2>/dev/null || cp "$disc" "$sd/game.iso"
fi
rm -f "$sd/sms.log" "$sd/shots/"*
HOME="$work/home" SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-offscreen}" SDL_AUDIODRIVER=dummy \
	timeout --signal=INT "$secs" systemd-run --user --scope --quiet -p MemoryMax=8G \
	"$eden/AppRun" eden-cli "$sd/sms.nro" >"$work/eden.log" 2>&1 || true
echo "log: $sd/sms.log  emulator: $work/eden.log  shots: $sd/shots"
