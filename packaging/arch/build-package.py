#!/usr/bin/env python3
"""Build a side-by-side Arch package from an exact committed i3 revision."""

import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


def git(root, *args):
    return subprocess.check_output(["git", "-C", str(root), *args])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("revision", help="committed source revision, e.g. HEAD")
    parser.add_argument(
        "--output",
        required=True,
        type=Path,
        help="new output directory outside the checkout",
    )
    parser.add_argument(
        "--prepare-only",
        action="store_true",
        help="generate the archive and recipe without invoking makepkg",
    )
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    commit = (
        git(
            root,
            "rev-parse",
            "--verify",
            "--end-of-options",
            args.revision + "^{commit}",
        )
        .decode()
        .strip()
    )
    output = args.output.expanduser().absolute()
    # Resolve aliases before checking containment; never clean an old output.
    output = output.resolve()
    if output == root or root in output.parents:
        parser.error("output must be outside the checkout")
    output.mkdir(parents=True, exist_ok=False)
    template = git(root, "show", commit + ":packaging/arch/PKGBUILD.in").decode()
    meson = git(root, "show", commit + ":meson.build").decode()
    match = re.search(r"version:\s*'([0-9.]+)'", meson)
    if match is None:
        raise RuntimeError("could not determine the source version")
    count = git(root, "rev-list", "--count", commit).decode().strip()
    epoch = git(root, "show", "-s", "--format=%ct", commit).decode().strip()
    version = f"{match[1]}.r{count}.g{commit[:12]}"
    archive = output / "i3-source.tar.gz"
    source = git(root, "archive", "--format=tar", "--prefix=i3-source/", commit)
    # A zero gzip timestamp makes the source checksum independent of build time.
    with archive.open("xb") as raw:
        with gzip.GzipFile(filename="", mode="wb", fileobj=raw, mtime=0) as zipped:
            zipped.write(source)
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    recipe = template.replace("@VERSION@", version).replace("@COMMIT@", commit)
    recipe = recipe.replace("@SOURCE_SHA256@", digest)
    if re.search(r"@[A-Z_]+@", recipe):
        raise RuntimeError("unresolved recipe placeholder")
    (output / "PKGBUILD").write_text(recipe)
    # Keep provenance separate from .BUILDINFO; neither includes secret values.
    receipt = {
        "commit": commit,
        "version": version,
        "source_sha256": digest,
        "source_date_epoch": int(epoch),
    }
    (output / "source-provenance.json").write_text(json.dumps(receipt, indent=2) + "\n")
    if not args.prepare_only:
        env = os.environ.copy()
        env["SOURCE_DATE_EPOCH"] = epoch
        for variable in ["PKGDEST", "SRCDEST", "SRCPKGDEST", "LOGDEST", "BUILDDIR"]:
            env[variable] = str(output)
        scratch = Path(env.get("TMPDIR") or Path.home() / ".cache")
        scratch.mkdir(parents=True, exist_ok=True)
        private_tmp = tempfile.mkdtemp(prefix="i3-pkg-", dir=scratch)
        env["TMPDIR"] = private_tmp
        subprocess.run(
            ["makepkg", "--cleanbuild", "--noconfirm", "--nosign"],
            cwd=output, env=env, check=True
        )
        shutil.rmtree(private_tmp)
        packages = sorted(output.glob("*.pkg.tar.*"))
        if not packages:
            raise RuntimeError("makepkg produced no package")
        receipt["packages"] = [
            {"file": p.name, "sha256": hashlib.sha256(p.read_bytes()).hexdigest()}
            for p in packages
            if p.is_file() and not p.name.endswith(".sig")
        ]
        (output / "source-provenance.json").write_text(
            json.dumps(receipt, indent=2) + "\n"
        )
    print(json.dumps(receipt, indent=2))


if __name__ == "__main__":
    main()
