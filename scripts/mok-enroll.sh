#!/bin/bash
# Prepare a local Machine Owner Key (MOK) for signing nvmev.ko.
# This only queues the key; the user must complete enrollment in the
# blue MokManager screen on the next boot (requires a VM console).
set -euo pipefail

KEY=${MOK_KEY:-/root/MOK.priv}
CRT=${MOK_CERT:-/root/MOK.der}

if [ ! -f "$KEY" ] || [ ! -f "$CRT" ]; then
	openssl req -new -x509 -newkey rsa:2048 \
		-keyout "$KEY" -outform DER -out "$CRT" \
		-nodes -days 36500 \
		-subj "/CN=Lab1 NVMeVirt Module Signing/"
	chmod 600 "$KEY"
fi

echo "Queuing MOK enrollment for $CRT"
echo "Set a one-time password now; you will type it in MokManager after reboot."
mokutil --import "$CRT"

echo
echo "Next steps:"
echo "  1. reboot"
echo "  2. in MokManager choose: Enroll MOK -> Continue -> Yes -> password -> Reboot"
echo "  3. sudo ./scripts/sign-module.sh && sudo insmod ./nvmev.ko ..."
