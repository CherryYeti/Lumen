#!/usr/bin/env bash
# Adds Lumen to the desktop's app launcher (~/.local/share/applications).
# Remove with: rm ~/.local/share/applications/dev.lumen.Control.desktop
set -euo pipefail
GUI="$(cd "$(dirname "$0")" && pwd)/lumen_gui.py"
DEST="$HOME/.local/share/applications/dev.lumen.Control.desktop"
mkdir -p "$(dirname "$DEST")"
cat > "$DEST" <<ENTRY
[Desktop Entry]
Type=Application
Name=Lumen
Comment=Stream SteamVR to a phone or Gear VR
Exec=/usr/bin/python3 $GUI
Icon=applications-games
Terminal=false
Categories=Game;Utility;
StartupWMClass=dev.lumen.Control
ENTRY
echo "Installed $DEST"
