#!/usr/bin/env bash
# Build tools/botstart/botstart.nro (botstart.c) in the devkitpro/devkita64
# image and, with --send, run it on the console through the homebrew menu's
# network loader (press Y there), its report printed here.
#   NSP=/path/sys-botbase.nsp   carry that build, installed when it runs
#   ./build.sh --send launch    also start sys-botbase now (see botstart.c)
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ip="${SWITCH_IP:-192.168.50.49}"
image="${SMS_SWITCH_IMAGE:-devkitpro/devkita64:latest}"
romfs=$(mktemp -d)
trap 'rm -rf "$romfs"' EXIT
[[ -n "${NSP:-}" ]] && cp "$NSP" "$romfs/exefs.nsp"
docker run --rm -u "$(id -u):$(id -g)" -v "$here:/w" -v "$romfs:/romfs:ro" -w /w "$image" sh -c '
	set -e
	export PATH=/opt/devkitpro/devkitA64/bin:/opt/devkitpro/tools/bin:$PATH
	aarch64-none-elf-gcc -O2 -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE -D__SWITCH__ \
		-I/opt/devkitpro/libnx/include -specs=/opt/devkitpro/libnx/switch.specs \
		botstart.c -L/opt/devkitpro/libnx/lib -lnx -o /tmp/botstart.elf
	nacptool --create botstart "sms-switch" 1.0 /tmp/botstart.nacp
	elf2nro /tmp/botstart.elf botstart.nro --nacp=/tmp/botstart.nacp --romfsdir=/romfs'
echo "built $here/botstart.nro"
if [[ "${1:-}" == --send ]]; then
	# the loader may not be open yet: try again every few seconds
	for _ in $(seq 200); do
		if docker run --rm --network host -v "$here:/w:ro" "$image" \
			stdbuf -o0 /opt/devkitpro/tools/bin/nxlink -s -a "$ip" -p botstart.nro /w/botstart.nro ${2:+"$2"}; then
			exit 0
		fi
		sleep 3
	done
	exit 1
fi
