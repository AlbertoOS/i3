#!/usr/bin/env python3
"""Real frame-pixel regression for inset-only changes (no Python dependencies).

Build the probe: cc -Wall -Wextra -Werror testcases/probe_frame_pixels.c \
    -o "$TMPDIR/probe_frame_pixels" -lxcb
Run: python3 testcases/test_sparse_surface_border_pixels.py \
    --i3 build-verify/i3 --i3-msg build-verify/i3-msg \
    --probe "$TMPDIR/probe_frame_pixels" --xvfb /usr/bin/Xvfb --xterm /usr/bin/xterm
All displays/sockets are private. Never activates the installed window manager.
"""

import argparse
import json
import os
from pathlib import Path
import selectors
import signal
import subprocess
import tempfile
import time


CHILD = "39b86b"
TITLE = "285a96"
INSTANCE = "sparse_border_pixels"


def nodes(tree):
    yield tree
    for child in tree.get("nodes", []) + tree.get("floating_nodes", []):
        yield from nodes(child)


def wait_for(check, description):
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        result = check()
        if result:
            return result
        time.sleep(0.05)
    raise AssertionError("timed out: " + description)


class Session:
    def __init__(self, args, root):
        self.args, self.root = args, root
        self.processes, self.logs = [], []
        self.env = os.environ.copy()
        for key in (
            "DISPLAY",
            "I3SOCK",
            "XAUTHORITY",
            "XDG_RUNTIME_DIR",
            "DBUS_SESSION_BUS_ADDRESS",
            "SESSION_MANAGER",
        ):
            self.env.pop(key, None)
        self.socket = str(root / "ipc.sock")
        if len(os.fsencode(self.socket)) >= 104:
            raise RuntimeError("TMPDIR path too long for private IPC socket")
        runtime = root / "runtime"
        runtime.mkdir(mode=0o700)
        self.env["XDG_RUNTIME_DIR"] = str(runtime)
        self.env["XDG_CONFIG_HOME"] = str(root / "config-home")
        self.env["XDG_CACHE_HOME"] = str(root / "cache")

    def run(self, command):
        return subprocess.run(
            command, env=self.env, check=True, capture_output=True, text=True, timeout=2
        ).stdout

    def spawn(self, command, name, **kwargs):
        log = (self.root / (name + ".log")).open("wb")
        self.logs.append(log)
        process = subprocess.Popen(
            command,
            env=self.env,
            stdout=log,
            stderr=log,
            start_new_session=True,
            **kwargs,
        )
        self.processes.append(process)
        return process

    def close(self):
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

    def ipc(self, *arguments):
        if self.wm.poll() is not None:
            raise RuntimeError("private i3 exited")
        return json.loads(self.run([self.args.i3_msg, "-s", self.socket, *arguments]))

    def command(self, command):
        replies = self.ipc(command)
        assert replies and all(reply.get("success") is True for reply in replies), (
            replies
        )

    def client(self):
        return next(
            (
                n
                for n in nodes(self.ipc("-t", "get_tree"))
                if (n.get("window_properties") or {}).get("instance") == INSTANCE
            ),
            None,
        )

    def probe(self, client, mode, points=()):
        args = [self.args.probe, self.env["DISPLAY"], str(client), mode]
        for x, y in points:
            args.extend((str(x), str(y)))
        return self.run(args).split()

    def start(self):
        read_fd, write_fd = os.pipe()
        try:
            self.spawn(
                [
                    self.args.xvfb,
                    "-displayfd",
                    str(write_fd),
                    "-screen",
                    "0",
                    "800x600x24",
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
                if not selector.select(5):
                    raise RuntimeError("Xvfb -displayfd timed out")
                display = os.read(read_fd, 32).decode().strip()
            assert display.isdecimal(), display
        finally:
            os.close(read_fd)
        self.env["DISPLAY"] = ":" + display
        config = self.root / "config"
        # Five colors in i3's exact order: border, background, text, indicator,
        # child_border. Equal focus palettes remove pointer/focus dependence.
        palette = f"#c24562 #{TITLE} #ffffff #{CHILD} #{CHILD}\n"
        config.write_text(
            "font pango:monospace 10\nfocus_follows_mouse no\n"
            "workspace_layout default\ndefault_border pixel 1\n"
            "hide_edge_borders none\nclient.background #162738\n"
            + "".join(
                "client." + state + " " + palette
                for state in ("focused", "focused_inactive", "unfocused")
            )
            + f"ipc-socket {self.socket}\n"
        )
        self.wm = self.spawn(
            [self.args.i3, "--shmlog-size", "0", "-c", str(config)], "i3"
        )
        wait_for(lambda: Path(self.socket).is_socket(), "private IPC socket")
        self.ipc("-t", "get_version")
        self.spawn(
            [
                self.args.xterm,
                "-name",
                INSTANCE,
                "-title",
                "pixels",
                "-bg",
                "#162738",
                "-e",
                "sleep",
                "120",
            ],
            "xterm",
        )
        return wait_for(self.client, "managed tiled client")

    def exercise(self):
        initial = self.start()
        outer = initial["rect"]
        frame = None
        failures = []
        for style, width in (("pixel", 1), ("pixel", 12), ("normal", 2)):
            label = f"border {style} {width}"
            self.command(f'[id="{initial["window"]}"] {label}')

            def ready():
                node = self.client()
                return (
                    node
                    if node
                    and node["border"] == style
                    and node["current_border_width"] == width
                    else None
                )

            node = wait_for(ready, label)
            assert node["rect"] == outer, "outer tiled rectangle changed"
            assert node["floating"] == "auto_off", "client is not tiled"
            w, h = outer["width"], outer["height"]
            # Sample inside the newly exposed strips, not just their old outer
            # one-pixel edges. Normal title text is short and left-aligned.
            inset = width // 2
            points = [(inset, h // 2), (w - 1 - inset, h // 2), (w // 2, h - 1 - inset)]
            expected = [CHILD] * 3
            if style == "pixel":
                points.append((w // 2, inset))
                expected.append(CHILD)
            else:
                deco = node["deco_rect"]
                assert deco["height"] > 2
                points.append(
                    (
                        deco["x"] + deco["width"] * 3 // 4,
                        deco["y"] + deco["height"] // 2,
                    )
                )
                expected.append(TITLE)
            wr = node["window_rect"]
            assert all(
                not (
                    wr["x"] <= x < wr["x"] + wr["width"]
                    and wr["y"] <= y < wr["y"] + wr["height"]
                )
                for x, y in points
            ), "sample overlaps client"
            for phase in ("render", "Expose restoration"):
                if phase == "Expose restoration":
                    self.probe(node["window"], "clear")
                actual = []

                def matches():
                    nonlocal actual, frame
                    result = self.probe(node["window"], "sample", points)
                    identity = tuple(result[:3])
                    if frame is None:
                        frame = identity
                    assert identity == frame, "frame identity or size changed"
                    assert tuple(map(int, result[1:3])) == (w, h), (
                        "X/IPC frame size mismatch"
                    )
                    actual = result[3:]
                    return actual == expected

                try:
                    wait_for(matches, f"{label} {phase}")
                    print(f"PASS {label} {phase}: {actual}", flush=True)
                except AssertionError as error:
                    failures.append(
                        f"{label} {phase}: samples={points}, expected={expected}, actual={actual}: {error}"
                    )
        assert not failures, "\n".join(failures)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for option in ("i3", "i3-msg", "probe", "xvfb", "xterm"):
        parser.add_argument("--" + option, required=True)
    args = parser.parse_args()
    for name in ("i3", "i3_msg", "probe", "xvfb", "xterm"):
        setattr(args, name, str(Path(getattr(args, name)).resolve()))
    scratch = os.environ.get("TMPDIR")
    if not scratch:
        parser.error("TMPDIR must name a private scratch directory")
    with tempfile.TemporaryDirectory(prefix="i3-border-", dir=scratch) as directory:
        session = Session(args, Path(directory))
        try:
            session.exercise()
        finally:
            session.close()
    print("sparse_surface_border_pixels=passed")


if __name__ == "__main__":
    main()
