#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Publish the Linux packages into the ONE GitHub release for a version.

WHY. GitHub marks one release "Latest", and the repository's front page links
to that release only. v0.5.3 shipped Windows on `v0.5.3` and Linux on a
separate `v0.5.3-linux` release, so anyone following the front page saw the
Windows installer and no Linux packages. A version now has one release, and
every platform's files go into it. Whichever platform publishes first creates
the release; the other uploads into it.

WHAT IT DOES, for --tag vX.Y.Z:
  * checks the source: the tag's commit (or --source-tag's, see below) must be
    the commit the packages were built from (--built-from, default HEAD). One
    version tag never covers two source trees, so if the Windows build of this
    version came from a different commit, the two must not share a tag;
  * checks the seven files exist, that each .sha256 file matches its package,
    and scans them with the release-asset hygiene gate (the same scan and
    sensitivity control as check_release_asset_hygiene.py);
  * reads the release. If absent, it will create it (`--verify-tag`: it never
    creates a tag) with the Linux section as its notes. If present, it uploads
    the files the release lacks. A file already there with the same SHA-256 is
    skipped; one with a different or unknown digest stops the run before any
    change, because replacing a shipped download is never automatic;
  * merges the Linux section into the release notes between two HTML comment
    markers. Everything outside the markers (the Windows section) is kept
    byte for byte, and re-running replaces the section with itself.

Nothing changes without --apply. The default run prints the plan and the
notes diff. After --apply it re-reads the release and checks that all seven
files are there and the notes still name the corresponding source.

--source-tag TAG is the one exception to "the release tag is the source tag".
It is for attaching packages to a release whose tag points at a different
tree, as v0.5.3 needed. The Linux section then says, in so many words, which
tag the Linux files were built from, and that tag (not --tag) must match
--built-from.

Usage:
  python3 tools/packaging/publish-linux-release.py --tag v<version> [--repo owner/name]
  python3 tools/packaging/publish-linux-release.py --tag v<version> --apply
"""
from __future__ import annotations

import argparse
import difflib
import hashlib
import importlib.util
import io
import json
import os
import re
import subprocess
import sys
import tempfile

for _n in ("stdout", "stderr"):
    _s = getattr(sys, _n, None)
    if _s is not None and hasattr(_s, "reconfigure"):
        _s.reconfigure(encoding="utf-8", errors="replace")

_HERE = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(os.path.dirname(_HERE))
REPO_DEFAULT = "ipswitch8/PDFE"

BEGIN = "<!-- pdfe-linux:begin -->"
END = "<!-- pdfe-linux:end -->"


# ---------------------------------------------------------------------------
# Pure logic -- unit-tested by tests/packaging/test_publish_linux_release.py.
# ---------------------------------------------------------------------------

def asset_names(version):
    """The seven Linux release files, in upload order."""
    return [
        "PDFE-%s-x86_64.AppImage" % version,
        "PDFE-%s-x86_64.AppImage.sha256" % version,
        "pdfe_%s_amd64.deb" % version,
        "pdfe_%s_amd64.deb.sha256" % version,
        "pdfe-%s-1.x86_64.rpm" % version,
        "pdfe-%s-1.x86_64.rpm.sha256" % version,
        "install.sh",
    ]


def version_of(tag):
    m = re.match(r"^v(\d+\.\d+\.\d+)$", tag or "")
    return m.group(1) if m else None


def linux_section(repo, tag, version, source_tag, shas):
    """The Linux part of the release notes, markers included. `shas` maps file
    name -> sha256 hex for the AppImage, .deb, .rpm and install.sh."""
    dl = "https://github.com/%s/releases/download/%s/" % (repo, tag)
    app, deb, rpm = (asset_names(version)[i] for i in (0, 2, 4))
    lines = [
        BEGIN,
        "## Linux (x86_64)",
        "",
        "| download | sha256 |",
        "|---|---|",
    ]
    for name in (app, deb, rpm, "install.sh"):
        lines.append("| [`%s`](%s%s) | `%s` |" % (name, dl, name, shas[name]))
    lines += [
        "",
        "Each package has a `.sha256` file beside it. Check it before installing.",
        "",
        "**Debian, Ubuntu and derivatives:**",
        "",
        "```",
        "sha256sum -c %s.sha256" % deb,
        "sudo apt install ./%s" % deb,
        "```",
        "",
        "**Fedora and other RPM distributions:**",
        "",
        "```",
        "sha256sum -c %s.sha256" % rpm,
        "sudo dnf install ./%s" % rpm,
        "```",
        "",
        "**Any distribution, AppImage with the installer:**",
        "",
        "```",
        "sha256sum -c %s.sha256" % app,
        "chmod +x install.sh",
        "./install.sh %s --user" % app,
        "```",
        "",
    ]
    if source_tag and source_tag != tag:
        lines += [
            "These Linux files were built from tag `%s`, not from `%s`. The "
            "Windows build of this version came from a different commit, so "
            "the two have separate tags. See also the "
            "[%s release](https://github.com/%s/releases/tag/%s)."
            % (source_tag, tag, source_tag, repo, source_tag),
            "",
        ]
    src = source_tag or tag
    lines += [
        "**Corresponding source (Linux).** PDFE is licensed under AGPL-3.0. "
        "The complete corresponding source for these Linux files is the tagged "
        "tree `%s`: https://github.com/%s/tree/%s" % (src, repo, src),
        END,
    ]
    return "\n".join(lines)


def merge_notes(body, section):
    """Put `section` into `body`: replace an existing marked section, else
    append it. Text outside the markers is kept exactly."""
    body = body or ""
    b, e = body.find(BEGIN), body.find(END)
    if b != -1 and e != -1 and e > b:
        return body[:b] + section + body[e + len(END):]
    if b != -1 or e != -1:
        raise ValueError("release notes have an unmatched pdfe-linux marker")
    if not body.strip():
        return section + "\n"
    return body.rstrip("\n") + "\n\n" + section + "\n"


def plan_uploads(existing_assets, local_shas):
    """Decide per file. existing_assets: the release's asset dicts (name,
    digest). local_shas: name -> sha256 hex. Returns (uploads, skips,
    conflicts); any conflict means the run must stop before changing anything."""
    have = {a.get("name"): (a.get("digest") or "") for a in existing_assets or []}
    uploads, skips, conflicts = [], [], []
    for name, sha in local_shas.items():
        if name not in have:
            uploads.append(name)
        elif have[name].lower() == "sha256:" + sha.lower():
            skips.append(name)
        else:
            conflicts.append((name, have[name] or "no digest"))
    return uploads, skips, conflicts


def source_check(tag_commit, built_commit, tag_used):
    """(ok, message). The tag whose tree the packages claim must be the commit
    they were built from."""
    if not tag_commit:
        return False, "tag %s does not exist in this clone" % tag_used
    if not built_commit:
        return False, "could not resolve the built-from commit"
    if tag_commit != built_commit:
        return False, ("tag %s is %s, but the packages were built from %s. One "
                       "version tag never covers two source trees: tag the "
                       "build commit, or pass --source-tag for the tag that "
                       "points at it." % (tag_used, tag_commit[:12],
                                          built_commit[:12]))
    return True, "tag %s is the build commit %s" % (tag_used, built_commit[:12])


def sha_file_ok(sha_text, name, sha):
    """A `sha256sum` line for `name` with digest `sha`."""
    for line in (sha_text or "").splitlines():
        parts = line.split()
        if len(parts) == 2 and parts[0].lower() == sha.lower() \
                and parts[1].lstrip("*") == name:
            return True
    return False


# ---------------------------------------------------------------------------
# I/O
# ---------------------------------------------------------------------------

def _load_relhyg():
    spec = importlib.util.spec_from_file_location(
        "relhyg", os.path.join(REPO_ROOT, "tools", "gate",
                               "check_release_asset_hygiene.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def _run(args):
    return subprocess.run(args, capture_output=True, text=True,
                          encoding="utf-8", errors="replace")


def _rev(ref):
    r = _run(["git", "-C", REPO_ROOT, "rev-parse", "--verify", "--quiet",
              "%s^{commit}" % ref])
    return r.stdout.strip() if r.returncode == 0 else ""


def _sha256(path):
    h = hashlib.sha256()
    with io.open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def _get_release(gh, repo, tag):
    """(release dict or None, error). None with no error means absent."""
    r = _run([gh, "api", "repos/%s/releases/tags/%s" % (repo, tag)])
    if r.returncode == 0:
        return json.loads(r.stdout), None
    if "404" in (r.stderr or "") or "Not Found" in (r.stderr or ""):
        return None, None
    return None, (r.stderr or r.stdout).strip()[:300]


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--tag", required=True, help="the release tag, vX.Y.Z")
    ap.add_argument("--repo", default=REPO_DEFAULT,
                    help="owner/name (default %s; the public mirror is "
                         "ipswitch8/PDFE-public)" % REPO_DEFAULT)
    ap.add_argument("--dist", default=os.path.join(REPO_ROOT, "dist"),
                    help="directory holding the packages and .sha256 files")
    ap.add_argument("--installer",
                    default=os.path.join(REPO_ROOT, "packaging", "linux",
                                         "install.sh"))
    ap.add_argument("--built-from", default="HEAD",
                    help="the commit the packages were built from (default HEAD)")
    ap.add_argument("--source-tag",
                    help="the tag the packages were built from, when it is not --tag")
    ap.add_argument("--apply", action="store_true",
                    help="make the changes (default: print the plan only)")
    args = ap.parse_args(argv)

    version = version_of(args.tag)
    if not version:
        print("ABORT: --tag must look like vX.Y.Z (got %r)" % args.tag)
        return 2

    tag_used = args.source_tag or args.tag
    ok, msg = source_check(_rev(tag_used), _rev(args.built_from), tag_used)
    print("source: %s" % msg)
    if not ok:
        print("ABORT: %s" % msg)
        return 2

    names = asset_names(version)
    paths = {n: (args.installer if n == "install.sh"
                 else os.path.join(args.dist, n)) for n in names}
    missing = [p for p in paths.values() if not os.path.isfile(p)]
    if missing:
        print("ABORT: missing file(s): %s" % ", ".join(missing))
        return 2
    shas = {n: _sha256(p) for n, p in paths.items()}
    for n in names:
        if n.endswith(".sha256"):
            pkg = n[:-len(".sha256")]
            with io.open(paths[n], encoding="utf-8") as fh:
                if not sha_file_ok(fh.read(), pkg, shas[pkg]):
                    print("ABORT: %s does not match %s" % (n, pkg))
                    return 2
    print("files: 7 present; each .sha256 matches its package")

    relhyg = _load_relhyg()
    gh = relhyg._gh()
    if not gh:
        print("ABORT: gh not found")
        return 2
    m = relhyg._load_guard()
    needles, err = m.load_all_needles()
    if err:
        print("ABORT: could not load the needle inventory: %s" % err)
        return 2
    if not relhyg.run_controls(m, needles):
        print("ABORT: the scan's sensitivity control failed")
        return 2
    hits, failures, scanned = [], [], 0
    for n in names:
        scanned += relhyg.scan_asset(paths[n], n, needles, m, hits, failures)
    verdict, why = relhyg.asset_verdict(scanned, hits, failures)
    print("asset scan: %s (%s)" % (verdict.upper(), why))
    if verdict != "clean":
        for where, lab in sorted(set(hits)):
            print("   %s  needle=%s" % (where, lab))
        return 1

    rel, err = _get_release(gh, args.repo, args.tag)
    if err:
        print("ABORT: could not read release %s on %s: %s"
              % (args.tag, args.repo, err))
        return 2
    section = linux_section(args.repo, args.tag, version, args.source_tag,
                            shas)
    old_body = (rel or {}).get("body") or ""
    try:
        new_body = merge_notes(old_body, section)
    except ValueError as exc:
        print("ABORT: %s" % exc)
        return 2
    if not relhyg.source_statement_ok(new_body, args.repo):
        print("ABORT: the notes would not name the corresponding source")
        return 2
    uploads, skips, conflicts = plan_uploads(
        (rel or {}).get("assets"), shas)

    print("release %s on %s: %s" % (args.tag, args.repo,
                                    "EXISTS" if rel else "ABSENT -- will create"))
    for n in uploads:
        print("  upload  %s" % n)
    for n in skips:
        print("  skip    %s (already there, same sha256)" % n)
    for n, d in conflicts:
        print("  CONFLICT %s (release has %s)" % (n, d))
    diff = list(difflib.unified_diff(old_body.splitlines(),
                                     new_body.splitlines(), "notes (now)",
                                     "notes (after)", lineterm=""))
    print("notes: %s" % ("unchanged" if not diff else "%d diff line(s)"
                                                         % len(diff)))
    for line in diff:
        print("  " + line)
    if conflicts:
        print("ABORT: %d file(s) already on the release with a different or "
              "unknown sha256; nothing changed" % len(conflicts))
        return 1
    if not args.apply:
        print("DRY RUN: nothing changed. Re-run with --apply to publish.")
        return 0

    td = tempfile.mkdtemp(prefix="pdfe-linux-notes-")
    notes = os.path.join(td, "notes.md")
    with io.open(notes, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(new_body)
    try:
        if not rel:
            r = _run([gh, "release", "create", args.tag, "--repo", args.repo,
                      "--verify-tag", "--title", "PDFE %s" % version,
                      "--notes-file", notes])
            if r.returncode != 0:
                print("FAIL: could not create the release: %s"
                      % (r.stderr or r.stdout).strip()[:300])
                return 2
            print("created release %s" % args.tag)
        if uploads:
            r = _run([gh, "release", "upload", args.tag, "--repo", args.repo]
                     + [paths[n] for n in uploads])
            if r.returncode != 0:
                print("FAIL: upload: %s" % (r.stderr or r.stdout).strip()[:300])
                return 2
            print("uploaded %d file(s)" % len(uploads))
        if rel and diff:
            r = _run([gh, "release", "edit", args.tag, "--repo", args.repo,
                      "--notes-file", notes])
            if r.returncode != 0:
                print("FAIL: notes edit: %s" % (r.stderr or r.stdout).strip()[:300])
                return 2
            print("notes updated")
    finally:
        os.remove(notes)
        os.rmdir(td)

    after, err = _get_release(gh, args.repo, args.tag)
    if err or not after:
        print("FAIL: could not re-read the release to verify it")
        return 2
    have = {a.get("name") for a in after.get("assets") or []}
    lacking = [n for n in names if n not in have]
    got = (after.get("body") or "").replace("\r\n", "\n")
    # The section carries its own corresponding-source statement for this repo
    # (pinned by test_the_section_is_a_complete_source_offer), so finding it
    # intact also proves the notes still name the source.
    body_ok = section in got
    if lacking or not body_ok:
        print("FAIL: verify: missing %s; notes %s"
              % (lacking or "none", "ok" if body_ok else "WRONG"))
        return 1
    print("verified: 7 Linux files on %s, notes carry the Linux section and "
          "a corresponding-source statement" % args.tag)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
