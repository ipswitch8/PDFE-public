# Building and testing PDFE on Linux

This is a developer build, not a supported platform release. The Linux port
(Defect-42; this is not GitHub #42) covers **configure, compile, link and the
cross-platform parity test subset**. It does not cover packaging, a
UI-automation harness, or the Linux implementations of several Windows-specific
subsystems (see [Scope](#scope)). Windows remains the only first-class target.

Open work is tracked on GitHub: the umbrella issue is
[GitHub #390](https://github.com/ipswitch8/PDFE/issues/390). The individual
issues are listed in [Scope](#scope) and in
[linux-parity-status.md](linux-parity-status.md).

Verified on Ubuntu 24.10 with GCC 14.2, CMake 3.30, Ninja 1.12 and Rust/cargo
1.99. The CMake preset is `linux-gcc` (Ninja Multi-Config). The vcpkg triplet is
`x64-linux`, which links the vcpkg dependencies statically, Qt 6.10 included.
The system X11/xcb, GL/EGL and glibc libraries (and dbus) are still linked
dynamically.

## 1. Prerequisites

System packages. Qt's xcb platform plugin needs the X11/xcb development headers,
and its configure step fails with `XCB_XINPUT_FOUND=FALSE` if
`libxcb-xinput-dev` is missing:

```
sudo apt install build-essential cmake ninja-build pkg-config git curl zip unzip tar \
    nasm libtool-bin autoconf autoconf-archive automake python3 \
    '^libxcb.*-dev' libxcb-xinput-dev libx11-xcb-dev libxrender-dev libxi-dev \
    libxkbcommon-dev libxkbcommon-x11-dev libgl1-mesa-dev libglu1-mesa-dev \
    libegl1-mesa-dev libcups2-dev
```

Rust is needed for the `tantivy_bridge` static library in `src/search`. Install
it with [rustup](https://rustup.rs) and make sure `~/.cargo/bin` is on `PATH`.

`libcups2-dev` is what Qt's CUPS print backend builds against. `vcpkg.json`
enables qtbase's `cups` feature on Linux, and without it Qt has no native print
backend there: printing silently becomes "write a PDF". The plugin is static
and is linked automatically (`QT_DEFAULT_PLUGIN`); check for
`libcupsprintersupport.a` in `build/CMakeFiles/impl-Release.ninja`.

If you point an existing build directory at a different vcpkg installed tree
(for example one rebuilt with a new Qt feature), configure a FRESH build
directory. `-DVCPKG_INSTALLED_DIR=...` on a reconfigure leaves every cached
`*_DIR` and library path on the old tree, and the build links the old Qt
without any warning.

Fonts. PDFE finds system fonts through fontconfig. The test suite asks for the
Microsoft core fonts by their Windows file names (`arial.ttf`, `times.ttf`, ...)
and resolves each to its metric-compatible substitute
(`tests/system_font_path.cpp`), so it needs the usual desktop font packages:

```
sudo apt install fonts-liberation fonts-crosextra-carlito fonts-dejavu-core \
    fonts-noto-core fonts-noto-cjk fonts-urw-base35 fonts-font-awesome
```

Test-only Python packages. The suite runs a few repository Python tools as
subprocesses: the image-fixture generator, the fz_try/RAII static scanner and
the UI-runner probe. No PowerShell is needed; the sanitizer verdict logic has a
Python port (`tests/scripts/asan_gate_verdict.py`).

```
sudo apt install python3-pil python3-pytest-timeout python3-clang
```

PyMuPDF must be the version Windows CI pins (1.27.2.3). The distribution's
`python3-fitz` 1.23 segfaults at interpreter exit after walking the fixture
corpus, which fails `ExtractionSidecarConformance` after every check has passed.
Use a virtual environment that can still see the apt packages above, and point
CMake at it:

```
python3 -m venv --system-site-packages ~/.venvs/pdfe
~/.venvs/pdfe/bin/pip install --require-hashes -r tests/fixtures/image_pdf/requirements.txt
# then add -DPython3_EXECUTABLE=$HOME/.venvs/pdfe/bin/python3 to the configure step
```

Generate the image fixtures once. They are git-ignored, as on Windows CI:
`python3 tests/fixtures/image_pdf/gen_fixtures.py`.

## 2. vcpkg

Clone and bootstrap vcpkg. The `linux-gcc` preset reads `VCPKG_ROOT` from the
environment, so export it explicitly in every shell you configure from:

```
git clone https://github.com/microsoft/vcpkg ~/vcpkg
~/vcpkg/bootstrap-vcpkg.sh -disableMetrics
export VCPKG_ROOT=$HOME/vcpkg
```

The dependency build (static Qt, MuPDF from `vcpkg-overlay-ports/`, QPDF,
Tesseract, OpenSSL and the rest) is large: several hours, and memory-hungry. Build it
**once** into a directory outside the source tree, then reuse it for every build
tree. Cap the parallelism, because the Qt build alone can exhaust a 16 GB
machine at full width:

```
cd /path/to/PDFE
VCPKG_MAX_CONCURRENCY=2 $VCPKG_ROOT/vcpkg install --triplet x64-linux \
    --x-install-root=$HOME/.cache/pdfe-linux/vcpkg_installed
```

`vcpkg-configuration.json` points vcpkg at the overlay ports automatically when
you run this from the repository root.

## 3. Configure

Use the prebuilt dependency tree and turn off the manifest install, so the
configure step never starts a vcpkg build:

```
export PATH=$HOME/.cargo/bin:$PATH VCPKG_ROOT=$HOME/vcpkg
cmake --preset linux-gcc -B build \
    -DVCPKG_INSTALLED_DIR=$HOME/.cache/pdfe-linux/vcpkg_installed \
    -DVCPKG_MANIFEST_INSTALL=OFF
```

`-B` is optional. The preset's default is `build/`, and you can pass any other
path to keep separate trees.

## 4. Build

```
cmake --build build --config Release --target pdfe pdfe_preflight pdfe_tests -j3
```

On Linux the executables land in `build/bin/Release/`.

**Memory.** Pick `-j` by free RAM, not by core count. The test suite's
translation units and the final static links are the heavy steps. `-j3` worked
on a 16 GB machine with about 3–5 GB free. Drop to `-j1` if the kernel's OOM
killer starts reaping compiler processes.

## 5. Run

The binaries link Qt statically, and the xcb (X11) and offscreen platform
plugins and the OpenSSL TLS backend are compiled in. Run the binaries in place
on the machine that built them. The exception to static linking is
`libdbus-1.so.3`, which comes from the vcpkg tree as a shared library. It is
found through a RUNPATH pointing into `vcpkg_installed/x64-linux/debug/lib`,
so moving or deleting that directory breaks the binaries. Making them
relocatable belongs with the (not yet done) packaging work.

```
build/bin/Release/pdfe                                  # needs an X11 display (or Xwayland)
QT_QPA_PLATFORM=offscreen build/bin/Release/pdfe --version
```

## 6. Test

Run the cross-platform parity subset, the same selection the CI validation lanes
use, headless:

```
F="$(python3 tools/gate/parity_filter.py)"
QT_QPA_PLATFORM=offscreen PDFE_RUN_SLOW_SAVE_TESTS=1 \
    build/bin/Release/pdfe_tests --gtest_filter="$F" --gtest_output=xml:parity.xml
```

Expected results, the remaining skips and the history are in
[linux-parity-status.md](linux-parity-status.md). The run needs no exclusions:
every selected test is expected to pass or skip. The three Windows file-locking
tests that used to be excluded now have platform-accurate expectations
([GitHub #387](https://github.com/ipswitch8/PDFE/issues/387)).

`QT_QPA_PLATFORM=offscreen` is required for headless runs: without a display,
creating the `QApplication` aborts. With the multi-config generator, `ctest`
also needs a configuration, e.g. `ctest -C Release -N`.

Every `pdfe_tests` run installs a guard against tests that modify the tracked
fixtures under `tests/fixtures`
(`FixtureImmutabilityGuardInstall.IsInstalledForThisRun` fails if it stops
doing so). The test that does so fails by name, with the file it changed
([GitHub #386](https://github.com/ipswitch8/PDFE/issues/386)). If that ever
happens, restore the fixtures with `git checkout -- tests/fixtures` before
trusting another run.

## Scope

What works on Linux: the three targets configure, compile and link; the app
starts and reports its version; and the parity subset passes with no
exclusions.

Network mounts are treated like Windows mapped drives (GitHub #390). A path on
an NFS, SMB/CIFS or network FUSE mount (sshfs, rclone, ...) is "remote" to the
same gate that refuses UNC paths. You can still open and save such a file
yourself; the network-locations preference governs that and allows it by
default, behind the same reachability check Windows uses. What is refused is
what Windows refuses for a mapped drive:
- a document asking to open a file there;
- a PDF `/Launch` action pointing there;
- batch, header/footer, optimize, alignment-injection and redaction-verify
  inputs there;
- a PDF dropped onto the window from there, which is ignored. Use File > Open
  instead.

If your home directory is on NFS, copy files locally for those operations.
Detection uses `statfs` and, for FUSE, the mount's subtype
(`path_is_on_network_filesystem` in `src/core/PathSecurity.cpp`).

Not done, and still Windows-only or stubbed on Linux. Each item is a
checklist entry on [GitHub #390](https://github.com/ipswitch8/PDFE/issues/390):

- **PinnedTempFile "held" property.** POSIX has no share modes, so the
  Windows guarantee (no other writer, deleter or renamer while the file is
  held) cannot hold against a process running as the same user. Linux now has
  a best-effort substitute that detects tampering rather than preventing it:
  `seal()` makes the file 0400, and `verifyUnchanged()` detects a later
  rename-over, delete or in-place rewrite. A change made after
  `verifyUnchanged()` returns is not detected. `InsertImageCommand::redo()`
  re-reads a pasted image without verifying it. See
  [linux-parity-status.md](linux-parity-status.md).
- **Crash handler, partially.** Linux now writes a text crash report (signal,
  fault address, backtrace) for SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT and
  SIGTRAP. Not done:
  - no minidump (no memory contents);
  - no report for a stack overflow on a worker thread;
  - no crash dialog for a fatal signal.
- **Preflight sandbox, partially.** On Linux the helper runs with
  `no_new_privs`, resource limits, Landlock (all filesystem writes denied;
  TCP bind/connect denied on Landlock ABI 4 and later) and a seccomp filter
  that denies sockets, socket I/O, ptrace, `process_vm_*` and io_uring. A
  kernel without Landlock or seccomp runs it
  degraded, and `lastHelperIsolation()` reports that as `Unrestricted`. Not
  done:
  - no analogue of the Windows ACG/CIG code-integrity mitigations;
  - the helper can still signal its parent.
- **The UI-automation harness.** The pywinauto/WinAppDriver/FlaUI stack is
  Windows-only.
- **Distribution packages.** There is no .deb, .rpm or Flatpak. The release
  form is an AppImage plus `packaging/linux/install.sh`, built by
  `tools/packaging/build-appimage.sh`; see [releasing-linux.md](releasing-linux.md).

Related issues:

- [GitHub #385](https://github.com/ipswitch8/PDFE/issues/385): the POSIX
  save-path defects fixed during the port.
- [GitHub #387](https://github.com/ipswitch8/PDFE/issues/387): the three
  Windows-semantics tests, which now pass with platform-accurate
  expectations.
- [GitHub #388](https://github.com/ipswitch8/PDFE/issues/388): the macOS link
  failure.
- [GitHub #389](https://github.com/ipswitch8/PDFE/issues/389): `Defect #nn`
  autolinking.
