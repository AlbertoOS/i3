# Experimental sparse-edge i3 on Arch Linux

This package installs a sparse-edge i3 build alongside the official `i3-wm`
package. It does not replace `/usr/bin/i3`, change your configuration or start a
window manager. Keep the official package installed as the recovery option.

## Build

Install the dependencies listed in `PKGBUILD.in`, including Xvfb and xterm for
checks. From the checkout, build an exact committed revision into a new directory
outside the repository:

```sh
python3 packaging/arch/build-package.py HEAD --output "$HOME/.cache/i3-package-build"
```

The output directory must not already exist. The helper records the full source
commit and source/package SHA-256 checksums in `source-provenance.json`. The
package also contains `SOURCE_COMMIT`; `i3 --version` identifies its revision.
Build inputs are pinned, but byte-identical output across different dependency
or compiler versions is not promised. Makepkg records those versions in
`.BUILDINFO`.

The recipe uses `-O2`, without host-native tuning or LTO. It runs the focused
sparse-surface tests, including real Pango/XCB rendering and isolated Xvfb smoke
coverage. Run the complete integration suite separately with the upstream test
environment. Build and install are deliberately separate: the helper never calls
sudo or installs the package.

## Inspect and install

Inspect the resulting archive with `pacman -Qip PACKAGE` and `pacman -Qlp PACKAGE`.
Files must stay under `/opt/i3-sparse-edge-experimental/`; no official i3 files,
package hooks or system session entries should be present. Run `namcap PACKAGE`
if available. Install the reviewed archive with:

```sh
sudo pacman -U /absolute/path/to/PACKAGE.pkg.tar.zst
```

Check the installed identity without starting a window manager:

```sh
/opt/i3-sparse-edge-experimental/bin/i3 --version
```

## Try it without replacing the current desktop

First exercise the extracted or installed artifact on an isolated X server:

```sh
bash testcases/test_sparse_surface_xvfb_smoke.sh \
  /opt/i3-sparse-edge-experimental/bin/i3 \
  /opt/i3-sparse-edge-experimental/bin/i3-msg \
  "$(command -v xterm)" "$(command -v Xvfb)"
```

For an interactive nested test, use a separate Xephyr display and an explicit
private IPC socket. Never pass the current desktop's display or socket to a test.
Automated smoke tests do not replace checking titlebars, marks and redraws by eye.

For normal use, save work and choose the experimental binary at the next planned
login. Put `/opt/i3-sparse-edge-experimental/bin` first in that session's `PATH` so
its matching companion tools are available. Use your existing session launcher
and input/GPU initialization; change only its final i3 command. For example, an
existing launcher that accepts a session command can receive:

```sh
env PATH="/opt/i3-sparse-edge-experimental/bin:$PATH" \
  nvidia-xrun /opt/i3-sparse-edge-experimental/bin/i3
```

Use that example only if `nvidia-xrun` is already your working login path. The
package intentionally supplies no autostart, session registration or live
replacement command. Do not run this from an already active graphical session.

## Update and recover

The official package continues receiving normal repository updates. This local
package does not automatically follow upstream: update the local fixes branch,
review the delta, rerun tests and build a new exact revision. Keep `next` tracking
`upstream/next`; preserve local fixes on a separate topic branch. Do not pull
blindly over uncommitted fixes or rewrite published history.

To return to the official build, select the original session command at login.
Once the experimental session is no longer running, remove only its package:

```sh
sudo pacman -R i3-sparse-edge-experimental
```

Your official `i3-wm` package and personal configuration remain in place.
