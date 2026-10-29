#!/usr/bin/env bash
# Explicit log path: no phone command, privilege change, or hidden log selection.
set -Eeuo pipefail
mode="${1:?usage: raven-phase7-log.sh live|history LOG}"
log="${2:?provide the hardware test log path}"
[[ -f "$log" && ! -L "$log" ]] || { echo 'STOP: missing or symlinked log' >&2; exit 1; }
pattern='launchd_cache|BOOTTASK|AMFI|unsecure|Boot task|failed|error|panic|SIGKILL|FIPS|keybag|backboard|SpringBoard|watchdog|SWEET COW'
case "$mode" in
  live) tail -n 150 -F -- "$log" | grep --line-buffered -aiE "$pattern" ;;
  history) grep -ainE "$pattern" "$log" || [[ $? == 1 ]] ;;
  *) echo 'STOP: expected live or history' >&2; exit 1 ;;
esac
