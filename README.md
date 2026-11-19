# usbio-shim

usbio-shim lets Avisoft RECORDER USGH run under Wine on Linux and macOS with
UltraSoundGate xx16H units, for example the 1216H. On Windows these units need
Avisoft's kernel driver `usgh_x64.sys`, a renamed build of Thesycon's USBIO
generic USB driver. Wine cannot load kernel drivers, so usbio-shim stands in
for it with libusb. Avisoft's files are not modified.

**Status (2026-11-19):** tested with simulated units only. Under Wine 11 on
macOS, RECORDER finds two units, configures them, sets the rate and streams from
both; it reports dropouts and writes no WAV, because the bytes are not real
1216H audio. On Linux, CI streams from two simulated units through the shim
under Ubuntu's Wine 9. **Not yet tested with real units.**

## How it works

- **`powrprof.dll`** (`shim.c`) is a 32-bit Windows DLL. It goes in
  `rec_usgh.exe`'s folder and loads with `WINEDLLOVERRIDES=powrprof=n,b`.
  RECORDER imports only `SetSuspendState` from Windows' powrprof, and the shim
  passes that call on. When the DLL loads, it points 12 of RECORDER's imports
  at its own versions:
  - finding the device: `SetupDiGetClassDevsA`, `SetupDiEnumDeviceInterfaces`,
    `SetupDiGetDeviceInterfaceDetailA`, `SetupDiDestroyDeviceInfoList`;
  - opening it: `CreateFileA/W`;
  - talking to it: `DeviceIoControl`, `ReadFile`, `WriteFile`,
    `GetOverlappedResult`, `CancelIo`, `CloseHandle`.

  Calls on other handles go to Wine unchanged. Calls on a unit go over TCP on
  127.0.0.1 (port 47321, or `USBIO_SHIM_PORT`) to the helper.
- **`usbio-helper`** (`helper.c`, `usbio.c`) is a native program built on
  libusb 1.0. It carries out the driver requests (configuration, control
  requests, pipes, isochronous and bulk transfers) on the real units. Options:
  - `-p PORT`: the port to listen on;
  - `-o SERIAL1,SERIAL2`: the unit order RECORDER sees (the first unit is the
    SYNC master). By default units are ordered by USB bus and port;
  - `-v` / `-vv`: more logging.

  Environment variables:
  - `USBIO_HELPER_DUMP=<file>`, with `USBIO_HELPER_DUMP_SECONDS`, records every
    isochronous packet received;
  - `USBIO_HELPER_ISO_BYTES` and `USBIO_HELPER_API_VERSION` switch details
    that could not be settled without real units.
- `proto.h` is the wire protocol between the DLL and the helper.
  `usbio_defs.h` holds the request codes, data layouts and status values
  RECORDER uses.

## Build

```bash
./build.sh            # out/powrprof.dll (needs zig) and out/usbio-helper (cc + libusb-1.0)
./build.sh helper     # only the helper; LIBUSB_PREFIX=<dir> for an unpacked libusb-1.0-0-dev
```

You need [zig](https://ziglang.org/) for the DLL, plus a C compiler and libusb
1.0 for the helper: `sudo apt install libusb-1.0-0-dev` on Ubuntu, or
`brew install libusb` on macOS.

Linux, once: let your user open the units without root.

```bash
sudo cp 99-avisoft-ultrasoundgate.rules /etc/udev/rules.d/
sudo udevadm control --reload && sudo udevadm trigger
sudo usermod -aG plugdev $USER     # then log out and back in
```

Then plug the units in again. macOS needs no such step.

## Wine prefix

Use Wine 11 or newer with 32-bit support (`rec_usgh.exe` is a 32-bit program):
WineHQ's `winehq-stable` on Ubuntu, `brew install --cask wine-stable` on macOS.
Give RECORDER a prefix of its own and install it there. Avisoft's installer is
a self-extracting archive holding `RECORDER USGH.msi`:

```bash
export WINEPREFIX=~/.wine-avisoft
wineboot -u
# A new prefix links Desktop, Documents, ... to your own folders. Replace the
# links with plain folders so the installer's shortcuts stay inside the prefix.
for d in Desktop Documents Downloads Music Pictures Videos Templates; do
    link="$WINEPREFIX/drive_c/users/$USER/$d"
    if [ -L "$link" ]; then unlink "$link"; mkdir "$link"; fi
done
mkdir recorder-installer && unzip RECORDER_USGH.exe -d recorder-installer
cd recorder-installer && WINEDLLOVERRIDES=winemenubuilder.exe=d wine msiexec /i "RECORDER USGH.msi" /qn
```

The installer also stages Avisoft's driver, which goes unused under Wine.

## Run

- By hand: `WINE_BIN=<folder holding wine> WINEPREFIX=<prefix> ./run-recorder.sh`
  copies the DLL, starts the helper, then starts RECORDER with
  `/CFG=avisoft_config.ini /AUT`. Pass other RECORDER arguments after it.
  `HELPER_ARGS` passes options to the helper.
- From [usv-playpen](https://github.com/bartulem/usv-playpen): set
  `avisoft_usbio_shim_directory` to this folder's `out/`. usv-playpen then
  starts the helper and RECORDER for each recording and stops them afterwards.

## Tests (no hardware)

`tests/build-tests.sh` builds two programs:

- `out/usbio-helper-mock` is the helper linked against `tests/libusb_mock.c`,
  which simulates high-speed units streaming in real time:
  - `MOCK_LAYOUT=1216h` gives the 1216H's interfaces;
  - `MOCK_DEVICES=2` simulates two units;
  - `MOCK_ISO_MODE=frames` streams sine audio instead of a byte counter.
- `out/usbio_client.exe` is a test client that runs under Wine and talks to
  the units the way RECORDER does. It checks for gaps in the stream, the data
  rate and cancelling:

  ```bash
  MOCK_DEVICES=2 out/usbio-helper-mock -p 47399 &
  cd out && WINEDLLOVERRIDES=powrprof=n,b USBIO_SHIM_PORT=47399 wine usbio_client.exe 5 2
  ```

`tests/run-recorder.sh` runs RECORDER, installed in a prefix as above, against
the simulated units and summarizes what it did.

`tools/observe_helper.py` answers the shim's requests from a table instead of
a device and logs each one. It shows what RECORDER asks for.

CI builds everything on Linux and macOS, and runs the test client under Wine
on Linux.

## License

GPL-3.0-or-later; see `LICENSE`.

Not affiliated with Avisoft Bioacoustics or Thesycon. RECORDER USGH,
UltraSoundGate and USBIO are their owners' products and trademarks. You need
your own copy of RECORDER USGH.
