#!/usr/bin/env bash
# Build usbio-shim.
#   ./build.sh            the shim DLL (32-bit Windows, with zig) + the helper for this machine
#   ./build.sh dll        only the DLL
#   ./build.sh helper     only the helper (cc + libusb-1.0 from pkg-config or Homebrew)
# Outputs go to ./out/.
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p out
what=${1:-all}

if [ "$what" = all ] || [ "$what" = dll ]; then
  zig cc -target x86-windows-gnu -O2 -shared -Wall -Wno-unused-function \
    -o out/powrprof.dll shim.c powrprof.def -lws2_32
  echo "built out/powrprof.dll"
fi

if [ "$what" = all ] || [ "$what" = helper ]; then
  if command -v pkg-config >/dev/null && pkg-config --exists libusb-1.0; then
    USB_CFLAGS=$(pkg-config --cflags libusb-1.0); USB_LIBS=$(pkg-config --libs libusb-1.0)
  elif [ -d /opt/homebrew/include/libusb-1.0 ]; then
    USB_CFLAGS=-I/opt/homebrew/include/libusb-1.0; USB_LIBS="-L/opt/homebrew/lib -lusb-1.0"
  elif [ -n "${LIBUSB_PREFIX:-}" ]; then   # e.g. Ubuntu's libusb-1.0-0-dev unpacked with dpkg -x
    USB_CFLAGS="-I$LIBUSB_PREFIX/usr/include/libusb-1.0"
    USB_LIBS="-L$LIBUSB_PREFIX/usr/lib/$(uname -m)-linux-gnu -lusb-1.0"
  else
    echo "libusb-1.0 headers not found (set LIBUSB_PREFIX)" >&2; exit 1
  fi
  ${CC:-cc} -O2 -g -Wall -Wextra -Wno-unused-parameter -std=gnu11 $USB_CFLAGS \
    -o out/usbio-helper helper.c usbio.c $USB_LIBS -lpthread
  echo "built out/usbio-helper"
fi
