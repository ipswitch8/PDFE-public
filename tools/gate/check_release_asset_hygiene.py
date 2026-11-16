#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""check_release_asset_hygiene.py — release ASSETS are scanned before anyone
downloads them, and every release names its corresponding source.

WHY THIS EXISTS
---------------
Until p100, nothing scanned a shipped artifact. `scan_binary_path_hygiene.py`
reads only changed non-text files in the push diff, and `build/` is gitignored
— so the signed installer reached the release page without any scanner reading
a byte of it. Two recorded incidents are exactly the class this misses: a
linker-embedded `.pdb` path carrying the build account name (GitHub #256), and
gtest XML captured under `docs/`. p100 phase-1 scanned the archived signed
v0.3.0 installer by hand (83 files, 418 MB, clean); this guard makes that scan
a standing, per-release control.

It also carries the AGPL half of the release channel (GitHub #200 item 5): a
recipient of the binary alone must not be left to guess where the source is,
so the release body must name the corresponding source. Coupling the two in
one per-release guard means neither can be forgotten separately.

HOW IT SCANS
------------
For each asset: two decodings — latin-1 (a 1:1 byte map; ASCII strings in a PE,
the measured leak class) and UTF-16LE (Windows resources, NSIS strings) — plus
payload EXTRACTION for `.exe`/`.zip`/`.7z` via 7-Zip, because NSIS compresses
with LZMA and a needle inside the payload is invisible to a raw-byte scan. A
failed or empty extraction is a reported failure (`PAYLOAD-NOT-SCANNED`), never
a silent fallback to the raw scan — a raw-only scan of an installer is the
vacuous-pass shape.

The Linux release formats are compressed too, so they are opened the same way
(v0.5.3, GitHub #390):

  .AppImage   an ELF runtime followed by a squashfs image. The image is cut
              out at the offset the ELF header gives, then unpacked with
              unsquashfs, or with 7-Zip where unsquashfs is missing. The
              AppImage is never run.
  .deb        an ar archive holding control.tar.* and data.tar.*. Read in
              Python (ar + tarfile) so the scan doesn't depend on dpkg.
  .rpm        lead, signature header, header, then a compressed cpio
              payload. Read in Python: the header names the compressor, and
              the cpio (newc) archive is walked in memory.

xz, gzip and bzip2 come from the standard library. zstd uses the `zstd` tool
or 7-Zip. An unknown compressor or a missing tool is PAYLOAD-NOT-SCANNED.
Symlinks are scanned as their target text and never followed, so a link in a
package can't make the scan read a file from the scanning machine.

Sensitivity is proved through BOTH decode paths before anything is scanned,
plus a specificity control; any control failure refuses to scan at all. The
probe is the live build identity: COMPUTERNAME on Windows. On other systems
it's the login name, because a Linux build leaks the account through
/home/<account> paths. If the inventory doesn't recognise that identity, the
scan refuses to run rather than reporting a clean pass it can't back up.

MODES
-----
  --release TAG   download every asset of that release (gh CLI auth; token
                  stays in gh's keyring / CI env, never on disk here), scan
                  them, and verify the release body names the corresponding
                  source (a `github.com/<repo>` URL plus the phrase
                  "corresponding source", case-insensitive).
  --paths P...    scan local artifacts BEFORE upload — the pre-release gate.
                  (No release body exists yet, so the AGPL check is skipped
                  and said so.)
  --self-test     pure decision logic, no network, no 7z.

Exit 0 = scanned and clean (and, for --release, source named); 1 = a hit or a
missing source statement; 2 = could not scan (fails closed).
"""
from __future__ import annotations

import argparse
import bz2
import getpass
import gzip
import hashlib
import importlib.util
import io
import json
import lzma
import os
import re
import shutil
import stat
import struct
import subprocess
import sys
import tarfile
import tempfile

for _n in ("stdout", "stderr"):
    _s = getattr(sys, _n, None)
    if _s is not None and hasattr(_s, "reconfigure"):
        _s.reconfigure(encoding="utf-8", errors="replace")

REPO_DEFAULT = "ipswitch8/PDFE"
_HERE = os.path.dirname(os.path.abspath(__file__))
SEVEN_Z_CANDIDATES = (
    r"C:\Program Files\7-Zip\7z.exe",
    r"C:\Program Files (x86)\7-Zip\7z.exe",
)
EXTRACT_EXT = (".exe", ".zip", ".7z", ".msi")
# Opened by this file's own readers rather than handed to 7-Zip whole.
LINUX_EXT = (".appimage", ".deb", ".rpm")


def _load_guard():
    spec = importlib.util.spec_from_file_location(
        "csr", os.path.join(_HERE, "check_scrub_regressions.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def _gh():
    return (os.environ.get("PDFE_GH")
            or shutil.which("gh.exe") or shutil.which("gh"))


def _seven_z():
    for c in SEVEN_Z_CANDIDATES:
        if os.path.exists(c):
            return c
    return shutil.which("7z")


# ---------------------------------------------------------------------------
# Pure decision logic — unit-tested by test_check_release_asset_hygiene.py.
# ---------------------------------------------------------------------------

_SCREEN_TOKEN = re.compile(r"[A-Za-z0-9]+")
_SCREEN_SEP = re.compile(r"[^A-Za-z0-9]")
_SCREEN_CHUNK = 1 << 22


def _token_sha(s):
    return hashlib.sha256(s.lower().encode("utf-8")).hexdigest()


def could_hold_needle(text, needles):
    """Fast screen: False only when `scan_text` would find no needle in `text`.

    `scan_text` walks the text line by line and hashes every token and every
    adjacent pair in pure Python. That costs about 30 s per 20 MB of a real
    binary, and a Linux package unpacks to some 190 MB. Here each distinct
    token, and each distinct pair of consecutive tokens, is hashed once per
    4 MiB chunk instead.

    The screen is a SUPERSET of scan_text's matching. It uses the same
    tokeniser and the same hash, and it pairs tokens across line breaks and
    chunk boundaries as well as within lines. scan_text only pairs within a
    line, so every token or pair scan_text hashes is also hashed here, and
    False is exact. True only means "run scan_text", which finds the line and
    the label. The equivalence is pinned by tests against scan_text itself.
    """
    wanted = {n["sha256"] for n in needles}
    if not wanted or not text:
        return False
    seen_tok, seen_pair = set(), set()
    prev_last = None
    pos, n = 0, len(text)
    while pos < n:
        end = min(n, pos + _SCREEN_CHUNK)
        if end < n:
            # Never cut a token in two: extend to the next non-token char.
            m = _SCREEN_SEP.search(text, end)
            end = m.start() if m else n
        toks = [t.lower() for t in _SCREEN_TOKEN.findall(text, pos, end)]
        pos = end
        if not toks:
            continue
        if prev_last is not None:
            toks.insert(0, prev_last)
        for t in set(toks) - seen_tok:
            if _token_sha(t) in wanted:
                return True
            seen_tok.add(t)
        for a, b in set(zip(toks, toks[1:])) - seen_pair:
            if _token_sha(a + " " + b) in wanted:
                return True
            seen_pair.add((a, b))
        if len(seen_pair) > 4_000_000:
            seen_pair.clear()          # bound memory; only costs re-hashing
        prev_last = toks[-1]
    return False


def scan_bytes(raw, where, needles, scan_text):
    """Scan one byte blob under both decodings. Returns [(where, label)]."""
    hits = []
    for enc in ("latin-1", "utf-16-le"):
        text = raw.decode(enc, "replace")
        if not could_hold_needle(text, needles):
            continue
        for _ln, lab in scan_text(text, needles):
            hits.append(("%s [%s]" % (where, enc), lab))
    return hits


def download_complete(expected, got):
    """A partial download is a refusal, never a smaller clean scan.

    Extracted from main() because the claim-check gate found the refusal was
    inline glue no test reached: the one branch that decides whether a missing
    asset silently shrinks the scan population deserves a pin of its own.
    """
    return expected > 0 and got == expected


def workflow_shape_ok(text):
    """release-asset-hygiene.yml still fires on publish/edit and runs this
    guard against the release.

    This is the ratchet the claim-check gate demanded: deleting that workflow
    file -- or stripping its `release:` trigger -- used to fail NOTHING, which
    silently reproduces the exact v0.3.0 defect this guard exists to close
    (the signed installer reached its release page unscanned). The guard's own
    --self-test runs in fast-checks.yml on every push, so wiring the check
    here means the deletion now reddens the next push.
    """
    if not text:
        return False
    has_release_trigger = re.search(
        r"^\s*release:\s*$(?:\n\s+types:\s*\[[^\]]*published[^\]]*\])",
        text, re.M)
    runs_this_guard = "check_release_asset_hygiene.py" in text and \
        "--release" in text
    return bool(has_release_trigger and runs_this_guard)


def source_file_installed(cmake_text):
    """CMakeLists.txt still installs CORRESPONDING-SOURCE.txt.

    The release-body guard validates release TEXT; it cannot see the installed
    tree, so removing this install() rule would silently drop the in-tree half
    of the AGPL offer from every future installer. Also demanded by the
    claim-check gate.
    """
    return bool(re.search(
        r"install\s*\(\s*FILES\s+[^)]*CORRESPONDING-SOURCE\.txt[^)]*\)",
        cmake_text or ""))


def source_statement_ok(body, repo):
    """The release body must name the corresponding source: the phrase AND a
    URL to this repository. Either alone is not an offer a binary-only
    recipient can act on."""
    if not body:
        return False
    low = body.lower()
    if "corresponding source" not in low:
        return False
    return bool(re.search(r"github\.com/%s" % re.escape(repo.lower()), low))


def asset_verdict(assets_scanned, hits, extraction_failures):
    if assets_scanned <= 0:
        return "refuse", "0 assets scanned -- refusing to report clean"
    if extraction_failures:
        return "refuse", ("%d archive(s) could not be extracted -- their "
                          "payloads are UNSCANNED, which is not clean"
                          % len(extraction_failures))
    if hits:
        return "leak", "%d hit(s)" % len(hits)
    return "clean", "0 hits over %d asset(s)" % assets_scanned


class PayloadError(Exception):
    """A package payload could not be read in full. Always fails the scan
    closed (PAYLOAD-NOT-SCANNED); it is never downgraded to a raw-only pass."""


def appimage_squashfs_offset(head):
    """Offset of the squashfs image inside a type-2 AppImage, or None.

    The AppImage runtime is an ordinary ELF64 file, and the image starts where
    the ELF ends: e_shoff + e_shnum * e_shentsize. That is how the runtime
    itself finds it, and the caller then checks the squashfs magic there.
    """
    if len(head) < 64 or head[:4] != b"\x7fELF" or head[4] != 2 \
            or head[5] != 1:
        return None
    e_shoff, = struct.unpack_from("<Q", head, 0x28)
    e_shentsize, e_shnum = struct.unpack_from("<HH", head, 0x3A)
    end = e_shoff + e_shentsize * e_shnum
    return end if end > 0 else None


def _zstd_decompress(blob):
    tool = shutil.which("zstd")
    if tool:
        r = subprocess.run([tool, "-d", "-c", "-q"], input=blob,
                           capture_output=True)
        if r.returncode == 0:
            return r.stdout
        raise PayloadError("zstd exit %d" % r.returncode)
    sz = _seven_z()
    if sz:
        td = tempfile.mkdtemp(prefix="pdfe-relhyg-zst-")
        try:
            src = os.path.join(td, "payload.zst")
            io.open(src, "wb").write(blob)
            r = subprocess.run([sz, "e", "-y", "-o" + td, src],
                               capture_output=True, text=True,
                               encoding="utf-8", errors="replace")
            out = [f for f in os.listdir(td) if f != "payload.zst"]
            if r.returncode == 0 and len(out) == 1:
                return io.open(os.path.join(td, out[0]), "rb").read()
            raise PayloadError("7z could not decompress zstd (exit %d)"
                               % r.returncode)
        finally:
            shutil.rmtree(td, ignore_errors=True)
    raise PayloadError("zstd payload, but neither zstd nor 7-Zip is available")


def decompress(blob, kind):
    """Decompress one package payload. `kind` is a tar member suffix or an
    rpm PAYLOADCOMPRESSOR value. Unknown kinds raise; they never pass."""
    try:
        if kind in ("", "none", "tar"):
            return blob
        if kind in ("xz", "lzma"):
            return lzma.decompress(blob)
        if kind in ("gz", "gzip"):
            return gzip.decompress(blob)
        if kind in ("bz2", "bzip2"):
            return bz2.decompress(blob)
    except (lzma.LZMAError, OSError, EOFError, ValueError) as exc:
        raise PayloadError("%s payload is corrupt: %s" % (kind, exc))
    if kind in ("zst", "zstd"):
        return _zstd_decompress(blob)
    raise PayloadError("unsupported payload compression %r" % kind)


def ar_members(data):
    """[(name, bytes)] of a Unix ar archive (the .deb container)."""
    if not data.startswith(b"!<arch>\n"):
        raise PayloadError("not an ar archive")
    out, pos = [], 8
    while pos < len(data):
        hdr = data[pos:pos + 60]
        if len(hdr) < 60 or hdr[58:60] != b"`\n":
            raise PayloadError("truncated or corrupt ar member header")
        name = hdr[:16].decode("ascii", "replace").strip().rstrip("/")
        try:
            size = int(hdr[48:58].decode("ascii").strip())
        except ValueError:
            raise PayloadError("corrupt ar member size")
        body = data[pos + 60:pos + 60 + size]
        if len(body) != size:
            raise PayloadError("truncated ar member %s" % name)
        out.append((name, body))
        pos += 60 + size + (size & 1)
    return out


def tar_members(blob, where):
    """[(name, bytes)] of every member of a tar; a symlink or hard link
    yields its target text, which is what could carry a path."""
    out = []
    try:
        with tarfile.open(fileobj=io.BytesIO(blob), mode="r:") as tf:
            for ti in tf:
                if ti.isfile():
                    out.append((ti.name, tf.extractfile(ti).read()))
                elif ti.issym() or ti.islnk():
                    out.append((ti.name + " -> (link)",
                                ti.linkname.encode("utf-8", "replace")))
    except (tarfile.TarError, EOFError, OSError) as exc:
        raise PayloadError("%s: corrupt tar: %s" % (where, exc))
    return out


def deb_members(data):
    """Every file in a .deb: control.tar.* and data.tar.* opened, plus the
    small members (debian-binary) as they are."""
    out, seen_data = [], False
    for name, body in ar_members(data):
        if name.startswith(("control.tar", "data.tar")):
            kind = name.split(".tar", 1)[1].lstrip(".")
            for mname, mbody in tar_members(decompress(body, kind), name):
                out.append((name + "/" + mname, mbody))
            seen_data |= name.startswith("data.tar")
        else:
            out.append((name, body))
    if not seen_data:
        raise PayloadError("no data.tar member -- not a complete .deb")
    return out


_RPM_TAG_PAYLOADFORMAT = 1124
_RPM_TAG_PAYLOADCOMPRESSOR = 1125


def _rpm_header(data, pos):
    """Parse one rpm header structure at `pos`.

    Returns ({tag: string value}, end_offset). Only string-typed tags are
    decoded, because those are the only ones this reader needs."""
    if data[pos:pos + 4] != b"\x8e\xad\xe8\x01":
        raise PayloadError("bad rpm header magic at %d" % pos)
    nindex, hsize = struct.unpack_from(">II", data, pos + 8)
    store = pos + 16 + 16 * nindex
    end = store + hsize
    if end > len(data):
        raise PayloadError("truncated rpm header")
    tags = {}
    for i in range(nindex):
        tag, typ, off, _cnt = struct.unpack_from(">IIII", data,
                                                 pos + 16 + 16 * i)
        if typ == 6:  # RPM_STRING_TYPE
            raw = data[store + off:end]
            tags[tag] = raw[:raw.find(b"\0")].decode("utf-8", "replace")
    return tags, end


def rpm_members(data):
    """Every file in an .rpm: the compressed cpio payload, walked in memory."""
    if data[:4] != b"\xed\xab\xee\xdb" or len(data) < 96:
        raise PayloadError("not an rpm (bad lead)")
    try:
        _sig, end = _rpm_header(data, 96)
        end += (8 - end % 8) % 8      # the signature header is 8-aligned
        hdr, end = _rpm_header(data, end)
    except (struct.error, IndexError) as exc:
        # A header that runs off the end of the file: a reported refusal,
        # like every other unreadable payload, not a traceback.
        raise PayloadError("truncated or corrupt rpm header: %s" % exc)
    fmt = hdr.get(_RPM_TAG_PAYLOADFORMAT, "cpio")
    if fmt != "cpio":
        raise PayloadError("unsupported rpm payload format %r" % fmt)
    kind = hdr.get(_RPM_TAG_PAYLOADCOMPRESSOR, "gzip")
    return cpio_members(decompress(data[end:], kind))


def cpio_members(data):
    """[(name, bytes)] of a newc / crc cpio archive. A symlink yields its
    target text; it is never followed."""
    out, pos = [], 0
    while True:
        magic = data[pos:pos + 6]
        if magic not in (b"070701", b"070702"):
            raise PayloadError("corrupt cpio header at %d" % pos)
        try:
            f = [int(data[pos + 6 + 8 * i:pos + 14 + 8 * i], 16)
                 for i in range(13)]
        except ValueError:
            raise PayloadError("corrupt cpio header fields at %d" % pos)
        mode, filesize, namesize = f[1], f[6], f[11]
        name_start = pos + 110
        name = data[name_start:name_start + namesize - 1].decode(
            "utf-8", "replace")
        body_start = name_start + namesize
        body_start += (4 - body_start % 4) % 4
        body = data[body_start:body_start + filesize]
        if len(body) != filesize:
            raise PayloadError("truncated cpio member %s" % name)
        if name == "TRAILER!!!":
            return out
        if stat.S_ISLNK(mode):
            out.append((name + " -> (link)", body))
        elif stat.S_ISREG(mode):
            out.append((name, body))
        pos = body_start + filesize
        pos += (4 - pos % 4) % 4


# ---------------------------------------------------------------------------


def live_identity_probe(environ=None, login=None, platform=None):
    """(probe text, what it is) for the sensitivity control, or (None, why).

    Windows: the host name, as before. Elsewhere the login name, embedded the
    way a Linux build leaks it, in a home-directory path. The returned text is
    a probe and is never printed.
    """
    environ = os.environ if environ is None else environ
    platform = sys.platform if platform is None else platform
    if platform.startswith("win"):
        live = (environ.get("COMPUTERNAME") or "").strip()
        if len(live) < 4:
            return None, "COMPUTERNAME absent/short"
        return "x %s y" % live, "COMPUTERNAME"
    if login is None:
        try:
            login = getpass.getuser()
        except (KeyError, OSError):
            login = ""
    login = (login or "").strip()
    if len(login) < 3:
        return None, "login name absent/short"
    return "built in /home/%s/src here" % login, "login name"


def run_controls(m, needles):
    probe, what = live_identity_probe()
    if probe is None:
        print("FAIL: %s -- sensitivity control cannot run; refusing to scan "
              "uncontrolled" % what)
        return False
    print("control probe: the live %s" % what)
    narrow = bool(m.scan_text(probe, needles))
    wide = bool(m.scan_text(probe.encode("utf-16-le").decode("utf-16-le",
                                                             "replace"),
                            needles))
    spec = not m.scan_text("zzsynth qqq", needles)
    print("controls: narrow %s, wide %s, specificity %s"
          % tuple("PASS" if x else "FAIL" for x in (narrow, wide, spec)))
    return narrow and wide and spec


def scan_file_tree(root, prefix, needles, m, hits):
    n = 0
    for dp, _dn, fn in os.walk(root):
        for f in fn:
            p = os.path.join(dp, f)
            rel = prefix + "/" + os.path.relpath(p, root).replace("\\", "/")
            if os.path.islink(p):
                # Scan where it points; never follow it. An absolute link in
                # an unpacked package names a path on the SCANNING machine.
                n += 1
                hits.extend(scan_bytes(os.readlink(p).encode("utf-8",
                                                              "replace"),
                                       rel + " -> (link)", needles,
                                       m.scan_text))
                continue
            try:
                raw = io.open(p, "rb").read()
            except OSError:
                hits.append((prefix + "/" + f, "UNREADABLE"))
                continue
            n += 1
            hits.extend(scan_bytes(
                raw, prefix + "/" + os.path.relpath(p, root).replace("\\", "/"),
                needles, m.scan_text))
    return n


def scan_appimage(raw, label, needles, m, hits):
    """Unpack the squashfs image of an AppImage and scan every file in it.
    Returns the number of files scanned; raises PayloadError if the image
    cannot be found or unpacked."""
    off = appimage_squashfs_offset(raw[:4096])
    if off is None or raw[off:off + 4] != b"hsqs":
        raise PayloadError("no squashfs image after the ELF runtime")
    td = tempfile.mkdtemp(prefix="pdfe-relhyg-appimage-")
    try:
        img = os.path.join(td, "image.squashfs")
        io.open(img, "wb").write(raw[off:])
        root = os.path.join(td, "root")
        tool = shutil.which("unsquashfs")
        if tool:
            r = subprocess.run([tool, "-no-xattrs", "-d", root, img],
                               capture_output=True, text=True,
                               encoding="utf-8", errors="replace")
            how = "unsquashfs"
        else:
            sz = _seven_z()
            if not sz:
                raise PayloadError("neither unsquashfs nor 7-Zip is available")
            r = subprocess.run([sz, "x", "-y", "-tSquashFS", "-o" + root,
                                img], capture_output=True, text=True,
                               encoding="utf-8", errors="replace")
            how = "7z"
        extracted = sum(len(f) for _d, _dn, f in os.walk(root)) \
            if os.path.isdir(root) else 0
        if r.returncode != 0 or not extracted:
            raise PayloadError("%s exit %d, %d files extracted"
                               % (how, r.returncode, extracted))
        print("   extracted %d payload file(s) from %s" % (extracted, label))
        return scan_file_tree(root, label + "/payload", needles, m, hits)
    finally:
        shutil.rmtree(td, ignore_errors=True)


def scan_asset(path, label, needles, m, hits, extraction_failures):
    """Scan one asset; extract archives. Returns files-scanned count."""
    try:
        raw = io.open(path, "rb").read()
    except OSError as exc:
        extraction_failures.append((label, "unreadable: %s" % exc))
        return 0
    count = 1
    hits.extend(scan_bytes(raw, label + " (raw)", needles, m.scan_text))
    ext = os.path.splitext(path)[1].lower()
    if ext in LINUX_EXT:
        try:
            if ext == ".appimage":
                return count + scan_appimage(raw, label, needles, m, hits)
            members = deb_members(raw) if ext == ".deb" else rpm_members(raw)
        except PayloadError as exc:
            extraction_failures.append((label, str(exc)))
            return count
        if not members:
            extraction_failures.append((label, "0 files in the payload"))
            return count
        print("   extracted %d payload file(s) from %s" % (len(members), label))
        for name, body in members:
            hits.extend(scan_bytes(body, label + "/payload/" + name, needles,
                                   m.scan_text))
        return count + len(members)
    if ext in EXTRACT_EXT:
        sz = _seven_z()
        if not sz:
            extraction_failures.append((label, "7-Zip not available"))
            return count
        td = tempfile.mkdtemp(prefix="pdfe-relhyg-")
        try:
            r = subprocess.run([sz, "x", "-y", "-o" + td, path],
                               capture_output=True, text=True,
                               encoding="utf-8", errors="replace")
            extracted = sum(len(f) for _d, _dn, f in os.walk(td))
            if r.returncode in (0, 1) and extracted:
                print("   extracted %d payload file(s) from %s"
                      % (extracted, label))
                count += scan_file_tree(td, label + "/payload", needles, m,
                                        hits)
            else:
                extraction_failures.append(
                    (label, "7z exit %d, %d files extracted"
                     % (r.returncode, extracted)))
        finally:
            shutil.rmtree(td, ignore_errors=True)
    return count


def self_test():
    import hashlib
    syn_hash = hashlib.sha256(b"zzsynth host").hexdigest()
    needles = [{"label": "syn", "sha256": syn_hash,
                "n": len("zzsynth host")}]
    m = _load_guard()
    bad = 0

    def case(name, ok):
        nonlocal bad
        print("  [%s] %s" % ("ok " if ok else "FAIL", name))
        bad += 0 if ok else 1

    case("a needle in ASCII bytes is found",
         scan_bytes(b"see zzsynth-host here", "a", needles, m.scan_text)
         == [("a [latin-1]", "syn")])
    wide = "see zzsynth-host here".encode("utf-16-le")
    case("a needle in UTF-16LE bytes is found through the wide decode",
         ("a [utf-16-le]", "syn") in
         scan_bytes(wide, "a", needles, m.scan_text))
    case("clean bytes produce no hit",
         not scan_bytes(b"nothing of note \x00\x01", "b", needles,
                        m.scan_text))
    case("source statement: phrase + URL passes",
         source_statement_ok(
             "## Corresponding source\nhttps://github.com/o/r at this tag",
             "o/r"))
    case("source statement: URL alone FAILS",
         not source_statement_ok("see https://github.com/o/r", "o/r"))
    case("source statement: phrase alone FAILS",
         not source_statement_ok("the corresponding source is available",
                                 "o/r"))
    case("source statement: empty body FAILS",
         not source_statement_ok("", "o/r"))
    case("verdict refuses 0 assets", asset_verdict(0, [], [])[0] == "refuse")
    case("verdict refuses an unextracted archive",
         asset_verdict(3, [], [("x", "7z exit 2")])[0] == "refuse")
    case("verdict reports a leak",
         asset_verdict(3, [("x", "syn")], [])[0] == "leak")
    case("verdict clean only when scanned and hitless",
         asset_verdict(3, [], [])[0] == "clean")

    case("a partial download is refused", not download_complete(3, 2))
    case("a complete download passes", download_complete(3, 3))
    case("a zero-asset 'download' never passes", not download_complete(0, 0))

    # Linux formats (v0.5.3). Pure logic only: synthetic bytes, no tools.
    case("the fast screen passes a needle on to scan_text",
         could_hold_needle("a\nb zzsynth-host c", needles))
    case("the fast screen clears clean text",
         not could_hold_needle("nothing to see\n" * 50, needles))
    ar = (b"!<arch>\n" + ("%-16s%-12s%-6s%-6s%-8s%-10d`\n"
                           % ("data.tar.lz4", 0, 0, 0, 100644, 2)
                           ).encode("ascii") + b"xx")
    try:
        deb_members(ar)
        refused = False
    except PayloadError:
        refused = True
    case("a .deb payload in an unknown compression is refused", refused)
    cpio = (b"070701" + b"".join(b"%08X" % v for v in (
        0, 0o100644, 0, 0, 1, 0, 3, 0, 0, 0, 0, 6, 0)) + b"./a.x\0"
        + b"abc\0" + b"070701" + b"".join(b"%08X" % v for v in (
            0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 11, 0)) + b"TRAILER!!!\0")
    try:
        got = cpio_members(cpio)
    except PayloadError:
        got = None
    case("a cpio member is read in full", got == [("./a.x", b"abc")])
    case("off Windows the control probe is the login name",
         live_identity_probe({}, login="zzsynth-host", platform="linux")[1]
         == "login name"
         and bool(m.scan_text(live_identity_probe(
             {}, login="zzsynth-host", platform="linux")[0], needles)))
    case("an absent login name refuses the control",
         live_identity_probe({}, login="", platform="linux")[0] is None)

    # Wiring ratchets: these read the REAL sibling files, so a deletion of the
    # release workflow or of the CMake install() rule fails this self-test --
    # which fast-checks.yml runs on every push.
    root = os.path.dirname(_HERE.rstrip(os.sep))
    repo_root = os.path.dirname(root)
    wf = os.path.join(repo_root, ".github", "workflows",
                      "release-asset-hygiene.yml")
    try:
        wf_text = io.open(wf, encoding="utf-8").read()
    except OSError:
        wf_text = ""
    case("release-asset-hygiene.yml exists, fires on publish/edit, and runs "
         "this guard with --release", workflow_shape_ok(wf_text))
    case("a workflow without the release trigger is rejected",
         not workflow_shape_ok("on:\n  workflow_dispatch:\njobs: {}\n"))
    case("a workflow that no longer runs this guard is rejected",
         not workflow_shape_ok(
             "on:\n  release:\n    types: [published, edited]\njobs: {}\n"))
    try:
        cmake_text = io.open(os.path.join(repo_root, "CMakeLists.txt"),
                             encoding="utf-8").read()
    except OSError:
        cmake_text = ""
    case("CMakeLists.txt still installs CORRESPONDING-SOURCE.txt",
         source_file_installed(cmake_text))
    case("a CMakeLists without the install rule is rejected",
         not source_file_installed("install(FILES LICENSE DESTINATION .)"))

    print("self-test: %d failure(s)" % bad)
    return 0 if bad == 0 else 1


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", default=REPO_DEFAULT)
    ap.add_argument("--release", metavar="TAG",
                    help="scan every asset of this release and require a "
                         "corresponding-source statement in its body")
    ap.add_argument("--paths", nargs="+", metavar="FILE",
                    help="scan local artifacts before upload (pre-release)")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args(argv)

    if args.self_test:
        return self_test()
    if not args.release and not args.paths:
        print("nothing to do: pass --release TAG or --paths FILE...")
        return 2

    m = _load_guard()
    needles, err = m.load_all_needles()
    if err:
        print("FAIL: could not load the needle inventory: %s" % err)
        return 2
    print("needles: %d (full inventory)" % len(needles))
    if not run_controls(m, needles):
        return 2

    hits, extraction_failures, scanned = [], [], 0
    rc_agpl = 0

    if args.paths:
        for p in args.paths:
            scanned += scan_asset(p, os.path.basename(p), needles, m, hits,
                                  extraction_failures)
        print("NOTE: --paths mode scans pre-upload artifacts; the "
              "corresponding-source statement is a property of the release "
              "body and is NOT checked here")

    if args.release:
        gh = _gh()
        if not gh:
            print("FAIL: gh not found -- refusing to report clean")
            return 2
        r = subprocess.run(
            [gh, "api", "-X", "GET",
             "repos/%s/releases/tags/%s" % (args.repo, args.release)],
            capture_output=True, text=True, encoding="utf-8",
            errors="replace")
        if r.returncode != 0:
            print("FAIL: could not read release %s" % args.release)
            return 2
        rel = json.loads(r.stdout)
        assets = rel.get("assets") or []
        print("release %s: %d asset(s)" % (args.release, len(assets)))
        if not assets:
            print("FAIL: a release with 0 assets has nothing to certify -- "
                  "refusing to report a clean scan of nothing")
            return 2

        if source_statement_ok(rel.get("body") or "", args.repo):
            print("corresponding-source statement: PRESENT")
        else:
            print("corresponding-source statement: MISSING -- the release "
                  "body must contain the phrase 'corresponding source' and a "
                  "github.com/%s URL (GitHub #200 item 5)" % args.repo)
            rc_agpl = 1

        td = tempfile.mkdtemp(prefix="pdfe-relassets-")
        try:
            dl = subprocess.run(
                [gh, "release", "download", args.release, "--repo", args.repo,
                 "--dir", td, "--pattern", "*"],
                capture_output=True, text=True, encoding="utf-8",
                errors="replace")
            got = os.listdir(td)
            if dl.returncode != 0 or not download_complete(len(assets),
                                                           len(got)):
                print("FAIL: downloaded %d of %d asset(s) -- a partial scan "
                      "is not a clean scan" % (len(got), len(assets)))
                return 2
            for f in sorted(got):
                scanned += scan_asset(os.path.join(td, f), f, needles, m,
                                      hits, extraction_failures)
        finally:
            shutil.rmtree(td, ignore_errors=True)

    print()
    print("files scanned (incl. payloads): %d" % scanned)
    v, why = asset_verdict(scanned, hits, extraction_failures)
    for label, detail in extraction_failures:
        print("   PAYLOAD-NOT-SCANNED: %s (%s)" % (label, detail))
    seen = set()
    for where, lab in hits:
        if (where, lab) in seen:
            continue
        seen.add((where, lab))
        print("   %s  needle=%s" % (where, lab))
    print("asset verdict: %s (%s)" % (v.upper(), why))
    rc_scan = {"clean": 0, "leak": 1, "refuse": 2}[v]
    return max(rc_scan, rc_agpl)


if __name__ == "__main__":
    raise SystemExit(main())
