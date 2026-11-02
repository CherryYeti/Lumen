#!/usr/bin/env bash
# Register (or with --remove, unregister) the built Lumen driver with SteamVR.
set -euo pipefail

STEAMVR="${STEAMVR:-$HOME/.local/share/Steam/steamapps/common/SteamVR}"
DRIVER_DIR="$(cd "$(dirname "$0")" && pwd)/build/lumen"
VRPATHREG="$STEAMVR/bin/vrpathreg.sh"

if [[ ! -f "$DRIVER_DIR/bin/linux64/driver_lumen.so" ]]; then
    echo "Driver not built. Run: cmake -S driver -B driver/build && cmake --build driver/build" >&2
    exit 1
fi

if [[ "${1:-}" == "--remove" ]]; then
    "$VRPATHREG" removedriver "$DRIVER_DIR"
else
    "$VRPATHREG" adddriver "$DRIVER_DIR"
fi
"$VRPATHREG" show
