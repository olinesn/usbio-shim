#!/usr/bin/env bash
# Run RECORDER USGH under Wine through the shim against a helper (the simulated device by
# default), for N seconds, then summarize what it did.
#
#   WINE_BIN=... WINEPREFIX=... tests/run-recorder.sh [seconds=25] [helper=out/usbio-helper-mock]
#
# The prefix must have RECORDER USGH installed (README: Wine prefix) and an avisoft_config.ini
# in Documents\Avisoft Bioacoustics\Configurations\RECORDER_USGH.
# MOCK_* environment variables pass through to the simulated device (see tests/libusb_mock.c).
# Output: $OUT (default ./out/recorder-run) holds the shim log, helper log and Wine log.
set -uo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
secs=${1:-25}
helper=${2:-$here/out/usbio-helper-mock}
out=${OUT:-$here/out/recorder-run}
mkdir -p "$out"
export PATH="${WINE_BIN:?set WINE_BIN}:$PATH"
: "${WINEPREFIX:?set WINEPREFIX}"
export WINEDLLOVERRIDES="winemenubuilder.exe=d"
app="$WINEPREFIX/drive_c/Program Files (x86)/Avisoft Bioacoustics/RECORDER USGH"
base=$(ls -d "$WINEPREFIX"/drive_c/users/*/Documents/"Avisoft Bioacoustics" | head -1)
cp "$here/out/powrprof.dll" "$app/"
wine reg add 'HKCU\Software\Wine\AppDefaults\rec_usgh.exe\DllOverrides' /v powrprof /d native,builtin /f >/dev/null 2>&1
rm -f "$out"/*.log
touch "$out/start.stamp"
"$helper" ${HELPER_ARGS:--v} >"$out/helper.log" 2>&1 &
hp=$!
sleep 0.5
winlog=$(printf 'Z:%s' "$out/shim.log" | tr / '\\')
(cd "$app" && USBIO_SHIM_LOG="$winlog" WINEDEBUG=-all,+msgbox wine rec_usgh.exe /CFG=avisoft_config.ini /AUT >"$out/wine.log" 2>&1) &
sleep "$secs"
wineserver -k 2>/dev/null
kill $hp 2>/dev/null
wait 2>/dev/null
echo "== message boxes"; grep -a msgbox "$out/wine.log" | sed 's/.*MSGBOX_OnInit //' | cut -c1-200 || echo "(none)"
echo "== new files under the base directory"
find "$base" -type f -newer "$out/start.stamp" -exec ls -l {} \; | sed "s|$base/||" | awk '{print $5, $NF}'
for ch in ch1 ch13; do
  f="$base/$ch/$ch.log"
  [ -f "$f" ] && echo "$ch.log since start: $(awk -v RS='\r?\n' 'BEGIN{n=0} /Monitoring_started/{n=0} /dropout/{n++} END{print n}' "$f") dropout lines in the last session"
done
echo "== helper"; grep -v 'IOCTL fn' "$out/helper.log" | grep -E 'bound|configuration|lost|vendor|error|failed' | head -20
