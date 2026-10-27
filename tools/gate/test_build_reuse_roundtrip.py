# -*- coding: utf-8 -*-
"""Exercise the REAL publish/restore scripts from windows-validation.yml.

check_build_reuse_guards.py checks the WIRING statically (which steps run when).
This checks the BEHAVIOUR: it extracts the two steps' `run:` blocks straight out
of the workflow and executes them against a synthetic build tree, so the thing
under test is the shipped script rather than a re-implementation of it.

A synthetic tree rather than the real 2.3 GB one, deliberately: the logic worth
testing is copy / marker-ordering / verification / refusal, none of which cares
about file size, and the real tree is in use by CI runs.

Cases:
  round-trip   publish then restore -> the three consumed binaries land
  marker-last  an interrupted publish (no marker) is REFUSED, not accepted
  wrong-sha    a store from another commit is REFUSED
  incomplete   a store missing a consumed binary is REFUSED
  no-store     a commit with nothing published is REFUSED, not silently ok
"""
from __future__ import annotations

import io
import os
import re
import sys
import shutil
import tempfile
import subprocess

import yaml  # type: ignore

WORKFLOW = os.path.join(".github", "workflows", "windows-validation.yml")
CONSUMED = (
    os.path.join("bin", "Debug", "pdfe_tests.exe"),
    os.path.join("bin", "Debug", "pdfe.exe"),
    os.path.join("bin", "Release", "pdfe.exe"),
)


def step_script(name_fragment: str) -> str:
    with io.open(WORKFLOW, encoding="utf-8") as fh:
        doc = yaml.safe_load(fh.read())
    # Search EVERY job, not a hardcoded one. The lane is a chained DAG now, so
    # publish and restore live in different jobs -- and a hardcoded job name
    # turned this whole behavioural test into a KeyError the moment that split
    # happened, which would have read as "the scripts broke".
    for job in doc["jobs"].values():
        for step in job.get("steps", []):
            if name_fragment in step.get("name", ""):
                return step["run"]
    raise SystemExit(f"no step matching {name_fragment!r}")


def run_ps(script: str, env: dict) -> tuple[int, str]:
    full = dict(os.environ)
    full.update(env)
    with tempfile.NamedTemporaryFile("w", suffix=".ps1", delete=False,
                                     encoding="utf-8") as fh:
        fh.write(script)
        path = fh.name
    try:
        # `pwsh`, NOT `powershell`: the workflow declares `shell: pwsh`, and the
        # two differ in a way that matters here. Windows PowerShell 5.1 reads a
        # BOM-less UTF-8 .ps1 as ANSI, which mangles any non-ASCII character and
        # can break string quoting outright — this harness reported a parse
        # error in a script that is perfectly valid under the shell that
        # actually runs it. A test must use the real shell or it tests a
        # different program.
        p = subprocess.run(
            ["pwsh", "-NoProfile", "-ExecutionPolicy", "Bypass",
             "-File", path],
            capture_output=True, text=True, encoding="utf-8",
            errors="replace", env=full)
        return p.returncode, (p.stdout or "") + (p.stderr or "")
    finally:
        os.unlink(path)


def make_tree(root: str) -> None:
    """A synthetic build tree carrying everything the publish step copies."""
    for rel in CONSUMED:
        path = os.path.join(root, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with io.open(path, "w", encoding="utf-8") as fh:
            fh.write("binary-stand-in")
    for rel in (os.path.join("vcpkg_installed", "x64-windows", "bin", "Qt6Core.dll"),
                os.path.join("vcpkg_installed", "x64-windows", "debug", "bin", "Qt6Cored.dll"),
                os.path.join("tests", "security", "CTestTestfile.cmake")):
        path = os.path.join(root, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with io.open(path, "w", encoding="utf-8") as fh:
            fh.write("x")


# A named, r-string helper rather than an inline nest of .replace() calls. The
# inline version was unreadable enough that it could have been vacuously true
# without anyone noticing -- so it is proved discriminating below rather than
# assumed. Non-raw Windows-path literals have caused two silent bugs in this
# session already.
_SHARED_STORE_PREFIX = r"c:\pdfe-build-store"


def _is_shared_store(path) -> bool:
    if not path:
        return False
    return path.replace("/", "\\").lower().startswith(_SHARED_STORE_PREFIX)


def _self_check_shared_store() -> None:
    """Guards the guard: the check above must be able to say NO."""
    assert _is_shared_store(r"C:\pdfe-build-store\abc"), "rejects the real path"
    assert not _is_shared_store(r"C:\somewhere-else\abc"), "accepts a wrong path"
    assert not _is_shared_store(None), "accepts a missing path"
    assert not _is_shared_store(""), "accepts an empty path"


def artifact_path_of(name_fragment: str) -> tuple:
    """(artifact name, extract path) for a step that uses upload/download-artifact."""
    with io.open(WORKFLOW, encoding="utf-8") as fh:
        doc = yaml.safe_load(fh.read())
    for job in doc["jobs"].values():
        for step in job.get("steps", []):
            if name_fragment in step.get("name", ""):
                w = step.get("with") or {}
                return w.get("name"), w.get("path")
    raise SystemExit("no artifact step matching %r" % name_fragment)


def check_cross_machine(record) -> None:
    """The artifact path must reuse the SHARED store dir, not a second one.

    This is the whole reason the refusal cases did not have to be re-proved for
    cross-machine reuse: the download populates exactly the directory the local
    publish writes, and the SAME verification step then runs. If the upload and
    download ever disagree about that directory -- or stop being SHA-keyed --
    the shared-verifier property silently stops holding and every refusal above
    would be testing a path nothing uses.
    """
    up_name, up_path = artifact_path_of("Upload the published build as an ARTIFACT")
    dl_name, dl_path = artifact_path_of("Fetch the published build from ARTIFACTS")

    record("upload and download agree on the artifact name",
           up_name == dl_name and up_name is not None, "%r vs %r" % (up_name, dl_name))
    record("upload and download agree on the store path",
           up_path == dl_path and up_path is not None, "%r vs %r" % (up_path, dl_path))
    record("the artifact is keyed on the commit SHA",
           up_name is not None and "github.sha" in up_name, str(up_name))
    record("the download extracts into the SHARED local store dir",
           _is_shared_store(dl_path), str(dl_path))


def check_artifact_provenance(record, sb, res_script, store_root) -> None:
    """The four refusals must hold for a store that arrived by DOWNLOAD.

    WHY THIS EXISTS SEPARATELY from the round-trip above. That one publishes
    with the real publish step and then restores -- so it only ever exercises a
    store this workflow wrote itself. A cold machine's store arrives by
    `download-artifact` extracting an archive: same directory, same contents,
    but nothing local produced it.

    The claim being tested is that the verifier does not care about provenance,
    which is the entire reason the artifact path did not need its own copy of
    the refusal logic. Asserting it beats reasoning about it -- "the paths are
    the same so the behaviour is the same" is exactly the kind of transitivity
    argument that has been wrong before in this repo.
    """
    import os as _os
    import shutil as _shutil

    SHA = "c" * 40
    ws = _os.path.join(sb, "ws-artifact")
    build = _os.path.join(ws, "build", "vs2022")
    store = _os.path.join(store_root, SHA)
    env = {"GITHUB_WORKSPACE": ws, "GITHUB_SHA": SHA}

    def _lay_down_store():
        """Exactly what extracting the artifact would leave behind."""
        _shutil.rmtree(store, ignore_errors=True)
        for rel in CONSUMED:
            p = _os.path.join(store, rel)
            _os.makedirs(_os.path.dirname(p), exist_ok=True)
            with io.open(p, "w", encoding="utf-8") as fh:
                fh.write("extracted-from-artifact")
        with io.open(_os.path.join(store, "published-sha.txt"), "w",
                     encoding="ascii") as fh:
            fh.write(SHA)

    # 1. success: a downloaded store restores
    _lay_down_store()
    _shutil.rmtree(build, ignore_errors=True)
    rc, out = run_ps(res_script, env)
    record("artifact-provenance store RESTORES", rc == 0, out.strip()[-250:])
    record("artifact-provenance restore produced every consumed binary",
           all(_os.path.isfile(_os.path.join(build, r)) for r in CONSUMED), build)

    # 2. no marker
    _lay_down_store()
    _os.unlink(_os.path.join(store, "published-sha.txt"))
    rc, out = run_ps(res_script, env)
    record("artifact-provenance: no marker is REFUSED",
           rc != 0 and "marker" in out.lower(), out.strip()[-200:])

    # 3. wrong commit
    _lay_down_store()
    with io.open(_os.path.join(store, "published-sha.txt"), "w",
                 encoding="ascii") as fh:
        fh.write("d" * 40)
    rc, out = run_ps(res_script, env)
    record("artifact-provenance: commit mismatch is REFUSED",
           rc != 0 and "refusing" in out.lower(), out.strip()[-200:])

    # 4. incomplete
    _lay_down_store()
    _os.unlink(_os.path.join(store, CONSUMED[-1]))
    _shutil.rmtree(build, ignore_errors=True)
    rc, out = run_ps(res_script, env)
    record("artifact-provenance: incomplete store is REFUSED",
           rc != 0 and "incomplete" in out.lower(), out.strip()[-200:])

    # 5. nothing there at all
    _shutil.rmtree(store, ignore_errors=True)
    rc, out = run_ps(res_script, env)
    record("artifact-provenance: absent store is REFUSED",
           rc != 0 and "no published build" in out.lower(), out.strip()[-200:])


def main() -> int:
    # The scripts hardcode C:\pdfe-build-store; point that at a temp root by
    # rewriting only that literal, so everything else stays byte-identical.
    sandbox = tempfile.mkdtemp(prefix="pdfe-reuse-test-")
    store_root = os.path.join(sandbox, "store")
    pub = step_script("Publish the build for reuse").replace(
        r"C:\pdfe-build-store", store_root)
    res = step_script("Restore the published build").replace(
        r"C:\pdfe-build-store", store_root)

    results: list[tuple[str, bool, str]] = []

    def record(label: str, ok: bool, detail: str) -> None:
        results.append((label, ok, detail))

    SHA = "a" * 40
    OTHER = "b" * 40
    try:
        ws = os.path.join(sandbox, "ws")
        build = os.path.join(ws, "build", "vs2022")
        make_tree(build)
        env = {"GITHUB_WORKSPACE": ws, "GITHUB_SHA": SHA}

        rc, out = run_ps(pub, env)
        published = os.path.join(store_root, SHA)
        marker = os.path.join(published, "published-sha.txt")
        record("publish exits 0", rc == 0, out.strip()[-200:])
        record("publish wrote the marker", os.path.isfile(marker), marker)
        record("publish copied the consumed binaries",
               all(os.path.isfile(os.path.join(published, r)) for r in CONSUMED),
               published)
        record("publish copied the CTest manifest",
               os.path.isfile(os.path.join(published, "tests", "security",
                                           "CTestTestfile.cmake")), "")

        # --- round trip: restore into a WIPED workspace ----------------------
        shutil.rmtree(build, ignore_errors=True)
        rc, out = run_ps(res, env)
        record("restore exits 0", rc == 0, out.strip()[-300:])
        record("restore produced every consumed binary",
               all(os.path.isfile(os.path.join(build, r)) for r in CONSUMED),
               build)

        # --- marker-last: an interrupted publish must be REFUSED -------------
        os.unlink(marker)
        shutil.rmtree(build, ignore_errors=True)
        rc, out = run_ps(res, env)
        record("no marker is REFUSED", rc != 0 and "marker" in out.lower(),
               out.strip()[-200:])
        with io.open(marker, "w", encoding="ascii") as fh:
            fh.write(SHA)

        # --- wrong commit must be REFUSED ------------------------------------
        with io.open(marker, "w", encoding="ascii") as fh:
            fh.write(OTHER)
        rc, out = run_ps(res, env)
        record("commit mismatch is REFUSED",
               rc != 0 and "refusing" in out.lower(), out.strip()[-200:])
        with io.open(marker, "w", encoding="ascii") as fh:
            fh.write(SHA)

        # --- an incomplete store must be REFUSED -----------------------------
        os.unlink(os.path.join(published, "bin", "Release", "pdfe.exe"))
        shutil.rmtree(build, ignore_errors=True)
        rc, out = run_ps(res, env)
        record("incomplete store is REFUSED",
               rc != 0 and "incomplete" in out.lower(), out.strip()[-200:])

        # --- nothing published for this commit must be REFUSED ---------------
        rc, out = run_ps(res, {"GITHUB_WORKSPACE": ws, "GITHUB_SHA": OTHER})
        record("unpublished commit is REFUSED",
               rc != 0 and "no published build" in out.lower(),
               out.strip()[-200:])

        # --- cross-machine: the artifact path feeds the SAME verifier --------
        _self_check_shared_store()
        check_cross_machine(record)

        # ...and the same refusals hold for a store that arrived by DOWNLOAD.
        check_artifact_provenance(record, sandbox, res, store_root)
    finally:
        shutil.rmtree(sandbox, ignore_errors=True)

    print("")
    failed = 0
    for label, ok, detail in results:
        print("  %-42s %s" % (label, "PASS" if ok else "FAIL"))
        if not ok:
            failed += 1
            print("      " + detail.replace("\n", "\n      "))
    print("")
    if failed:
        print("RESULT: %d of %d FAILED" % (failed, len(results)))
        return 1
    print("RESULT: all %d checks pass" % len(results))
    return 0


if __name__ == "__main__":
    sys.exit(main())
