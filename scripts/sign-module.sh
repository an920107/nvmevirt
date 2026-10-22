#!/bin/bash
# Sign nvmev.ko with the local MOK key used for this lab.
# Usage: sudo ./scripts/sign-module.sh [path-to-ko]
set -euo pipefail

KO=${1:-./nvmev.ko}
KDIR=/usr/src/linux-headers-$(uname -r)
KEY=${MOK_KEY:-/root/MOK.priv}
CRT=${MOK_CERT:-/root/MOK.der}

if [ ! -f "$KEY" ] || [ ! -f "$CRT" ]; then
	echo "Missing $KEY or $CRT" >&2
	echo "Run scripts/mok-enroll.sh first, or disable Secure Boot." >&2
	exit 1
fi

"$KDIR/scripts/sign-file" sha256 "$KEY" "$CRT" "$KO"
echo "signed: $KO"
modinfo "$KO" | grep -E '^(signer|sig_key):' || true
