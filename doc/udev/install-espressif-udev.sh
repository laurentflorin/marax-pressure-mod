#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RULE_FILE="${SCRIPT_DIR}/99-espressif-tty.rules"
RULE_DIR="/etc/udev/rules.d"

if [[ $(id -u) -ne 0 ]]; then
  echo "This script must be run with sudo (or as root)." >&2
  exit 1
fi

install -m 0644 "${RULE_FILE}" "${RULE_DIR}/99-espressif-tty.rules"
udevadm control --reload-rules 2>/dev/null || true
udevadm trigger 2>/dev/null || true

printf '\nInstalled %s\n' "${RULE_DIR}/99-espressif-tty.rules"
printf 'Reloaded udev rules. Unplug and re-plug the ESP32-S3 board, then retry the upload.\n'
