#!/usr/bin/env bash
# Build the simulated-device helper and the Windows USBIO test client into ../out/.
# The simulated helper needs libusb's header only (tests/libusb_mock.c replaces the library).
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p out
if [ -n "${LIBUSB_INCLUDE:-}" ]; then
  USB_CFLAGS="-I$LIBUSB_INCLUDE"
elif command -v pkg-config >/dev/null && pkg-config --exists libusb-1.0; then
  USB_CFLAGS=$(pkg-config --cflags libusb-1.0)
elif [ -d /opt/homebrew/include/libusb-1.0 ]; then
  USB_CFLAGS=-I/opt/homebrew/include/libusb-1.0
elif [ -n "${LIBUSB_PREFIX:-}" ]; then   # e.g. Ubuntu's libusb-1.0-0-dev unpacked with dpkg -x
  USB_CFLAGS="-I$LIBUSB_PREFIX/usr/include/libusb-1.0"
else
  echo "libusb-1.0 header not found (set LIBUSB_INCLUDE or LIBUSB_PREFIX)" >&2; exit 1
fi
${CC:-cc} -O2 -g -Wall -Wextra -Wno-unused-parameter -std=gnu11 $USB_CFLAGS \
  -o out/usbio-helper-mock helper.c usbio.c tests/libusb_mock.c -lpthread -lm
zig cc -target x86-windows-gnu -O2 -Wall -o out/usbio_client.exe tests/usbio_client.c -lsetupapi -lpowrprof
echo "built out/usbio-helper-mock out/usbio_client.exe"
