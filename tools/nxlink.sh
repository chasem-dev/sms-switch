#!/usr/bin/env bash
# The quick test loop: send build/sms.nro (12 MB) to the Switch over Wi-Fi,
# start it with NAME=value options, and show its log here as it runs.
#
# On the Switch, once per run: open the homebrew menu through a game (hold R
# while starting one), then press Y for its network loader. Then here:
#   tools/nxlink.sh                          build/sms.nro as it is
#   tools/nxlink.sh SMS_GX_SCALE=2           with options (any env.txt switch)
#   SWITCH_IP=192.168.1.20 tools/nxlink.sh   skip the network search
#
# It goes to sdmc:/switch/sms/sms.nro, beside sms-standalone.nro, whose
# disc image it uses when switch/sms/ has no .iso of its own. Ctrl+C stops
# the log here; the game keeps running until it is closed on the Switch.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
[[ -f "$here/build/sms.nro" ]] || { echo "nxlink.sh: build/sms.nro is missing; run ./build.sh" >&2; exit 1; }
image="${SMS_SWITCH_IMAGE:-devkitpro/devkita64:latest}"
address=()
[[ -n "${SWITCH_IP:-}" ]] && address=(-a "$SWITCH_IP")
# The host network: nxlink finds the Switch by UDP broadcast, and the game's
# log connects back to it.
exec docker run --rm -i --network host -v "$here/build:/b:ro" "$image" \
	/opt/devkitpro/tools/bin/nxlink -s -p sms/sms.nro "${address[@]}" /b/sms.nro "$@"
