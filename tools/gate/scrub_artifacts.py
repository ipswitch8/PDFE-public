# -*- coding: utf-8 -*-
"""Scrub host-rooted paths out of artifacts a CI step produced directly (F48).

WHY THIS EXISTS. `run_ui_suite.py` scrubs everything it writes, but a workflow
step that invokes `pytest --junitxml=...` ITSELF never goes through that code.
`windows-validation.yml`'s render-verification step does exactly that: it builds
an absolute file list with `Get-ChildItem | ForEach-Object { $_.FullName }`,
passes it straight to pytest, and tees the console into a log. Both
`render-junit.xml` and `render-suite.log` are then uploaded as artifacts with a
14-day retention, carrying the runner account name in pytest's `rootdir:` line
and in every test path.

Found by the `validator` gate as the SIXTH emission point, after it had already
found the fifth. The five closed in the producer were each closed correctly;
the completeness claim around them was what was wrong.

WHY SCRUB RATHER THAN ROUTE THROUGH `run_render_suite.py`. Routing looks tidier
and is the wrong trade here. That runner gives per-file subprocess isolation but
**always exits 0 by design** — its result lives in its summary JSON. The CI step
currently fails the job on a non-zero pytest exit, so routing it through the
runner would have removed the job-failure behaviour unless a new summary-reading
gate were added at the same time. Quietly weakening a CI gate while fixing a
path-hygiene defect is not a trade worth making. This scrubs the artifacts and
changes nothing about what runs or when the job goes red.

Usage:
    python tools/gate/scrub_artifacts.py <repo-root> <file> [<file> ...]
    python tools/gate/scrub_artifacts.py --self-test

Missing files are skipped without error: a step that failed before producing an
artifact must not turn a red job into a differently-red job. Every file's
outcome is printed, so a skip is visible rather than silent.
"""
from __future__ import annotations

import sys
from pathlib import Path

REPO_ROOT_DEFAULT = Path(__file__).resolve().parents[2]
if str(REPO_ROOT_DEFAULT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT_DEFAULT))

from tests.scripts.run_ui_suite import (  # noqa: E402
    _ABS_PATH_RE, scrub_junit_file, scrub_log_file, scrub_text,
)


def scrub_file(path: Path, repo_root: Path) -> str:
    """Scrub one artifact. Returns a one-word outcome for the report."""
    if not path.exists():
        return "absent"
    if path.suffix.lower() == ".xml":
        # Through the XML parser, so escaping stays correct and a replacement
        # cannot land inside a tag.
        return "rewritten" if scrub_junit_file(path, repo_root) else "clean"
    before = path.read_text(encoding="utf-8", errors="replace")
    if not _ABS_PATH_RE.search(before):
        return "clean"
    oversize = scrub_log_file(path, repo_root)
    if oversize is not None:
        return f"skipped-oversize-{oversize}B"
    return "rewritten"


def self_test() -> int:
    """Prove the tool actually removes what it claims to. Returns check count."""
    import tempfile
    failures: list[str] = []
    checks = 0
    tmp = Path(tempfile.mkdtemp(prefix="scrubart_"))
    repo = Path(r"C:\Users\somedev\source\PDFE")
    try:
        log = tmp / "a.log"
        log.write_text(
            "rootdir: C:\\Users\\somedev\\source\\PDFE\n"
            "collecting C:\\Users\\somedev\\source\\PDFE\\tests\\t.py\n",
            encoding="utf-8")
        outcome = scrub_file(log, repo)
        body = log.read_text(encoding="utf-8")
        checks += 1
        if outcome != "rewritten":
            failures.append(f"log outcome {outcome!r}, want 'rewritten'")
        checks += 1
        if "somedev" in body:
            failures.append(f"log still names the account: {body!r}")
        checks += 1
        if _ABS_PATH_RE.search(body):
            failures.append(f"log still has an absolute path: {body!r}")

        xml = tmp / "a.xml"
        xml.write_text(
            '<?xml version="1.0" encoding="utf-8"?>\n'
            '<testsuite name="s"><testcase classname="c" name="t" '
            'file="C:\\Users\\somedev\\source\\PDFE\\tests\\t.py">'
            '<failure message="boom at C:\\Users\\somedev\\x.pdf">body'
            '</failure></testcase></testsuite>',
            encoding="utf-8")
        outcome = scrub_file(xml, repo)
        body = xml.read_text(encoding="utf-8")
        checks += 1
        if outcome != "rewritten":
            failures.append(f"xml outcome {outcome!r}, want 'rewritten'")
        checks += 1
        if "somedev" in body:
            failures.append("xml still names the account")
        checks += 1
        if "&lt;" in body:
            failures.append("a marker was XML-escaped; markers must be "
                            "bracket-safe so the raw artifact stays readable")
        checks += 1
        if "boom at" not in body:
            failures.append("xml lost the surrounding prose")
        checks += 1
        if scrub_file(xml, repo) != "clean":
            failures.append("xml scrub is not idempotent")

        checks += 1
        if scrub_file(tmp / "nope.log", repo) != "absent":
            failures.append("a missing file must report 'absent', not fail")

        # Non-vacuity: a file with nothing to scrub must report 'clean', not
        # 'rewritten' -- otherwise 'rewritten' means nothing.
        plain = tmp / "b.log"
        plain.write_text("nothing to see\n", encoding="utf-8")
        checks += 1
        if scrub_file(plain, repo) != "clean":
            failures.append("a clean file must report 'clean'")
    finally:
        import shutil
        shutil.rmtree(tmp, ignore_errors=True)

    if failures:
        print("[scrub-artifacts] self-test FAILED:", file=sys.stderr)
        for f in failures:
            print("  - " + f, file=sys.stderr)
        return -1
    print(f"[scrub-artifacts] self-test: {checks} checks passed")
    return checks


def main(argv: list[str]) -> int:
    if "--self-test" in argv:
        return 0 if self_test() > 0 else 1
    if len(argv) < 2:
        print(__doc__)
        return 2
    repo_root = Path(argv[0]).resolve()
    worst = 0
    for name in argv[1:]:
        path = Path(name)
        outcome = scrub_file(path, repo_root)
        print(f"[scrub-artifacts] {path.name}: {outcome}")
        if outcome.startswith("skipped-oversize"):
            worst = 1
    return worst


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
