#!/usr/bin/env bash
# Build and load the kdispatch module.
#   sudo scripts/load_module.sh          # load (rebuilds if needed)
#   sudo scripts/load_module.sh unload
set -euo pipefail
cd "$(dirname "$0")/.."

if [[ "${1:-load}" == "unload" ]]; then
  rmmod kdispatch 2>/dev/null || true
  echo "unloaded"; exit 0
fi

make -C module >/dev/null
rmmod kdispatch 2>/dev/null || true
insmod module/kdispatch.ko
ls -l /dev/kdispatch
dmesg | tail -3
