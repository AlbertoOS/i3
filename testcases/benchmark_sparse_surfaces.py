#!/usr/bin/env python3
"""Compare two i3 builds on fresh, private Xvfb servers using real XRes.

Requires Python 3, cc, pkg-config (xcb-res/xcb), Xvfb and xterm. Example:
  python3 testcases/benchmark_sparse_surfaces.py \
    --baseline-i3 BASE/build/i3 --baseline-msg BASE/build/i3-msg \
    --experiment-i3 EXP/build/i3 --experiment-msg EXP/build/i3-msg \
    --output "$TMPDIR/pixmap-comparison.json"

No ambient display/socket discovery; no installed files or Git state changes.
Latency includes subprocess overhead, not CPU time or a presentation guarantee.
"""

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import selectors
import shlex
import shutil
import signal
import statistics
import subprocess
import tempfile
import time


def clean_env():
    env = os.environ.copy()
    for key in (
        "DISPLAY",
        "I3SOCK",
        "XAUTHORITY",
        "XDG_RUNTIME_DIR",
        "DBUS_SESSION_BUS_ADDRESS",
        "SESSION_MANAGER",
    ):
        env.pop(key, None)
    return env


def checked(argv, env, timeout=5):
    return subprocess.run(
        argv, env=env, check=True, capture_output=True, text=True, timeout=timeout
    ).stdout


def nodes(tree):
    yield tree
    for child in tree.get("nodes", []) + tree.get("floating_nodes", []):
        yield from nodes(child)


def stats(values):
    ordered = sorted(values)
    return {
        "count": len(values),
        "min": min(values),
        "median": statistics.median(values),
        "p95": ordered[math.ceil(len(values) * 0.95) - 1],
        "max": max(values),
    }


class Session:
    def __init__(self, root, i3, msg, helper, args):
        self.root, self.i3, self.msg = root, i3, msg
        self.helper, self.args = helper, args
        self.env = clean_env()
        self.env["XDG_CACHE_HOME"] = str(root / "cache")
        self.env["XDG_CONFIG_HOME"] = str(root / "config-home")
        self.processes = []
        self.logs = []
        self.socket = str(root / "ipc.sock")
        if len(os.fsencode(self.socket)) >= 104:
            raise RuntimeError("TMPDIR path too long for a private UNIX socket")

    def spawn(self, argv, label, **kwargs):
        log = (self.root / (label + ".log")).open("wb")
        self.logs.append(log)
        process = subprocess.Popen(
            argv, env=self.env, stdout=log, stderr=log, start_new_session=True, **kwargs
        )
        self.processes.append(process)
        return process

    def close(self):
        # Every process group is created by this session; never use pkill/name matching.
        for process in reversed(self.processes):
            try:
                os.killpg(process.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
        for process in reversed(self.processes):
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait(timeout=3)
        for log in self.logs:
            log.close()
        return [{"pid": p.pid, "returncode": p.returncode} for p in self.processes]

    def ipc(self, *arguments):
        if self.wm.poll() is not None:
            raise RuntimeError("Private i3 exited")
        return json.loads(
            checked(
                [self.msg, "-s", self.socket, *arguments],
                self.env,
                self.args.command_timeout,
            )
        )

    def command(self, command):
        reply = self.ipc(command)
        if not reply or not all(item.get("success") is True for item in reply):
            raise RuntimeError(f"IPC command failed: {command}: {reply}")

    def memory(self):
        return json.loads(
            checked(
                [str(self.helper), self.env["DISPLAY"], self.socket, str(self.wm.pid)],
                self.env,
                self.args.command_timeout,
            )
        )

    def wait(self, predicate):
        deadline = time.monotonic() + self.args.startup_timeout
        while time.monotonic() < deadline:
            if predicate():
                return
            time.sleep(0.05)
        raise RuntimeError("Timed out waiting for isolated X/IPC state")

    def start(self):
        read_fd, write_fd = os.pipe()
        try:
            self.xserver = self.spawn(
                [
                    self.args.xvfb,
                    "-displayfd",
                    str(write_fd),
                    "-screen",
                    "0",
                    f"{self.args.width}x{self.args.height}x24",
                    "-nolisten",
                    "tcp",
                ],
                "xvfb",
                pass_fds=(write_fd,),
            )
        finally:
            os.close(write_fd)
        try:
            with selectors.DefaultSelector() as selector:
                selector.register(read_fd, selectors.EVENT_READ)
                if not selector.select(self.args.startup_timeout):
                    raise RuntimeError("Xvfb -displayfd timed out")
                display = os.read(read_fd, 32).decode().strip()
            if not display.isdecimal():
                raise RuntimeError("Xvfb did not return a private display number")
        finally:
            os.close(read_fd)
        self.env["DISPLAY"] = ":" + display
        runtime = self.root / "runtime"
        runtime.mkdir(mode=0o700)
        self.env["XDG_RUNTIME_DIR"] = str(runtime)
        config = self.root / "config"
        config.write_text(
            "font pango:monospace 10\nfocus_follows_mouse no\n"
            "workspace_layout default\ndefault_border normal 2\n"
            f"ipc-socket {self.socket}\n"
        )
        # Keep debug/release logging defaults from confounding either build, and
        # avoid creating i3's default POSIX SHM log outside our private scratch.
        self.wm = self.spawn([self.i3, "--shmlog-size", "0", "-c", str(config)], "i3")
        self.wait(lambda: Path(self.socket).is_socket())
        self.version = self.ipc("-t", "get_version")
        # Verify the root socket and PID plus EWMH supporting-window ownership
        # BEFORE issuing any modifying IPC command.
        self.empty_memory = self.memory()
        self.command("workspace benchmark")
        leaves = [
            {"type": "con", "swallows": [{"instance": f"^bench_{i}$"}]}
            for i in range(self.args.clients)
        ]
        layout = {
            "type": "con",
            "layout": "splith",
            "nodes": [
                {
                    "type": "con",
                    "layout": "stacked",
                    "percent": 0.75,
                    "nodes": leaves[:-1],
                },
                dict(leaves[-1], percent=0.25),
            ],
        }
        layout_file = self.root / "layout.json"
        layout_file.write_text(json.dumps(layout))
        self.command(f'append_layout "{layout_file}"')
        for i in range(self.args.clients):
            self.spawn(
                [
                    self.args.xterm,
                    "-name",
                    f"bench_{i}",
                    "-title",
                    f"benchmark {i}",
                    "-geometry",
                    "40x10",
                    "-sl",
                    "0",
                    "-e",
                    "sleep",
                    str(self.args.run_timeout),
                ],
                f"xterm-{i}",
            )

        def managed():
            tree = self.ipc("-t", "get_tree")
            found = self.clients(tree)
            return len(found) == self.args.clients

        self.wait(managed)
        tree = self.ipc("-t", "get_tree")
        clients = self.clients(tree)
        self.ids = {n["window_properties"]["instance"]: n["id"] for n in clients}
        stack = next(
            (
                n
                for n in nodes(tree)
                if n.get("layout") == "stacked"
                and len(n.get("nodes", [])) == self.args.clients - 1
            ),
            None,
        )
        if stack is None or not all(n.get("window") for n in stack["nodes"]):
            raise RuntimeError("Workload did not produce the expected populated stack")
        self.stack_id = stack["id"]
        self.command(f"[con_id={self.ids['bench_0']}] focus")
        time.sleep(self.args.settle_seconds)
        self.loaded_memory = self.memory()
        self.initial_stack_rect = self.stack_rect()
        self.workload_geometry = {
            n["window_properties"]["instance"]: {
                key: n[key] for key in ("rect", "window_rect", "deco_rect", "border")
            }
            for n in self.clients(self.ipc("-t", "get_tree"))
        }

    @staticmethod
    def clients(tree):
        return [
            n
            for n in nodes(tree)
            if n.get("window")
            and (n.get("window_properties") or {})
            .get("instance", "")
            .startswith("bench_")
        ]

    def stack_rect(self):
        return next(
            n["rect"]
            for n in nodes(self.ipc("-t", "get_tree"))
            if n["id"] == self.stack_id
        )

    def measure(self):
        data = {}
        deadline = time.monotonic() + self.args.run_timeout
        for kind in ("resize", "focus_redraw"):
            samples = []
            for i in range(self.args.samples + self.args.warmup):
                if time.monotonic() >= deadline:
                    raise RuntimeError("Per-run timing limit exceeded")
                before = self.stack_rect() if kind == "resize" else None
                target = self.ids[f"bench_{i % (self.args.clients - 1)}"]
                command = (
                    f"[con_id={self.ids['bench_0']}] resize "
                    f"{'grow' if i % 2 == 0 else 'shrink'} width 8 px"
                    if kind == "resize"
                    else f"[con_id={target}] focus"
                )
                start = time.perf_counter_ns()
                self.command(command)
                replied = time.perf_counter_ns()
                tree = self.ipc("-t", "get_tree")
                stack = next(n for n in nodes(tree) if n["id"] == self.stack_id)
                if before is not None and stack["rect"]["width"] == before["width"]:
                    raise RuntimeError("Resize command did not change stack geometry")
                if kind == "focus_redraw" and not any(
                    n["id"] == target and n.get("focused") for n in nodes(tree)
                ):
                    raise RuntimeError("Focus redraw command did not change focus")
                memory = (
                    self.memory()
                )  # X-server reply fence; not compositor presentation.
                finished = time.perf_counter_ns()
                if i >= self.args.warmup:
                    samples.append(
                        {
                            "command_reply_ms": (replied - start) / 1e6,
                            "command_tree_xres_fence_ms": (finished - start) / 1e6,
                            "pixmap_bytes": memory["pixmap_bytes"],
                        }
                    )
            data[kind] = {
                "samples": samples,
                "command_reply_ms": stats([s["command_reply_ms"] for s in samples]),
                "command_tree_xres_fence_ms": stats(
                    [s["command_tree_xres_fence_ms"] for s in samples]
                ),
            }
        return {
            "version": self.version,
            "display": self.env["DISPLAY"],
            "socket": self.socket,
            "empty": self.empty_memory,
            "loaded": self.loaded_memory,
            "after": self.memory(),
            "stack_rect": self.initial_stack_rect,
            "workload_geometry": self.workload_geometry,
            "timings": data,
        }


def executable(value):
    found = shutil.which(value)
    if not found:
        raise argparse.ArgumentTypeError(f"Executable not found: {value}")
    return str(Path(found).resolve())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for variant in ("baseline", "experiment"):
        parser.add_argument(f"--{variant}-i3", required=True, type=executable)
        parser.add_argument(f"--{variant}-msg", required=True, type=executable)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--runs", type=int, default=3)
    parser.add_argument("--samples", type=int, default=30)
    parser.add_argument("--warmup", type=int, default=4)
    parser.add_argument("--clients", type=int, default=12)
    parser.add_argument("--width", type=int, default=1024)
    parser.add_argument("--height", type=int, default=768)
    parser.add_argument("--startup-timeout", type=float, default=15)
    parser.add_argument("--command-timeout", type=float, default=3)
    parser.add_argument("--run-timeout", type=int, default=90)
    parser.add_argument("--settle-seconds", type=float, default=0.5)
    parser.add_argument("--xvfb", type=executable, default=executable("Xvfb"))
    parser.add_argument("--xterm", type=executable, default=executable("xterm"))
    args = parser.parse_args()
    if not (
        1 <= args.runs <= 10
        and 1 <= args.samples <= 500
        and 0 <= args.warmup <= 100
        and 3 <= args.clients <= 24
        and 1024 <= args.width <= 1920
        and 768 <= args.height <= 1080
        and 0 < args.startup_timeout <= 60
        and 0 < args.command_timeout <= 30
        and 1 <= args.run_timeout <= 300
        and 0 <= args.settle_seconds <= 5
    ):
        parser.error("Parameters exceed bounded benchmark limits")
    scratch = os.environ.get("TMPDIR")
    if not scratch or not Path(scratch).is_dir():
        parser.error("TMPDIR must identify an existing private scratch directory")
    os.umask(0o077)
    output = args.output.resolve()
    if output.exists():
        parser.error("Output exists; choose a new output path (no implicit overwrite)")
    result = {
        "schema_version": 1,
        "i3_shmlog_size_bytes": 0,
        "font": "pango:monospace 10",
        "method": "XRes QueryClientPixmapBytes",
        "attribution": "EWMH supporting window -> XRes client resource base; root I3_PID and explicit IPC socket verified",
        "latency_note": "Wall-clock subprocess command reply and command + get_tree + XRes roundtrip. Includes launch/IPC/query overhead; not CPU usage or presentation latency.",
        "limits": {
            k: v
            for k, v in vars(args).items()
            if k not in ("output",) and not k.endswith(("_i3", "_msg"))
        },
        "workload": "N-1 stacked xterms plus one split sibling; alternate width +/-8px and stacked focus redraw",
        "builds": {},
        "runs": [],
    }
    for label in ("baseline", "experiment"):
        binary = getattr(args, label + "_i3")
        result["builds"][label] = {
            "i3": binary,
            "i3_msg": getattr(args, label + "_msg"),
            "i3_sha256": hashlib.sha256(Path(binary).read_bytes()).hexdigest(),
        }
    with tempfile.TemporaryDirectory(prefix="i3-xres-", dir=scratch) as temp:
        root = Path(temp)
        helper = root / "query-i3-pixmaps"
        env = clean_env()
        flags = shlex.split(
            checked(["pkg-config", "--cflags", "--libs", "xcb-res", "xcb"], env)
        )
        checked(
            [
                "cc",
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                str(Path(__file__).with_name("query_i3_pixmap_bytes.c")),
                "-o",
                str(helper),
                *flags,
            ],
            env,
            30,
        )
        for repeat in range(args.runs):
            # Alternate order to reduce systematic warm-cache/order bias.
            order = (
                ("baseline", "experiment")
                if repeat % 2 == 0
                else ("experiment", "baseline")
            )
            for label in order:
                run_root = root / f"{repeat}-{label}"
                run_root.mkdir(mode=0o700)
                session = Session(
                    run_root,
                    getattr(args, label + "_i3"),
                    getattr(args, label + "_msg"),
                    helper,
                    args,
                )
                try:
                    session.start()
                    measured = session.measure()
                    if (
                        result["runs"]
                        and measured["workload_geometry"]
                        != result["runs"][0]["workload_geometry"]
                    ):
                        raise RuntimeError(
                            "Baseline/experiment workload geometries differ"
                        )
                    result["runs"].append(
                        dict(measured, variant=label, repeat=repeat + 1)
                    )
                    print(
                        f"{label} run {repeat + 1}: {measured['loaded']['pixmap_bytes']} i3 pixmap bytes",
                        flush=True,
                    )
                except Exception:
                    # Bounded diagnostics only, never query another display/socket.
                    log = run_root / "i3.log"
                    if log.exists():
                        with log.open("rb") as stream:
                            stream.seek(max(0, log.stat().st_size - 4096))
                            print(stream.read().decode(errors="replace"))
                    raise
                finally:
                    cleanup = session.close()
                result["runs"][-1]["owned_process_cleanup"] = cleanup
    result["summary"] = {}
    for label in ("baseline", "experiment"):
        runs = [r for r in result["runs"] if r["variant"] == label]
        result["summary"][label] = {
            "loaded_pixmap_bytes": stats([r["loaded"]["pixmap_bytes"] for r in runs]),
            "latency_ms": {
                kind: {
                    metric: stats(
                        [s[metric] for r in runs for s in r["timings"][kind]["samples"]]
                    )
                    for metric in ("command_reply_ms", "command_tree_xres_fence_ms")
                }
                for kind in ("resize", "focus_redraw")
            },
        }
    output.parent.mkdir(parents=True, exist_ok=True)
    # Exclusive creation and mode 0600 even if the parent has a permissive mode.
    with open(
        output, "x", opener=lambda path, flags: os.open(path, flags, 0o600)
    ) as stream:
        json.dump(result, stream, indent=2)
        stream.write("\n")
    print(json.dumps(result["summary"], indent=2))
    print(f"Private JSON: {output}")


if __name__ == "__main__":

    def terminate(signum, _frame):
        raise SystemExit(128 + signum)

    signal.signal(signal.SIGTERM, terminate)
    main()
