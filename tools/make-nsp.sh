#!/usr/bin/env bash
# Pack build/sms-switch.nsp: an installable title (its own HOME menu icon,
# full application memory) that starts an NRO from the SD card. Its program is
# nx-hbloader (third_party/, the homebrew menu's own loader) set to load that
# NRO, so the game starts exactly as from the homebrew menu.
#
# Runs in the build image (hacbrewpack, devkitA64); build.sh calls it when
# SMS_SWITCH_KEYS is set:
#   tools/make-nsp.sh KEYS NRO_PATH_ON_SD ICON_JPG VERSION
# KEYS is your console's prod.keys (Lockpick_RCM); it is only read here.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
keys="$1" nro="$2" icon="$3" version="$4"
title_id=05534D5300000000
out="$here/build/nsp"
work="$out/work"
rm -rf "$work"
mkdir -p "$work/exefs" "$work/control" "$work/forwarder"

# The loader, with the game as its first and only NRO.
cp -r "$here/third_party/nx-hbloader/." "$work/forwarder/"
sed -i "s|^#define DEFAULT_NRO .*|#define DEFAULT_NRO \"$nro\"|" "$work/forwarder/source/main.c"
grep -q "DEFAULT_NRO \"$nro\"" "$work/forwarder/source/main.c"
cp "$here/switch/forwarder/sms-forwarder.json" "$work/forwarder/hbl.json"
make -s -C "$work/forwarder" >/dev/null
cp "$work/forwarder/hbl.nso" "$work/exefs/main"
cp "$work/forwarder/hbl.npdm" "$work/exefs/main.npdm"

# Name, version and icon on the HOME menu.
nacptool --create "Super Mario Sunshine" "sms-switch" "$version" "$work/control/control.nacp" >/dev/null
cp "$icon" "$work/control/icon_AmericanEnglish.dat"

hacbrewpack -k "$keys" --titleid "$title_id" --noromfs --nologo \
	--exefsdir "$work/exefs" --controldir "$work/control" \
	--nspdir "$work/nsp" --tempdir "$work/temp" --backupdir "$work/backup" >"$work/hacbrewpack.log" 2>&1 \
	|| { cat "$work/hacbrewpack.log" >&2; exit 1; }
mv "$work/nsp/$(echo "$title_id" | tr A-Z a-z).nsp" "$out/sms-switch.nsp" 2>/dev/null \
	|| mv "$work/nsp/"*.nsp "$out/sms-switch.nsp"
echo "make-nsp: built build/nsp/sms-switch.nsp (title $title_id, starts $nro)"
