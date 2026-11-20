# -*- coding: utf-8 -*-
"""Pins for tools/packaging/publish-linux-release.py: one release per version.

The helper publishes into a release that may already hold the Windows build,
on a public repository, so its promises are pinned rather than trusted:
  * without --apply it makes no mutating gh call;
  * it creates the release only when absent, and never creates a tag;
  * it uploads only the files the release lacks; a file already there with a
    different or unknown sha256 stops the run before any change;
  * it merges the Linux section into the notes, keeps the Windows text byte
    for byte, and a second run changes nothing;
  * it refuses when the tag is not the commit the packages were built from;
  * it refuses when the asset scan is not clean.

GitHub, git and the asset scan are stubbed. The scan's decision logic
(asset_verdict) and the corresponding-source check are the real ones.

Run: python3 -m pytest -q tests/packaging/test_publish_linux_release.py
"""
import contextlib
import hashlib
import importlib.util
import io
import json
import types

import pytest

from conftest import REPO

TOOL = REPO / "tools" / "packaging" / "publish-linux-release.py"
RELHYG = REPO / "tools" / "gate" / "check_release_asset_hygiene.py"
VERSION = "1.2.3"
TAG = "v1.2.3"
REPO_SLUG = "o/r"
COMMIT = "a" * 40

WINDOWS_NOTES = (
    "PDFE **v1.2.3**\n\n## Assets\n- `PDFE-1.2.3-win64.exe`\n\n"
    "## Corresponding source\nPDFE is licensed under AGPL-3.0. The complete "
    "corresponding source for this release is the `v1.2.3` snapshot in this "
    "public repository: https://github.com/o/r\n")


def _load(path, name):
    spec = importlib.util.spec_from_file_location(name, str(path))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


pub = _load(TOOL, "pub")
relhyg = _load(RELHYG, "relhyg_real")


# --- pure logic ---------------------------------------------------------------

def test_seven_assets():
    assert pub.asset_names(VERSION) == [
        "PDFE-1.2.3-x86_64.AppImage", "PDFE-1.2.3-x86_64.AppImage.sha256",
        "pdfe_1.2.3_amd64.deb", "pdfe_1.2.3_amd64.deb.sha256",
        "pdfe-1.2.3-1.x86_64.rpm", "pdfe-1.2.3-1.x86_64.rpm.sha256",
        "install.sh"]


def test_tag_must_be_a_plain_version():
    assert pub.version_of("v1.2.3") == "1.2.3"
    assert pub.version_of("v0.5.3-linux") is None
    assert pub.version_of("1.2.3") is None


def _shas():
    return {n: hashlib.sha256(n.encode()).hexdigest()
            for n in pub.asset_names(VERSION)}


def test_merge_keeps_the_windows_text_and_is_idempotent():
    section = pub.linux_section(REPO_SLUG, TAG, VERSION, None, _shas())
    once = pub.merge_notes(WINDOWS_NOTES, section)
    assert once.startswith(WINDOWS_NOTES.rstrip("\n")), "Windows text changed"
    assert once.count(pub.BEGIN) == 1 and section in once
    assert pub.merge_notes(once, section) == once


def test_merge_replaces_a_stale_linux_section_only():
    old = pub.linux_section(REPO_SLUG, TAG, VERSION, None,
                            {n: "0" * 64 for n in pub.asset_names(VERSION)})
    new = pub.linux_section(REPO_SLUG, TAG, VERSION, None, _shas())
    merged = pub.merge_notes(pub.merge_notes(WINDOWS_NOTES, old) + "\ntrailer\n",
                             new)
    assert old not in merged and new in merged
    assert merged.startswith(WINDOWS_NOTES.rstrip("\n"))
    assert merged.endswith("\ntrailer\n")


def test_an_unmatched_marker_is_refused():
    with pytest.raises(ValueError):
        pub.merge_notes("text\n" + pub.BEGIN + "\nno end", "x")


def test_the_section_is_a_complete_source_offer():
    section = pub.linux_section(REPO_SLUG, TAG, VERSION, None, _shas())
    assert relhyg.source_statement_ok(section, REPO_SLUG)
    for n in ("PDFE-1.2.3-x86_64.AppImage", "pdfe_1.2.3_amd64.deb",
              "pdfe-1.2.3-1.x86_64.rpm", "install.sh"):
        assert "https://github.com/o/r/releases/download/v1.2.3/%s" % n in section
        assert _shas()[n] in section
    assert "sudo apt install ./pdfe_1.2.3_amd64.deb" in section
    assert "sudo dnf install ./pdfe-1.2.3-1.x86_64.rpm" in section


def test_a_cross_tag_section_names_the_tag_it_was_built_from():
    section = pub.linux_section(REPO_SLUG, TAG, VERSION, "v1.2.3-linux", _shas())
    assert "built from tag `v1.2.3-linux`, not from `v1.2.3`" in section
    assert "https://github.com/o/r/tree/v1.2.3-linux" in section
    assert "https://github.com/o/r/releases/tag/v1.2.3-linux" in section
    assert "https://github.com/o/r/tree/v1.2.3\n" not in section
    same = pub.linux_section(REPO_SLUG, TAG, VERSION, None, _shas())
    assert "built from tag" not in same


def test_plan_uploads():
    shas = {"a": "11" * 32, "b": "22" * 32, "c": "33" * 32, "d": "44" * 32}
    existing = [{"name": "b", "digest": "sha256:" + "22" * 32},
                {"name": "c", "digest": "sha256:" + "99" * 32},
                {"name": "d", "digest": None},
                {"name": "PDFE-1.2.3-win64.exe", "digest": "sha256:" + "55" * 32}]
    up, skip, conflict = pub.plan_uploads(existing, shas)
    assert up == ["a"] and skip == ["b"]
    assert [n for n, _d in conflict] == ["c", "d"]


def test_source_check():
    assert pub.source_check(COMMIT, COMMIT, TAG)[0]
    assert not pub.source_check("b" * 40, COMMIT, TAG)[0]
    assert not pub.source_check("", COMMIT, TAG)[0]
    assert not pub.source_check(COMMIT, "", TAG)[0]


def test_sha_file_ok():
    sha = "ab" * 32
    assert pub.sha_file_ok("%s  pkg.deb\n" % sha, "pkg.deb", sha)
    assert pub.sha_file_ok("%s *pkg.deb\n" % sha, "pkg.deb", sha)
    assert not pub.sha_file_ok("%s  other.deb\n" % sha, "pkg.deb", sha)
    assert not pub.sha_file_ok("%s  pkg.deb\n" % ("cd" * 32), "pkg.deb", sha)


# --- main(), end to end with GitHub stubbed -------------------------------------

class FakeGitHub:
    """One release, or none. Records every mutating gh call, with the notes
    file's content captured at call time."""

    MUTATING = {"create", "upload", "edit", "delete"}

    def __init__(self, release=None):
        self.release = release
        self.calls = []

    def run(self, args):
        if args[0] == "git":
            return types.SimpleNamespace(returncode=0, stdout=COMMIT + "\n",
                                         stderr="")
        if args[1] == "api":
            if self.release is None:
                return types.SimpleNamespace(returncode=1, stdout="",
                                             stderr="gh: Not Found (HTTP 404)")
            return types.SimpleNamespace(returncode=0,
                                         stdout=json.dumps(self.release),
                                         stderr="")
        verb = args[2]
        assert args[1] == "release" and verb in self.MUTATING, args
        notes = None
        if "--notes-file" in args:
            with io.open(args[args.index("--notes-file") + 1],
                         encoding="utf-8") as fh:
                notes = fh.read()
        self.calls.append((verb, list(args), notes))
        if verb == "create":
            self.release = {"tag_name": args[3], "body": notes, "assets": []}
        elif verb == "upload":
            for p in args[args.index("--repo") + 2:]:
                with io.open(p, "rb") as fh:
                    d = hashlib.sha256(fh.read()).hexdigest()
                self.release["assets"].append(
                    {"name": p.replace("\\", "/").rsplit("/", 1)[-1],
                     "digest": "sha256:" + d})
        elif verb == "edit":
            self.release["body"] = notes.replace("\n", "\r\n")
        return types.SimpleNamespace(returncode=0, stdout="", stderr="")


def _fake_relhyg(leak=False):
    guard = types.SimpleNamespace(load_all_needles=lambda: ([{"label": "syn"}], None))

    def scan_asset(path, label, needles, m, hits, failures):
        if leak and label.endswith(".deb"):
            hits.append((label, "syn"))
        return 1

    return types.SimpleNamespace(
        _gh=lambda: "gh", _load_guard=lambda: guard,
        run_controls=lambda m, needles: True, scan_asset=scan_asset,
        asset_verdict=relhyg.asset_verdict,
        source_statement_ok=relhyg.source_statement_ok)


@pytest.fixture
def dist(tmp_path):
    d = tmp_path / "dist"
    d.mkdir()
    for n in pub.asset_names(VERSION):
        if n.endswith(".sha256") or n == "install.sh":
            continue
        data = ("payload of %s" % n).encode()
        (d / n).write_bytes(data)
        (d / (n + ".sha256")).write_text(
            "%s  %s\n" % (hashlib.sha256(data).hexdigest(), n), encoding="utf-8")
    inst = tmp_path / "install.sh"
    inst.write_text("#!/bin/sh\n", encoding="utf-8")
    return d, inst


def _main(gh, dist, extra=(), leak=False, rev=None):
    d, inst = dist
    pub._run = gh.run
    pub._rev = rev or (lambda ref: COMMIT)
    pub._load_relhyg = lambda: _fake_relhyg(leak)
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        rc = pub.main(["--tag", TAG, "--repo", REPO_SLUG, "--dist", str(d),
                       "--installer", str(inst)] + list(extra))
    return rc, buf.getvalue()


def _with_all_seven(dist, body):
    d, inst = dist
    assets = []
    for n in pub.asset_names(VERSION):
        p = inst if n == "install.sh" else d / n
        assets.append({"name": n, "digest": "sha256:"
                       + hashlib.sha256(p.read_bytes()).hexdigest()})
    return {"tag_name": TAG, "body": body, "assets": assets}


def test_a_dry_run_makes_no_mutating_call(dist):
    for release in (None, {"tag_name": TAG, "body": WINDOWS_NOTES, "assets": []}):
        gh = FakeGitHub(release)
        rc, out = _main(gh, dist)
        assert rc == 0, out
        assert gh.calls == [], "a dry run called %r" % gh.calls
        assert "DRY RUN: nothing changed" in out


def test_create_path_creates_with_verify_tag_then_uploads_seven(dist):
    gh = FakeGitHub(None)
    rc, out = _main(gh, dist, ["--apply"])
    assert rc == 0, out
    verbs = [c[0] for c in gh.calls]
    assert verbs == ["create", "upload"], verbs
    create = gh.calls[0][1]
    assert "--verify-tag" in create, "creating a release must never create a tag"
    assert relhyg.source_statement_ok(gh.calls[0][2], REPO_SLUG)
    assert sorted(a["name"] for a in gh.release["assets"]) == \
        sorted(pub.asset_names(VERSION))
    assert "verified: 7 Linux files" in out


def test_upload_path_keeps_the_windows_notes(dist):
    win = {"tag_name": TAG, "body": WINDOWS_NOTES,
           "assets": [{"name": "PDFE-1.2.3-win64.exe",
                       "digest": "sha256:" + "55" * 32}]}
    gh = FakeGitHub(win)
    rc, out = _main(gh, dist, ["--apply"])
    assert rc == 0, out
    verbs = [c[0] for c in gh.calls]
    assert verbs == ["upload", "edit"], verbs
    assert len(gh.calls[0][1]) - gh.calls[0][1].index("--repo") - 2 == 7
    notes = gh.calls[1][2]
    assert notes.startswith(WINDOWS_NOTES.rstrip("\n"))
    assert pub.BEGIN in notes
    names = [a["name"] for a in gh.release["assets"]]
    assert "PDFE-1.2.3-win64.exe" in names and len(names) == 8


def test_a_second_run_changes_nothing(dist):
    gh = FakeGitHub({"tag_name": TAG, "body": WINDOWS_NOTES, "assets": []})
    assert _main(gh, dist, ["--apply"])[0] == 0
    first = len(gh.calls)
    rc, out = _main(gh, dist, ["--apply"])
    assert rc == 0, out
    assert len(gh.calls) == first, "re-run made %r" % gh.calls[first:]
    assert "notes: unchanged" in out


def test_a_conflicting_file_stops_the_run_before_any_change(dist):
    rel = _with_all_seven(dist, WINDOWS_NOTES)
    rel["assets"][2]["digest"] = "sha256:" + "ee" * 32
    gh = FakeGitHub(rel)
    rc, out = _main(gh, dist, ["--apply"])
    assert rc == 1 and gh.calls == [], out
    assert "CONFLICT pdfe_1.2.3_amd64.deb" in out


def test_a_file_with_no_digest_is_a_conflict(dist):
    rel = _with_all_seven(dist, WINDOWS_NOTES)
    rel["assets"][0]["digest"] = None
    gh = FakeGitHub(rel)
    rc, _out = _main(gh, dist, ["--apply"])
    assert rc == 1 and gh.calls == []


def test_a_tag_that_is_not_the_build_commit_is_refused(dist):
    gh = FakeGitHub(None)
    rev = lambda ref: ("b" * 40) if ref == TAG else COMMIT
    rc, out = _main(gh, dist, ["--apply"], rev=rev)
    assert rc == 2 and gh.calls == [], out
    assert "One version tag never covers two source trees" in out


def test_source_tag_is_checked_instead_of_the_release_tag(dist):
    rev = lambda ref: {"v1.2.3": "b" * 40, "v1.2.3-linux": COMMIT,
                       "HEAD": COMMIT}.get(ref, "")
    gh = FakeGitHub({"tag_name": TAG, "body": WINDOWS_NOTES, "assets": []})
    rc, out = _main(gh, dist, ["--source-tag", "v1.2.3-linux", "--apply"], rev=rev)
    assert rc == 0, out
    assert "built from tag `v1.2.3-linux`" in gh.calls[-1][2]
    # ...and a --source-tag that is not the build commit is refused too.
    gh2 = FakeGitHub(None)
    rc, _out = _main(gh2, dist, ["--source-tag", "v1.2.3", "--apply"], rev=rev)
    assert rc == 2 and gh2.calls == []


def test_a_leaking_asset_is_never_uploaded(dist):
    gh = FakeGitHub(None)
    rc, out = _main(gh, dist, ["--apply"], leak=True)
    assert rc == 1 and gh.calls == [], out
    assert "asset scan: LEAK" in out


def test_a_sha256_file_that_does_not_match_is_refused(dist):
    d, _inst = dist
    (d / "pdfe_1.2.3_amd64.deb.sha256").write_text(
        "%s  pdfe_1.2.3_amd64.deb\n" % ("00" * 32), encoding="utf-8")
    gh = FakeGitHub(None)
    rc, out = _main(gh, dist, ["--apply"])
    assert rc == 2 and gh.calls == [], out


# --- the post-apply verification is what turns "gh said OK" into "it is there" --

class LossyGitHub(FakeGitHub):
    """Accepts every write, then loses part of it on the re-read: one uploaded
    file, the Linux section, or the corresponding-source statement."""

    def __init__(self, release, lose):
        FakeGitHub.__init__(self, release)
        self.lose = lose

    def run(self, args):
        r = FakeGitHub.run(self, args)
        if args[0] != "git" and args[1] == "api" and self.release is not None \
                and any(c[0] == "upload" for c in self.calls):
            rel = json.loads(r.stdout)
            if self.lose == "file":
                rel["assets"] = [a for a in rel["assets"]
                                 if a["name"] != "pdfe_1.2.3_amd64.deb"]
            elif self.lose == "section":
                rel["body"] = WINDOWS_NOTES
            elif self.lose == "source":
                rel["body"] = (rel["body"] or "").replace(
                    "corresponding source", "source")
                rel["body"] = rel["body"].replace(
                    "Corresponding source", "Source")
            r = types.SimpleNamespace(returncode=0, stdout=json.dumps(rel),
                                      stderr="")
        return r


@pytest.mark.parametrize("lose", ["file", "section", "source"])
def test_the_re_read_catches_what_github_did_not_keep(dist, lose):
    gh = LossyGitHub({"tag_name": TAG, "body": WINDOWS_NOTES, "assets": []},
                     lose)
    rc, out = _main(gh, dist, ["--apply"])
    assert rc == 1, out
    assert "FAIL: verify" in out and "verified:" not in out
    if lose == "file":
        assert "pdfe_1.2.3_amd64.deb" in out.split("FAIL: verify", 1)[1]


# --- the one-off that brings the public v0.5.3 into line ---------------------

ATTACH = REPO / "tools" / "ops" / "attach-v0.5.3-linux-to-public-release.sh"


def test_the_v053_attach_script_mutates_only_through_the_helper():
    # Its only gh call is a download; every change to the public release goes
    # through the helper, which is dry-run unless --apply reaches it, and
    # --apply reaches it only when the operator passed it.
    src = io.open(ATTACH, encoding="utf-8").read()
    code = "\n".join(l for l in src.splitlines() if not l.lstrip().startswith("#"))
    import re
    assert not re.search(r"gh\s+release\s+(create|upload|edit|delete)", code)
    assert "gh api" not in code and "--clobber" not in code
    assert code.count("--apply") == 3, "--apply appears outside the case arm/usage"
    assert "--apply) APPLY=(--apply) ;;" in code and "APPLY=()" in code
    assert '"${APPLY[@]}"' in code


def test_the_v053_attach_script_takes_the_published_files_not_dist():
    src = io.open(ATTACH, encoding="utf-8").read()
    assert "gh release download v0.5.3-linux" in src
    assert "--dist \"$STAGE\"" in src and "/dist" not in src
    assert "--source-tag v0.5.3-linux --built-from v0.5.3-linux" in src
    # The four packages are pinned to the SHA-256 values in the published
    # v0.5.3-linux notes, so a changed download stops the run.
    notes = io.open(REPO / "docs" / "release-notes" / "v0.5.3-linux.md",
                    encoding="utf-8").read()
    import re
    pinned = dict(re.findall(r"\[([^\]]+)\]=([0-9a-f]{64})", src))
    assert len(pinned) == 4
    for name, sha in pinned.items():
        assert "| `%s` | `%s` |" % (name, sha) in notes, name


# The script's own behaviour, run for real with `gh` and the helper replaced:
# a stub `gh` on PATH writes the "downloaded" files and records its arguments,
# and a stub helper beside a copy of the script records what it was given.

_GH_STUB = r'''#!/bin/sh
printf '%s\n' "$*" >> "$GH_LOG"
dir=""
while [ $# -gt 0 ]; do [ "$1" = "--dir" ] && dir="$2"; shift; done
for f in PDFE-0.5.3-x86_64.AppImage pdfe_0.5.3_amd64.deb pdfe-0.5.3-1.x86_64.rpm install.sh; do
    printf 'stub %s %s\n' "$f" "$GH_TAMPER" > "$dir/$f"
done
for f in PDFE-0.5.3-x86_64.AppImage pdfe_0.5.3_amd64.deb pdfe-0.5.3-1.x86_64.rpm; do
    [ -n "$GH_SHORT" ] && [ "$f" = pdfe-0.5.3-1.x86_64.rpm ] && continue
    printf 'sha line\n' > "$dir/$f.sha256"
done
'''

_HELPER_STUB = '''import sys
with open(sys.argv[0] + ".log", "a", encoding="utf-8") as fh:
    fh.write(" ".join(sys.argv[1:]) + "\\n")
'''


def _stub_content(name):
    return ("stub %s \n" % name).encode()


def _attach_sandbox(tmp_path, pin_to_stub):
    root = tmp_path / "repo"
    (root / "tools" / "ops").mkdir(parents=True)
    (root / "tools" / "packaging").mkdir(parents=True)
    src = io.open(ATTACH, encoding="utf-8").read()
    if pin_to_stub:
        # Re-pin to the stub's content so the run gets past the SHA check and
        # the hand-off to the helper can be observed. The real pins are tied
        # to the published notes by the test above.
        import re
        src = re.sub(r"\[([^\]]+)\]=[0-9a-f]{64}",
                     lambda m: "[%s]=%s" % (m.group(1), hashlib.sha256(
                         _stub_content(m.group(1))).hexdigest()), src)
    script = root / "tools" / "ops" / ATTACH.name
    script.write_text(src, encoding="utf-8")
    script.chmod(0o755)
    helper = root / "tools" / "packaging" / "publish-linux-release.py"
    helper.write_text(_HELPER_STUB, encoding="utf-8")
    bindir = tmp_path / "bin"
    bindir.mkdir()
    gh = bindir / "gh"
    gh.write_text(_GH_STUB, encoding="utf-8")
    gh.chmod(0o755)
    return script, helper


def _run_attach(tmp_path, args=(), pin_to_stub=False, tamper="", short=""):
    import os
    import subprocess
    script, helper = _attach_sandbox(tmp_path, pin_to_stub)
    env = dict(os.environ)
    env.update(PATH="%s:%s" % (tmp_path / "bin", env.get("PATH", "")),
               GH_LOG=str(tmp_path / "gh.log"), GH_TAMPER=tamper,
               GH_SHORT=short, TMPDIR=str(tmp_path))
    p = subprocess.run(["bash", str(script)] + list(args), env=env,
                       capture_output=True, text=True)
    log = helper.with_name(helper.name + ".log")
    helper_args = log.read_text(encoding="utf-8").split() if log.exists() else None
    gl = tmp_path / "gh.log"
    gh_log = gl.read_text(encoding="utf-8") if gl.exists() else ""
    return p, helper_args, gh_log


def test_a_download_that_differs_from_the_published_sha_stops_the_run(tmp_path):
    p, helper_args, gh_log = _run_attach(tmp_path)   # real pins, stub content
    assert p.returncode == 2, p.stdout + p.stderr
    assert "ABORT" in p.stderr and "v0.5.3-linux notes say" in p.stderr
    assert helper_args is None, "the helper ran after a SHA mismatch"
    assert "--repo ipswitch8/PDFE-public" in gh_log
    assert gh_log.startswith("release download v0.5.3-linux ")


def test_a_short_download_stops_the_run(tmp_path):
    p, helper_args, _gh = _run_attach(tmp_path, pin_to_stub=True, short="1")
    assert p.returncode == 2 and "expected 7 files" in p.stderr
    assert helper_args is None


def test_a_matching_download_reaches_the_helper_for_the_public_v053(tmp_path):
    p, helper_args, gh_log = _run_attach(tmp_path, pin_to_stub=True)
    assert p.returncode == 0, p.stdout + p.stderr
    a = helper_args
    assert a[a.index("--tag") + 1] == "v0.5.3"
    assert a[a.index("--source-tag") + 1] == "v0.5.3-linux"
    assert a[a.index("--built-from") + 1] == "v0.5.3-linux"
    assert a[a.index("--repo") + 1] == "ipswitch8/PDFE-public"
    stage = a[a.index("--dist") + 1]
    assert stage.startswith(str(tmp_path)), "staged outside the temp dir"
    assert a[a.index("--installer") + 1] == stage + "/install.sh"
    assert "--apply" not in a, "a run without --apply passed --apply on"
    assert "--repo ipswitch8/PDFE-public" in gh_log


def test_apply_reaches_the_helper_only_when_given(tmp_path):
    p, helper_args, _gh = _run_attach(tmp_path, ["--apply"], pin_to_stub=True)
    assert p.returncode == 0, p.stdout + p.stderr
    assert helper_args[-1] == "--apply"


def test_an_unknown_argument_is_refused_before_anything_runs(tmp_path):
    p, helper_args, gh_log = _run_attach(tmp_path, ["--bogus"], pin_to_stub=True)
    assert p.returncode == 2 and "usage:" in p.stderr
    assert helper_args is None and gh_log == ""
