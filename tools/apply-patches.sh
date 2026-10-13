#!/usr/bin/env bash
# Apply patches/sms-port/*.patch to the sms-port submodule, once each.
# Each patch adds the Switch's cases to the PC port; they are written so they
# could go upstream unchanged, after which they are dropped here.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
port="$here/sms-port"
shopt -s nullglob
for p in "$here"/patches/sms-port/*.patch; do
	if git -C "$port" apply --reverse --check "$p" 2>/dev/null; then
		continue # already applied
	fi
	if ! git -C "$port" apply --check "$p" 2>/dev/null; then
		echo "apply-patches: $(basename "$p") does not apply to sms-port $(git -C "$port" rev-parse --short HEAD)" >&2
		git -C "$port" apply --check "$p" || true
		exit 1
	fi
	git -C "$port" apply "$p"
	echo "apply-patches: applied $(basename "$p")"
done
