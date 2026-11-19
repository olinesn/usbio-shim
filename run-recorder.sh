#!/usr/bin/env bash
# Start RECORDER USGH under Wine with usbio-shim + usbio-helper (real hardware).
#   WINE_BIN=/path/to/wine/bin WINEPREFIX=/path/to/prefix ./run-recorder.sh [rec_usgh.exe arguments]
# Default arguments: /CFG=avisoft_config.ini /AUT (what usv-playpen uses).
# Env: HELPER_ARGS (e.g. "-o MASTERSERIAL,SLAVESERIAL"), USBIO_SHIM_LOG, USBIO_HELPER_DUMP.
# The helper runs as long as RECORDER does and is stopped when it exits.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
export PATH="${WINE_BIN:?set WINE_BIN to the folder holding wine}:$PATH"
: "${WINEPREFIX:?set WINEPREFIX}"
export WINEPREFIX WINEDLLOVERRIDES="${WINEDLLOVERRIDES:-winemenubuilder.exe=d}"
app="$WINEPREFIX/drive_c/Program Files (x86)/Avisoft Bioacoustics/RECORDER USGH"
[ -f "$app/rec_usgh.exe" ] || { echo "RECORDER USGH is not installed in $WINEPREFIX" >&2; exit 1; }
cmp -s "$here/out/powrprof.dll" "$app/powrprof.dll" || cp "$here/out/powrprof.dll" "$app/"
wine reg add 'HKCU\Software\Wine\AppDefaults\rec_usgh.exe\DllOverrides' /v powrprof /d native,builtin /f >/dev/null 2>&1
"$here/out/usbio-helper" ${HELPER_ARGS:-} &
hp=$!
trap 'kill $hp 2>/dev/null' EXIT
sleep 0.3
cd "$app"
if [ $# -eq 0 ]; then set -- /CFG=avisoft_config.ini /AUT; fi
wine rec_usgh.exe "$@"
