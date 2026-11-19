#!/usr/bin/env bash
# Build the simulated-device helper and the Windows USBIO test client into ../out/.
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p out
INC=${LIBUSB_INCLUDE:-$( (pkg-config --cflags-only-I libusb-1.0 2>/dev/null | sed 's/-I//') || echo /opt/homebrew/include/libusb-1.0)}
[ -f "$INC/libusb.h" ] || INC=/opt/homebrew/include/libusb-1.0
${CC:-cc} -O2 -g -Wall -Wextra -Wno-unused-parameter -std=gnu11 -I"$INC" \
  -o out/usbio-helper-mock helper.c usbio.c tests/libusb_mock.c -lpthread
zig cc -target x86-windows-gnu -O2 -Wall -o out/usbio_client.exe tests/usbio_client.c -lsetupapi -lpowrprof
echo "built out/usbio-helper-mock out/usbio_client.exe"
