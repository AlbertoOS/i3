#!/usr/bin/env bash
set -euo pipefail

I3_BIN=${1:?path to the isolated i3 binary is required}
I3MSG_BIN=${2:?path to the isolated i3-msg binary is required}
XTERM_BIN=${3:?path to xterm is required}
XVFB_BIN=${4:?path to Xvfb is required}
umask 077
unset DISPLAY I3SOCK XAUTHORITY
workdir=$(mktemp -d "${TMPDIR:?TMPDIR must name a private scratch directory}/i3-sparse-xvfb-smoke.XXXXXX")
socket_path="$workdir/ipc.sock"
phase=startup
cleanup() {
  status=$?
  trap - EXIT
  for pid in ${client_one:-} ${client_two:-} ${i3_pid:-} ${xvfb_pid:-}; do
    kill "$pid" 2> /dev/null || true
  done
  for pid in ${client_one:-} ${client_two:-} ${i3_pid:-} ${xvfb_pid:-}; do
    wait "$pid" 2> /dev/null || true
  done
  if ((status == 0)); then
    rm -rf -- "$workdir"
  else
    # Do not dump tree/log contents into CI output. Keep bounded private tails.
    python3 - "$workdir" << 'PY' || true
import pathlib, sys
root = pathlib.Path(sys.argv[1])
for path in list(root.glob("*.log")) + list(root.glob("*.json")):
    with path.open("rb") as stream:
        stream.seek(max(0, path.stat().st_size - 8192))
        tail = stream.read(8192)
    path.write_bytes(tail)
PY
    printf 'Sparse Xvfb smoke failed during %s (status %s); private diagnostics retained at %s (each log/tree <= 8 KiB; remove after review).\n' "$phase" "$status" "$workdir" >&2
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
export DISPLAY=":$display_number"
mkdir "$workdir/runtime"
export XDG_RUNTIME_DIR="$workdir/runtime"
printf 'font pango:monospace 10\nworkspace_layout default\nipc-socket %s\n' "$socket_path" > "$workdir/config"

# All IPC calls are explicit and bounded; never discover a socket from the root window.
ipc() {
  timeout 2 "$I3MSG_BIN" -s "$socket_path" "$@" 2>> "$workdir/ipc.log"
}
command_ok() {
  phase="$1"
  ipc "$1" > "$workdir/reply.json"
  python3 - "$workdir/reply.json" << 'PY'
import json, sys
replies = json.load(open(sys.argv[1]))
assert isinstance(replies, list) and replies
assert all(reply.get("success") is True for reply in replies)
PY
}

# Return false until the asynchronous X/IPC state satisfies the requested assertion.
cat > "$workdir/check_tree.py" << 'PY'
import json, sys
root = json.load(open(sys.argv[1]))
mode, *args = sys.argv[2:]
nodes, parents = [], {}
def walk(node):
    nodes.append(node)
    for child in node.get("nodes", []) + node.get("floating_nodes", []):
        parents[child["id"]] = node
        walk(child)
walk(root)
def clients(instance):
    return [node for node in nodes if
            (node.get("window_properties") or {}).get("instance") == instance]
one, two = clients("sparse_smoke_one"), clients("sparse_smoke_two")
ok = False
if mode == "managed":
    ok = len(one) == len(two) == 1
elif mode == "destroyed":
    ok = len(one) == 1 and not two
elif mode == "empty":
    ok = not one and not two
elif len(one) == len(two) == 1:
    node = one[0]
    if mode == "layout":
        parent = parents[node["id"]]
        ok = parent["id"] == parents[two[0]["id"]]["id"] and parent["layout"] == args[0]
    elif mode == "border":
        ok = node["border"] == args[0] and node["current_border_width"] == int(args[1])
    elif mode == "floating":
        ok = node["floating"] == args[0]
    elif mode == "fullscreen":
        ok = node["fullscreen_mode"] == int(args[0])
    elif mode == "mark":
        ok = (args[0] in node.get("marks", [])) == (args[1] == "present")
    elif mode == "title":
        ok = node["name"] == args[0]
    elif mode == "width":
        ok = node["rect"]["width"] > int(args[0])
raise SystemExit(0 if ok else 1)
PY
wait_tree() {
  phase="tree: $*"
  for ((attempt = 0; attempt < 100; attempt++)); do
    kill -0 "$i3_pid" 2> /dev/null || return 1
    if ipc -t get_tree > "$workdir/tree.json" \
      && python3 "$workdir/check_tree.py" "$workdir/tree.json" "$@"; then
      return 0
    fi
    sleep 0.05
  done
  return 1
}

"$I3_BIN" -c "$workdir/config" -d all > "$workdir/i3.log" 2>&1 &
i3_pid=$!
for ((attempt = 0; attempt < 100; attempt++)); do
  kill -0 "$i3_pid" 2> /dev/null || exit 1
  if [[ -S "$socket_path" ]] && ipc -t get_version > "$workdir/version.json"; then
    break
  fi
  sleep 0.05
done
[[ -S "$socket_path" ]]
ipc -t get_version > "$workdir/version.json"

# A private FIFO lets the client itself issue real OSC title updates, without xdotool.
mkfifo "$workdir/title-one"
exec {title_fd}<> "$workdir/title-one"
# The quoted program is evaluated by the client shell, not by this harness.
# shellcheck disable=SC2016
"$XTERM_BIN" -name sparse_smoke_one -title sparse_initial -geometry 50x10+20+20 \
  -e sh -c 'while IFS= read -r title; do printf "\033]0;%s\007" "$title"; done < "$1"' \
  sh "$workdir/title-one" > "$workdir/xterm-one.log" 2>&1 &
client_one=$!
"$XTERM_BIN" -name sparse_smoke_two -geometry 50x10+80+80 \
  -e sleep 120 > "$workdir/xterm-two.log" 2>&1 &
client_two=$!
wait_tree managed

one='[instance="^sparse_smoke_one$"]'
two='[instance="^sparse_smoke_two$"]'
command_ok "$one focus"
# Resize in a split with two clients, where it must change geometry.
command_ok 'layout splith'
wait_tree layout splith
width=$(
  python3 - "$workdir/tree.json" << 'PY'
import json, sys
stack = [json.load(open(sys.argv[1]))]
while stack:
    node = stack.pop()
    if (node.get("window_properties") or {}).get("instance") == "sparse_smoke_one":
        print(node["rect"]["width"])
        break
    stack.extend(node.get("nodes", []) + node.get("floating_nodes", []))
PY
)
command_ok "$one resize grow width 10 px"
wait_tree width "$width"
for layout in stacking tabbed stacking; do
  command_ok "$one layout $layout"
  # The tree calls the stacking layout "stacked".
  expected=$layout
  [[ $layout != stacking ]] || expected=stacked
  wait_tree layout "$expected"
done
for border in 'none 0' 'pixel 1' 'normal 2'; do
  read -r mode size <<< "$border"
  if [[ $mode == none ]]; then
    command_ok "$one border none"
  else
    command_ok "$one border $mode $size"
  fi
  wait_tree border "$mode" "$size"
done
command_ok "$one mark sparse_smoke_mark"
wait_tree mark sparse_smoke_mark present
printf '%s\n' sparse_title_updated >&"$title_fd"
wait_tree title sparse_title_updated
command_ok "$one unmark sparse_smoke_mark"
wait_tree mark sparse_smoke_mark absent
command_ok "$one floating enable"
wait_tree floating user_on
command_ok "$one fullscreen enable"
wait_tree fullscreen 1
command_ok "$one fullscreen disable"
wait_tree fullscreen 0
command_ok "$one floating disable"
wait_tree floating user_off
command_ok "$two kill"
wait_tree destroyed
command_ok "$one kill"
wait_tree empty

phase='surface diagnostics'
if grep -Eqi 'Could not create sparse surface|cairo.*error status' "$workdir/i3.log"; then
  exit 1
fi
kill -0 "$i3_pid"
printf 'xvfb_sparse_smoke=passed\n'
