#!/bin/bash
# Installs the OpenRack Virtual Audio driver (macOS). Usage: ./install.sh path/to/OpenRack.driver
# Set IDENTITY to your Developer ID Application certificate name for a properly signed install:
#   IDENTITY="Developer ID Application: Your Name (TEAMID)" ./install.sh OpenRack.driver
set -e
SRC="${1:-OpenRack.driver}"
DEST="/Library/Audio/Plug-Ins/HAL"
codesign --force --sign "${IDENTITY:--}" --timestamp=none "$SRC"
sudo mkdir -p "$DEST"
sudo rm -rf "$DEST/OpenRack.driver"
sudo cp -R "$SRC" "$DEST/"
sudo killall coreaudiod || true
echo "Installed. 'OpenRack Virtual Audio' now appears in Audio MIDI Setup and in OpenRack's audio settings."
