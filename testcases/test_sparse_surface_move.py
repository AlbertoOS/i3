#!/usr/bin/env python3
"""Floating moves must reuse edge pixmaps, without X-server request checks.

Observe a real private i3 through a test-only LD_PRELOAD library; all XCB calls
still reach a fresh Xvfb. No installed binary or live desktop is touched.
"""

import argparse
import os
import tempfile
from pathlib import Path

from test_sparse_surface_border_pixels import CHILD, Session, wait_for


class ObservedSession(Session):
    def spawn(self, command, name, **kwargs):
        original = self.env
        if name == "i3":
            self.env = dict(original, LD_PRELOAD=self.args.trace_library)
        try:
            return super().spawn(command, name, **kwargs)
        finally:
            self.env = original

    def counts(self):
        lines = (self.root / "i3.log").read_text(errors="replace").splitlines()
        return (lines.count("SPARSE_TEST_PIXMAP"), lines.count("SPARSE_TEST_CHECK"))

    def settled(self):
        # A second IPC request is serviced after the command's tree render.
        self.ipc("-t", "get_tree")
        client = self.client()
        assert client is not None, "private floating client disappeared"
        return client

    def expect_border(self, node):
        w, h = node["rect"]["width"], node["rect"]["height"]
        points = [(0, h // 2), (w - 1, h // 2), (w // 2, h - 1)]
        wait_for(
            lambda: self.probe(node["window"], "sample", points)[3:] == [CHILD] * 3,
            "floating frame border pixels",
        )

    def exercise(self):
        self.start()
        self.command("floating enable")
        self.command("resize set 320 px 220 px")
        initial = self.settled()
        before = self.counts()
        assert before[0] >= 4 and before[1] >= 8, "allocation observer is not active"
        self.expect_border(initial)
        failures = []
        for x, y in ((40, 50), (100, 80), (160, 120)):
            before = self.counts()
            self.command(f"move position {x} px {y} px")
            current = self.settled()
            assert current["rect"]["x"] == x and current["rect"]["y"] == y
            assert current["rect"]["width"] == initial["rect"]["width"]
            assert current["rect"]["height"] == initial["rect"]["height"]
            assert current["window_rect"] == initial["window_rect"]
            after = self.counts()
            delta = tuple(a - b for a, b in zip(after, before))
            self.expect_border(current)
            print(
                f"move ({x},{y}): checked pixmaps={delta[0]}, request checks={delta[1]}",
                flush=True,
            )
            if delta != (0, 0):
                failures.append(
                    f"position-only move ({x},{y}) recreated buffers: {delta}"
                )
        # A real size change must still allocate and validate replacement edges.
        before = self.counts()
        self.command("resize set 360 px 240 px")
        resized = self.settled()
        after = self.counts()
        assert resized["rect"]["width"] != initial["rect"]["width"]
        assert after[0] > before[0] and after[1] > before[1], (
            "resize failed to recreate checked buffers"
        )
        self.expect_border(resized)
        assert not failures, "\n".join(failures)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("i3", "i3-msg", "probe", "xvfb", "xterm", "trace-library"):
        parser.add_argument("--" + name, required=True)
    args = parser.parse_args()
    for name, value in vars(args).items():
        setattr(args, name, str(Path(value).resolve()))
    scratch = os.environ.get("TMPDIR")
    if not scratch:
        parser.error("TMPDIR must name a private scratch directory")
    with tempfile.TemporaryDirectory(
        prefix="i3-move-", dir=str(Path(scratch).resolve())
    ) as directory:
        session = ObservedSession(args, Path(directory))
        try:
            session.exercise()
        finally:
            session.close()
    print("sparse_surface_move=passed")


if __name__ == "__main__":
    main()
