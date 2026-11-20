# Releasing PDFE for Linux

How to build, verify and publish the Linux release (GitHub #390). Nothing here
is automated in CI. No step publishes anything until the last one, which a
maintainer runs by hand.

A Linux release has seven files:

| File | What it is |
|---|---|
| `PDFE-<version>-x86_64.AppImage` | the application, all non-system libraries bundled |
| `pdfe_<version>_amd64.deb` | the same files, as a Debian/Ubuntu package |
| `pdfe-<version>-1.x86_64.rpm` | the same files, as a Fedora/RPM package |
| a `.sha256` for each of the three | its SHA-256, in `sha256sum` format |
| `install.sh` | `packaging/linux/install.sh`, unchanged from the tagged commit |

## 1. Run the full suite

Before building a release, run the full suite on the test build (`build/`,
`PDFE_TESTING=ON`) and confirm it has 0 failures:

```
python3 tests/scripts/run_full_suite_linux.py build/bin/Release/pdfe_tests out/full-suite 3
```

On a machine with about 16 GB of memory, don't run the suite or the test build
at the same time as the release build (section 2). Each of them uses several
GB per compile job. Lower the shard count, the last argument, if needed.

## 2. Build the AppImage in the release container

**The release binaries are never built on a developer's machine.** Build paths
end up in the binary:

- static libraries carry `__FILE__` from their assertion macros (tesseract,
  libarchive, libjpeg-turbo and others);
- OpenSSL compiles its install prefix in as `ENGINESDIR` and `MODULESDIR`.

A host build of v0.5.3 contained 199 strings naming the build account's home
directory. So the release is built in a clean Ubuntu 24.04 container
(rootless podman), where everything lives under `/work`:

```
tools/packaging/build-release-container.sh --tools-dir <dir-with-the-two-tools> [--jobs 3]
```

The script:

1. exports the **committed** `HEAD` with `git archive`, so uncommitted changes
   are not built and the release is exactly a commit;
2. clones your vcpkg checkout at the same commit into the work directory
   (default `~/.cache/pdfe-release-container`). It gives the container its own
   vcpkg binary cache, because a package restored from the host's cache would
   bring the host's paths back with it;
3. inside the container, installs the build packages listed in
   `tools/packaging/release-container-inner.sh`, configures a fresh
   `PDFE_TESTING=OFF` build (triplet `x64-linux-release`), builds `pdfe` and
   `pdfe_preflight`, and runs `tools/packaging/build-appimage.sh`;
4. keeps the whole transcript in `<work-dir>/release-build.log`;
5. scans the finished AppImage with
   `tools/gate/check_release_asset_hygiene.py --paths` and **fails** on any
   build-host path or account name.

The first run builds every vcpkg dependency, Qt included, and takes hours.
Later runs reuse the container's binary cache and rebuild only PDFE.

`build-appimage.sh` needs two tools that no script downloads:

- `linuxdeploy-x86_64.AppImage` from https://github.com/linuxdeploy/linuxdeploy/releases
- `appimagetool-x86_64.AppImage` from https://github.com/AppImage/appimagetool/releases

Put both in one directory, make them executable, and check them against the
checksums their release pages publish before you run them. Inside the
container, `build-appimage.sh`:

1. reads the version from `pdfe --version`, so the file name cannot disagree
   with the binary;
2. builds an AppDir with `pdfe` and `pdfe_preflight` side by side. The
   preflight sandbox looks for its helper next to `pdfe`;
3. runs linuxdeploy. It copies every shared library outside its excludelist
   into `usr/lib` and rewrites RUNPATH to `$ORIGIN`-relative paths. The
   excludelist leaves glibc, libGL/libOpenGL/EGL, X11 with libSM/libICE,
   fontconfig and the like to the host. Qt is static, plugins included;
4. packs it with appimagetool into `dist/PDFE-<version>-x86_64.AppImage` and
   writes the `.sha256`.

It also measures the oldest glibc the bundle runs on: the newest `GLIBC_x.y`
symbol any bundled ELF file needs. It refuses to build if that disagrees with
`MIN_GLIBC` in `packaging/linux/install.sh`, which the installer uses to
refuse older systems. Ubuntu 24.04 ships glibc 2.39, so the floor is
**glibc 2.39** by construction. That means Ubuntu 24.04, Debian 13, Fedora 40
or later. To support older distributions, change the container's base image,
then update `MIN_GLIBC` and the release notes.

The output is not bit-for-bit reproducible, because two builds of the same
commit differ in timestamps. That is why the published checksum is the one of
the file you publish, not a value anyone can rebuild.

## 3. Build the .deb and .rpm

```
tools/packaging/build-linux-packages.sh --appimage dist/PDFE-<version>-x86_64.AppImage
```

The script unpacks the AppImage's squashfs image and repackages the same files
without rebuilding them. The `.deb` is built with `dpkg-deb`. The `.rpm` is
built with `rpmbuild` in a Fedora 41 container (`localhost/pdfe-rpmbuild:fedora-41`,
built on first use), because rpmbuild is not a Debian tool.

Both packages install the AppDir's `usr/` tree under `/opt/pdfe`, plus:

- the link `/usr/bin/pdfe -> ../../opt/pdfe/bin/pdfe`;
- the menu entry and the icon;
- the licence and the corresponding-source notice, in two places:
  - `/usr/share/doc/pdfe`;
  - `/opt/pdfe/share/pdfe`. This copy is needed because minimal installs
    leave documentation out: Ubuntu's container images path-exclude
    `/usr/share/doc`, and Fedora's run dnf with `tsflags=nodocs`.

The packages have no maintainer scripts. Their files are owned by root, and
none is setuid or world-writable. The deb `Depends` and rpm `Requires` are the
installer's package lists minus FUSE, plus `glibc >= 2.39`, and
`tests/packaging` keeps the lists in step. The rpm is built without stripping,
debuginfo extraction or automatic dependencies. The bundled libraries are
private, and the binaries stay byte-identical to the AppImage's.

## 4. Verify

Run all of these before publishing.

**Install, run and remove each file in clean containers:**

```
tools/packaging/verify-linux-packages.sh --version <version> \
    --deb dist/pdfe_<version>_amd64.deb \
    --rpm dist/pdfe-<version>-1.x86_64.rpm \
    --appimage dist/PDFE-<version>-x86_64.AppImage --installer packaging/linux/install.sh \
    --log-dir <evidence-dir>
```

| check | container | what it does |
|---|---|---|
| deb-ubuntu | ubuntu:24.04 | installs the `.deb` with apt (all Depends resolved from the archive), then runs the shared checks below, then purges it and confirms nothing is left |
| deb-debian | debian:trixie | same as deb-ubuntu |
| rpm-fedora | fedora:41 | the same with dnf |
| appimage-install | ubuntu:24.04 | runs `install.sh --user` twice as an ordinary user with sudo |
| appimage-system | ubuntu:24.04 | runs `install.sh --system` twice |

The shared checks on each installed system:

- `ldd` reports no unresolved library;
- `pdfe --version` prints the version;
- the desktop entry, the icon and the notices are installed.

Every check must PASS. The script exits non-zero otherwise, and it writes
one transcript per check.

**Checksums and the binary itself:**

```
cd dist && sha256sum -c *.sha256
QT_QPA_PLATFORM=offscreen ./PDFE-<version>-x86_64.AppImage --version
./PDFE-<version>-x86_64.AppImage --appimage-extract >/dev/null
readelf -d squashfs-root/usr/bin/pdfe squashfs-root/usr/lib/*.so* | grep -E 'RPATH|RUNPATH'
```

Every RPATH/RUNPATH must be `$ORIGIN`-relative.

**Scan every asset for build-host identifiers** (the extended extractor
unpacks the AppImage's squashfs, the deb's data archive and the rpm's cpio
payload, and reports how many members it read from each):

```
python3 tools/gate/check_release_asset_hygiene.py --paths \
    dist/PDFE-<version>-x86_64.AppImage dist/PDFE-<version>-x86_64.AppImage.sha256 \
    dist/pdfe_<version>_amd64.deb dist/pdfe_<version>_amd64.deb.sha256 \
    dist/pdfe-<version>-1.x86_64.rpm dist/pdfe-<version>-1.x86_64.rpm.sha256 \
    packaging/linux/install.sh
```

It must end `asset verdict: CLEAN` with exit 0. A hit is fixed at its source
and the assets are rebuilt. Hits are never allow-listed.

**The packaging tests:**

```
python3 -m pytest -q tests/packaging tools/gate/test_check_release_asset_hygiene.py
shellcheck packaging/linux/install.sh tools/packaging/*.sh
```

## 5. Publish (manual)

Only after section 4 passes.

**One release per version.** A version has one GitHub release, `v<version>`,
and every platform's files go into it. GitHub marks one release "Latest" and
the repository's front page links to that one only. v0.5.3 shipped Linux on a
separate `v0.5.3-linux` release, so the front page showed the Windows
installer and no Linux packages. Whichever platform publishes first creates
the release; the other uploads into it. There are no more `-linux` tags.

**One version tag never covers two source trees.** So Windows and Linux build
the version from the same commit, the one `v<version>` is tagged on. If they
cannot, do not tag the Linux build `v<version>`; the helper below refuses to
publish packages under a tag that is not the commit they were built from.

The helper uploads these seven files:

- `PDFE-<version>-x86_64.AppImage` and `PDFE-<version>-x86_64.AppImage.sha256`
- `pdfe_<version>_amd64.deb` and `pdfe_<version>_amd64.deb.sha256`
- `pdfe-<version>-1.x86_64.rpm` and `pdfe-<version>-1.x86_64.rpm.sha256`
- `install.sh`

If the tag does not exist yet (Linux publishes first), create it on the build
commit. If Windows published first, the tag is already there; check it is
the commit you built:

```
git tag -a v<version> -m "PDFE <version>"
git push origin v<version>
```

Then run the helper, first without `--apply` to see the plan, then with it:

```
python3 tools/packaging/publish-linux-release.py --tag v<version>
python3 tools/packaging/publish-linux-release.py --tag v<version> --apply
python3 tools/gate/check_release_asset_hygiene.py --release v<version>
```

The helper:

- checks that `v<version>` is the commit the packages were built from
  (`--built-from`, default HEAD);
- checks that each `.sha256` file matches its package, and scans all seven
  files with the release-asset scan;
- creates the release if it is absent, with `--verify-tag`, so it never
  creates a tag;
- uploads only the files the release lacks. A file already there with the
  same SHA-256 is skipped. One with a different SHA-256 stops the run before
  anything changes, because a shipped download is never replaced
  automatically;
- writes a Linux section into the release notes between
  `<!-- pdfe-linux:begin -->` and `<!-- pdfe-linux:end -->` markers. The
  section has the download links, the SHA-256 values, the install commands
  and a corresponding-source statement. Everything outside the markers,
  such as the Windows notes, is kept as it was, and a second run changes
  nothing;
- re-reads the release afterwards and checks that all seven files are there.

Without `--apply` it changes nothing and prints the uploads and the notes diff.

The release body must have a corresponding-source section with the phrase
"corresponding source" and a link to the repository the release is on. The
`--release` check fails without it. The Linux section carries one, and so do
the Windows notes.

`--source-tag` is for attaching packages to a release whose tag points at a
different commit, as v0.5.3 needed: its Windows build came from an earlier
commit, and the Linux build is tagged `v0.5.3-linux`. The helper then checks
`--source-tag` against the build commit, and the Linux section says the files
were built from that tag. Do not use it for a new version: build both
platforms from one commit instead.

For the public repository, publish the snapshot of the tagged tree first,
then run the helper against the public repository:

```
python3 tools/ops/publish_snapshot.py --ref v<version> --public-repo ipswitch8/PDFE-public --version v<version>
python3 tools/packaging/publish-linux-release.py --tag v<version> --repo ipswitch8/PDFE-public --apply
```

The corresponding-source link then names
https://github.com/ipswitch8/PDFE-public.

`install.sh` installs only from a local AppImage the user has already
downloaded and checked. It never downloads anything or pipes anything into a
shell.

## What users get

**The `.deb` and `.rpm`** install as described in section 3. The package
manager pulls in the runtime libraries, and removing the package removes
everything it installed.

**`install.sh`** (see `--help`) does the following:

- **Runtime packages.** It installs the packages the AppImage leaves to the
  host, through the distribution's package manager:
  - FUSE 2;
  - fontconfig, with the Liberation and DejaVu fonts;
  - xkbcommon-x11 and xcb-cursor;
  - GL and EGL, plus libOpenGL;
  - libSM and libICE;
  - libcups.

  It uses `sudo` for that step only, and only when something is missing. On
  apt systems after the time64 transition it installs `libcups2t64` rather
  than `libcups2`.

  The apt path is verified in a clean Ubuntu 24.04 container (section 4). For
  dnf, pacman, zypper and apk, the installer's tests check the package names
  and command lines it would run, but none of these has been run on its
  distribution.
- **The AppImage.** It places it at `/opt/pdfe/pdfe.AppImage` (`--system`) or
  `~/.local/opt/pdfe/pdfe.AppImage` (`--user`, the default when it cannot
  ask).
- **The command.** It links `pdfe` into `/usr/local/bin` or `~/.local/bin`.
- **The menu entry.** It registers `pdfe.desktop` for the PDF MIME type, and
  the icon.
  - The icon is extracted by running the AppImage with `--appimage-extract`.
  - A system install never does that as root. It runs it as the user who
    invoked sudo.
  - A real root login skips the icon.
- **Old installs.** On a system install, it removes an older per-user install
  that would shadow the new one, unless `--keep-old-install` is given.

Running it again is safe.

**On GNOME, dialogs move with the main window.** The Open and Save dialogs,
and every other dialog that blocks the window behind it, are attached to the
PDFE window: dragging a dialog's title bar moves the whole window. This is
GNOME's default for every application (mutter's `attach-modal-dialogs`), not
something PDFE does, and Windows has no such behaviour. Users who want
dialogs to move on their own can turn it off for all applications:

```
gsettings set org.gnome.mutter attach-modal-dialogs false
```

`true` restores GNOME's default. GNOME Tweaks has the same switch, labelled
"Attach Modal Dialogs".
