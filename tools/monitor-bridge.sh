#!/usr/bin/env bash
# Live serial monitor from inside the devcontainer, via an RFC2217 bridge on the Mac.
#
# Mac side first (one-time: brew install esptool):
#   esp_rfc2217_server -v -p 4000 /dev/cu.usbmodemXXXX
# The server must be restarted after every board re-plug or reset — and the
# usbmodem name can change, so re-check it (ls /dev/cu.usbmodem*).
#
# Usage: tools/monitor-bridge.sh [host:port]   (default: host.docker.internal:4000)
# Quit with Ctrl-]. Only one bridge client at a time (monitor OR flash, not both).
#
# Note: attaching REBOOTS the board (the bridge re-applies port settings on
# connect and the control-line transitions trip the S3 reset logic), so every
# session starts with a fresh boot log. For a non-disruptive live tail, monitor
# locally on the Mac instead: idf.py monitor --no-reset -p /dev/cu.usbmodemXXXX
set -euo pipefail

BRIDGE="${1:-host.docker.internal:4000}"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ELF="$REPO_ROOT/esp32-agui/build/esp32_agui.elf"

if [ -z "${IDF_PATH:-}" ]; then
  . /opt/esp/idf/export.sh >/dev/null 2>&1
fi

# --no-reset: the reset dance doesn't survive the bridge on this board (native
# USB re-enumerates → the Mac-side server loses the port).
exec python -m esp_idf_monitor --no-reset \
  --port "rfc2217://${BRIDGE}?ign_set_control" \
  $([ -f "$ELF" ] && printf '%s' "$ELF")
