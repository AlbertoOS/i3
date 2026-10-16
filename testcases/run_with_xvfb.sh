#!/usr/bin/env bash
set -euo pipefail

XVFB_BIN=${1:?path to Xvfb is required}
shift
[[ $# -gt 0 ]]
umask 077
unset DISPLAY I3SOCK XAUTHORITY
workdir=$(mktemp -d "${TMPDIR:?TMPDIR must name a private scratch directory}/i3-test-xvfb.XXXXXX")
cleanup() {
  status=$?
  trap - EXIT
  if [[ -n ${xvfb_pid:-} ]]; then
    kill "$xvfb_pid" 2> /dev/null || true
    wait "$xvfb_pid" 2> /dev/null || true
  fi
  if ((status == 0)); then
    rm -rf -- "$workdir"
  else
    # Retain only a bounded log tail, privately, for runner-side diagnosis.
    python3 - "$workdir/xvfb.log" << 'PY' || true
import pathlib, sys
path = pathlib.Path(sys.argv[1])
if path.exists():
    with path.open("rb") as stream:
        stream.seek(max(0, path.stat().st_size - 8192))
        tail = stream.read(8192)
    path.write_bytes(tail)
PY
    printf 'Xvfb test failed (status %s); private diagnostics retained at %s (log <= 8 KiB; remove after review).\n' "$status" "$workdir" >&2
  fi
  exit "$status"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

"$XVFB_BIN" -displayfd 3 -screen 0 1024x768x24 -nolisten tcp -ac \
  3> "$workdir/display" > "$workdir/xvfb.log" 2>&1 &
xvfb_pid=$!
for ((attempt = 0; attempt < 100; attempt++)); do
  [[ -s "$workdir/display" ]] && break
  kill -0 "$xvfb_pid" 2> /dev/null || exit 1
  sleep 0.05
done
[[ -s "$workdir/display" ]]
display_number=$(tr -d '\n' < "$workdir/display")
case "$display_number" in
  '' | *[!0-9]*) exit 1 ;;
esac
DISPLAY=":$display_number" "$@"
