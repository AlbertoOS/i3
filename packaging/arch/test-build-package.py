#!/usr/bin/env python3
"""Exercise package preparation and path guards using a disposable Git fixture."""

import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class PackagePreparationTest(unittest.TestCase):
    def setUp(self):
        scratch = os.environ.get("TMPDIR")
        if not scratch:
            self.skipTest("TMPDIR must name the private fixture root")
        self.temporary = tempfile.TemporaryDirectory(
            prefix="i3-package-test-", dir=scratch
        )
        self.addCleanup(self.temporary.cleanup)
        self.base = Path(self.temporary.name)
        self.root = self.base / "repo"
        self.root.mkdir()
        self.recipe_dir = self.root / "packaging/arch"
        self.recipe_dir.mkdir(parents=True)
        actual = Path(__file__).resolve().parent
        for name in ["build-package.py", "PKGBUILD.in"]:
            shutil.copy2(actual / name, self.recipe_dir / name)
        (self.root / "meson.build").write_text("project('i3', 'c', version: '4.25')\n")
        self.run_git("init", "-q")
        self.run_git("add", ".")
        self.run_git(
            "-c",
            "commit.gpgsign=false",
            "-c",
            "user.name=Fixture",
            "-c",
            "user.email=fixture@example.invalid",
            "commit",
            "-qm",
            "fixture",
        )

    def run_git(self, *args):
        # Fixture commits must not invoke the operator's global deployment hooks.
        return subprocess.check_output(
            ["git", "-c", "core.hooksPath=/dev/null", "-C", str(self.root), *args]
        )

    def run_builder(self, output, revision="HEAD", prepare_only=True, env=None):
        args = [
            "python3",
            str(self.recipe_dir / "build-package.py"),
            revision,
            "--output",
            str(output),
        ]
        if prepare_only:
            args.append("--prepare-only")
        return subprocess.run(args, capture_output=True, text=True, env=env)

    def test_archive_and_provenance_repeat(self):
        for name in ["first", "second"]:
            result = self.run_builder(self.base / name)
            self.assertEqual(result.returncode, 0, result.stderr)
            receipt = json.loads(result.stdout)
            self.assertEqual(
                receipt["commit"], self.run_git("rev-parse", "HEAD").decode().strip()
            )
            payload = (self.base / name / "i3-source.tar.gz").read_bytes()
            self.assertEqual(
                receipt["source_sha256"], hashlib.sha256(payload).hexdigest()
            )
            recipe = (self.base / name / "PKGBUILD").read_text()
            self.assertNotIn("@COMMIT@", recipe)
            self.assertNotIn("@VERSION@", recipe)
            self.assertNotIn("@SOURCE_SHA256@", recipe)
        self.assertEqual(
            (self.base / "first/i3-source.tar.gz").read_bytes(),
            (self.base / "second/i3-source.tar.gz").read_bytes(),
        )

    def test_existing_output_is_not_cleaned(self):
        output = self.base / "existing"
        output.mkdir()
        sentinel = output / "keep"
        sentinel.write_text("untouched")
        self.assertNotEqual(self.run_builder(output).returncode, 0)
        self.assertEqual(sentinel.read_text(), "untouched")

    def test_inside_checkout_rejected_including_symlink(self):
        for output in [self.root / "output", self.base / "alias/output"]:
            alias = self.base / "alias"
            if not alias.exists():
                alias.symlink_to(self.root, target_is_directory=True)
            self.assertNotEqual(self.run_builder(output).returncode, 0)
            self.assertFalse((self.root / "output").exists())

    def test_bad_revision_does_not_create_output(self):
        output = self.base / "invalid"
        self.assertNotEqual(
            self.run_builder(output, revision="--invalid").returncode, 0
        )
        self.assertFalse(output.exists())

    def test_makepkg_failure_is_propagated(self):
        fake = self.base / "bin"
        fake.mkdir()
        command = fake / "makepkg"
        command.write_text("#!/bin/sh\nexit 42\n")
        command.chmod(0o700)
        env = os.environ.copy()
        env["PATH"] = str(fake) + os.pathsep + env["PATH"]
        result = self.run_builder(
            self.base / "failed-build", prepare_only=False, env=env
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("42", result.stderr)
        self.assertEqual(list((self.base / "failed-build").glob("*.pkg.tar.zst")), [])


if __name__ == "__main__":
    unittest.main()
