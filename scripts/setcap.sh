#!/usr/bin/env bash
# Grant server_kcm the capability to load its BPF stream parser, so sweeps can
# run as a normal user. File capabilities live on the inode, so this has to be
# re-run after every rebuild of server_kcm.
#
#   sudo scripts/setcap.sh
set -euo pipefail
cd "$(dirname "$0")/.."
BIN=build/server_kcm
[[ -x "$BIN" ]] || { echo "build $BIN first" >&2; exit 1; }
setcap cap_bpf,cap_net_admin+ep "$BIN"
getcap "$BIN"
