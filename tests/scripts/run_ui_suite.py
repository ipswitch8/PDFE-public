# -*- coding: utf-8 -*-
"""
Phase 2 of pipeline f3c91a2e: per-file pytest isolation runner.

Runs every UI test file in its own pytest subprocess so a single
hung test (e.g. a pywinauto IUIA query that the per-test timeout
can't cancel) only kills its own file's pytest, not the entire
suite.

Background — pytest-timeout's `timeout_method=thread` cannot
interrupt a blocking native COM call (observed at
tests/helpers/file_dialog.py:357 inside _drive_via_uia →
IUIA().iuia.ElementFromHandle). When that fires, pytest-timeout
hard-kills the pytest process via os._exit(1), aborting every
remaining test in the session.

Solution: each UI test file gets a fresh pytest invocation with
a subprocess-level wall-clock timeout. The outer timeout will
kill the pytest+PDFE.exe child tree if the inner pytest-timeout
can't reclaim control.

Outputs:
  <output_dir>/ui-junit-merged.xml   — merged JUnit XML
  <output_dir>/ui-suite-summary.json  — per-file stats
  <output_dir>/per-file/*.xml         — individual junit files
  <output_dir>/per-file/*.log         — individual stdout logs

Exit code: always 0 (failure detail lives in the JSON + XML).
"""
from __future__ import annotations

import argparse
import csv
import json
import os
import re
import shutil
import subprocess
import sys
import time
import xml.etree.ElementTree as ET
from pathlib import Path

# Per-file outer timeout in seconds. The per-test pytest-timeout=120
# is the inner ceiling; this is the outer wall-clock cap for cases
# where a single test or the pytest process itself hangs past 120s
# (e.g. pywinauto IUIA blocking in a way the thread timeout can't
# interrupt).
DEFAULT_FILE_TIMEOUT_S = 1200  # 20 min per file

# Per-file wall-clock overrides for known-heavy UI files whose LEGITIMATE
# (all-passing) runtime exceeds the 20-min default. These files relaunch a
# fresh pdfe.exe per test (~6-8s) across many tests plus slow operations
# (PDF/A conversion, OCR), so their total wall-clock genuinely exceeds the
# default even though every test passes at the 120s per-test cap. Without an
# override the subprocess is killed mid-run and the file's results are lost,
# making a fully-passing file look like a "timeout file" (GAP-A14, Phase 7).
# Keyed by file BASENAME. Characterized 2026-06-14:
#   test_ui_compliance_accessibility.py — 0 failures, ~28+ min total wall-clock.
# IMPORTANT: an override is correct ONLY for files proven to be all-passing-but-
# slow. A file that TRULY hangs (a UIA query the thread-timeout can't interrupt)
# must be FIXED (pipe/introspection seam), not given a longer rope here.
FILE_TIMEOUT_OVERRIDES_S = {
    "test_ui_compliance_accessibility.py": 2700,  # ~45 min cap; measured ~28+ min
    # Characterized 2026-06-14 (Phase-7 loop): every test produces a verdict
    # at the 120s per-test cap (no single-test wedge), but the 45-test file
    # relaunches a fresh pdfe.exe per test plus slow save/reopen+markup ops,
    # so total wall-clock genuinely exceeds the 20-min default. Measured
    # ~1455s (24:15) with 0 failures after the comment-panel seam fix.
    "test_ui_annotations.py": 2400,  # ~40 min cap; measured ~24 min
    # Characterized 2026-06-14 (Phase-7 loop): originally a per-test
    # timeout (test_undo_chain_5_steps / test_redo_chain_after_undo each
    # exhausted a 24-position screen-fraction double-click grid that landed
    # in the canvas letterbox margin, never selecting a span, blowing the
    # 120s per-test cap and hard-killing the file).  FIXED by converting
    # those tests to deterministic PDF-coord double-click + type seams (no
    # weakening).  Full file now completes 37 passed / 0 failed in ~1091s —
    # just under the 1200s default; this generous cap protects against
    # slower CI hardware tipping it over (measured * ~1.6).
    "test_ui_text_editing.py": 1800,  # ~30 min cap; measured ~18 min
    # Characterized 2026-06-15 (loop-until-clean final 3): the 39-test file
    # (PDFESession per-test → a fresh pdfe.exe launch each time) completes
    # 36 passed / 2 skipped / 1 xpassed / 0 failed at the 120s per-test cap —
    # NO single-test wedge (slowest is test_form_field_persistence_across_save
    # _reopen at 107s, under the cap). Total wall-clock measured 1171.89s
    # (19:31), which sits just UNDER the 1200s default — so on marginally
    # slower CI hardware the full-suite runner killed it at exactly 1200s
    # (rc=-1), losing the all-passing results. This generous cap (measured
    # * ~1.5) protects the all-pass-but-slow file from that tip-over. It is
    # NOT a skip: every test still runs and must pass.
    "test_ui_forms.py": 1800,  # ~30 min cap; measured ~19.5 min
    # Cumulative single-session journey soak (pipeline f4a82c1e P4 CI lane). ONE
    # long-lived pdfe.exe driven through a long op sequence against the committed
    # corpus (SpecimenA skip-if-absent). At the CI-bounded JOURNEY_OP_BUDGET=120
    # (set in windows-validation.yml) this measures ~30 min (P2: ~9.5 min/40 ops);
    # a generous 45-min cap absorbs slower CI hardware. NOT a skip — the corpus
    # variant always runs. An extended/nightly budget raises JOURNEY_OP_BUDGET.
    "test_ui_journey_soak.py": 2700,  # ~45 min cap; measured ~30 min @ budget 120
}


def partition_files_for_shard(
    files: list[Path], shard: int, shards: int
) -> list[Path]:
    """Return the subset of ``files`` belonging to 1-based ``shard`` of ``shards``.

    Cost-weighted longest-processing-time-first, NOT round-robin. File runtimes
    span roughly 20x (a handful carry 30-45 min caps in
    ``FILE_TIMEOUT_OVERRIDES_S`` while most finish in a few minutes), and
    round-robin can place several heavy files in the same shard -- putting that
    shard back over the very budget sharding exists to respect.

    Properties this guarantees, all pinned by ``self_test``:
      * every file lands in exactly ONE shard (a partition -- no loss, no dupes)
      * the result is deterministic for a given input list and shard count
      * ``shards == 1`` returns the input unchanged
    """
    if shards < 1:
        raise ValueError("--shards must be >= 1, got %d" % shards)
    if not 1 <= shard <= shards:
        raise ValueError(
            "--shard must be within 1..%d, got %d" % (shards, shard)
        )
    if shards == 1:
        return list(files)

    def weight(path: Path) -> int:
        return FILE_TIMEOUT_OVERRIDES_S.get(path.name, DEFAULT_FILE_TIMEOUT_S)

    # Sort by descending cost, tie-broken by path so the result cannot depend on
    # filesystem enumeration order. Then greedily assign each file to whichever
    # bucket is currently lightest.
    ordered = sorted(files, key=lambda p: (-weight(p), str(p)))
    buckets: list[list[Path]] = [[] for _ in range(shards)]
    loads = [0] * shards
    for path in ordered:
        target = loads.index(min(loads))
        buckets[target].append(path)
        loads[target] += weight(path)

    # Restore the caller's original ordering within the shard: run order is a
    # separate concern from partitioning, and some files are order-sensitive
    # only in the sense that a stable order makes logs comparable run to run.
    chosen = set(buckets[shard - 1])
    return [f for f in files if f in chosen]


def collect_ui_test_files(repo_root: Path) -> list[Path]:
    """Return every UI test file we want to run.

    Scope: every test_ui_*.py at repo_root/tests/ + every
    tests/ui_automation/test_*.py + every tests/visual/test_*.py
    (pipeline c4a9e271 Phase 10 wired the visual-regression layer into the
    gated sweep alongside the ui_automation + test_ui_* layers; the visual
    tests self-skip on unsupported DPI / missing app, so they never spuriously
    fail the sweep on a rig whose scaling is outside the supported set).
    """
    files: list[Path] = []
    tests_dir = repo_root / "tests"
    files.extend(sorted(tests_dir.glob("test_ui_*.py")))
    ua = tests_dir / "ui_automation"
    if ua.is_dir():
        files.extend(sorted(ua.glob("test_*.py")))
    visual = tests_dir / "visual"
    if visual.is_dir():
        files.extend(sorted(visual.glob("test_*.py")))
    return files


#: Precedence used to resolve a row's aggregate status across the committed
#: baseline and every per-file runtime CSV (lower number = higher authority,
#: wins the tie). This is GREEN-WINS aggregation (pipeline c82a1f37 P1):
#:
#:   red (0) > green (1) > wont-fix (2) > xfail (3) > blocked (4) > skip (5) > planned (6)
#:
#: Rationale (false-block root cause): a row like ``act_zoomInAction`` is owned
#: by ``test_ui_batch1_actions`` (records GREEN when the action fires) but is
#: ALSO iterated by a non-owning phase-loading suite (``test_annotations_p1``
#: filters ``owner_phase == phase-5``), which cannot parse it and records
#: BLOCKED. Under the OLD precedence (green=6, blocked=1) the non-owning blocked
#: clobbered the owning green, so ~131 genuinely-passing ``act_`` rows read as
#: blocked. Green-wins fixes this: if ANY suite proves a row green this run, it
#: is green, regardless of how many non-owning suites skip/block it.
#:
#: RED still dominates GREEN (0 < 1): a genuine failure anywhere must surface
#: the row as red — this preserves the GAP-A15 false-green defense and the
#: matrix-health gate's green->red regression detection. (The planner's AC1
#: phrasing "green > red" is read as "green and red both beat blocked/skip";
#: red-dominates is the only ordering that keeps a real failure from being
#: hidden behind a non-owning suite's green.)
_STATUS_PRECEDENCE = {
    "red": 0, "green": 1, "wont-fix": 2, "xfail": 3,
    "blocked": 4, "skip": 5, "planned": 6, "no-run": 7,
}

#: Statuses that are NOT fresh evidence of a result this sweep. A per-file CSV
#: emits ``no-run`` (pipeline e5b9c231 Phase 5) for every row it did not record,
#: replacing the old carry-forward-committed emission. ``""`` is an absent/blank
#: cell. Neither counts as a recorded result during aggregation.
_NON_RESULT_STATUSES = frozenset({"no-run", ""})


# ══════════════════════════════════════════════════════════════════════════════
# Path portability — deferred-backlog row F48 (follow-up phase-65).
#
# WHAT WAS WRONG. Every path this runner put into an artifact was absolute and
# host-rooted. The committed artifact
# `tests/_runtime/discovery_phase8/ui-suite-summary.json` carries 338 of them
# (330 distinct), every one beginning `C:\Users\<developer>\source\PDFE\`, and
# the file is committed because `F19DocClaims.ComparisonDocUiE2eCountsMatchThe
# SummaryArtifact` cites it BY NAME as the evidence for a documented figure.
# So a developer's username entered permanent git history for no functional
# reason: the three consumers that exist -- that GoogleTest, the
# `windows-validation.yml` gating step, and `tools/gate/verify_f19_doc_claims.py`
# -- read SCALARS ONLY (`files_run`, `failure_count`, `totals.*`,
# `files_with_problems_count`). Nothing has ever read a path.
#
# WHY RELATIVE IS ALSO THE BETTER DATA, not merely the safer data. An absolute
# path is only meaningful on the machine that produced it; `tests/test_ui_
# annotations.py` identifies the same file on every checkout, which is what a
# committed artifact is FOR. This is a fix, not a redaction tax.
#
# THE RULE, applied uniformly so there is one definition rather than a habit:
# nothing that leaves this module in an artifact is absolute. A path under the
# repo becomes repo-relative; a path under the run's own output directory (when
# that sits outside the repo, as it can on a runner) becomes `[output]/...`;
# anything else keeps its basename only, behind `[redacted]/`. The basename is
# retained deliberately -- dropping it would make a failure report unreadable to
# buy nothing, since a basename is not host-identifying.
#
# STATED LIMIT. `_ABS_PATH_RE` recognises Windows drive-letter and UNC roots.
# PDFE is Windows-only today (see CLAUDE.md), and a POSIX `/home/<user>/...`
# root would NOT be found by the text scrubber -- `portable_path` itself is
# separator-agnostic and handles it, but `scrub_text` would not spot it inside a
# log line. Recorded here rather than left for a future port to discover.
# ══════════════════════════════════════════════════════════════════════════════

#: Marker for a path that is neither under the repo nor under the run's output
#: directory. Only its basename survives.
#:
#: SQUARE brackets, not angle brackets, and that is a correctness requirement
#: rather than taste. These markers are written into JUnit **XML** as well as
#: JSON and plain logs, and `<` is not legal in XML character data or an
#: attribute value — an angle-bracketed marker would have to be escaped to
#: `&lt;redacted&gt;`, which is legible to a parser and confusing to a human
#: reading the raw artifact. Square brackets need no escaping in any of the
#: three formats.
REDACTED_PREFIX = "[redacted]"

#: Marker for a path under the run's output directory when that directory is
#: not itself inside the repo.
OUTPUT_PREFIX = "[output]"

#: Windows drive-letter (`C:\...`, `C:/...`) or UNC (`\\host\share\...`) roots.
#: Stops at whitespace, quotes and characters illegal in a Windows path, so a
#: path embedded in prose or an assertion message is matched without swallowing
#: the sentence around it.
_ABS_PATH_RE = re.compile(r'(?:[A-Za-z]:[\\/]|\\\\)[^\s"\'<>|*?\r\n]*')

#: A RELATIVE path that climbs out of the tree. pytest reports test files
#: relative to its rootdir, so anything outside the repo arrives as
#: `..\..\Users\<account>\...` — carrying the home directory, and therefore the
#: account name, with no drive letter for `_ABS_PATH_RE` to anchor on. Requires
#: at least one `..` segment: a plain relative path like `tests/foo.py` is
#: already portable and must be left alone, or every log line would be redacted.
_CLIMBING_REL_RE = re.compile(r'(?:\.\.[\\/])+[^\s"\'<>|*?\r\n]*')

#: What a surviving account name is replaced with. Distinct from
#: `REDACTED_PREFIX` so a reader can tell "a path was redacted" from "the
#: account name reached here by a route the path rules did not cover", which is
#: a weaker position and worth seeing in the artifact.
REDACTED_ACCOUNT = "[redacted-account]"

#: Above this size a per-file log is left unscrubbed rather than read into
#: memory. The runner REPORTS that in the summary (`log_scrub_skipped_bytes`)
#: instead of passing over it silently: a cap nobody is told about reads as
#: coverage that was never there.
LOG_SCRUB_MAX_BYTES = 128 * 1024 * 1024


def _norm(s: str) -> str:
    """Case-folded, forward-slashed, trailing-separator-free form for compares."""
    return str(s).replace("\\", "/").rstrip("/").lower()


def account_name() -> str:
    """The account this process runs as, or "" if it cannot be determined.

    Used to close a hole the `[redacted]/<basename>` rule had on its own: the
    basename is kept because a FILE name is not host-identifying, but the
    basename of a HOME DIRECTORY is exactly the account name. Found by
    tools/gate/f48_producer_probe.py, which caught pytest printing
    `rootdir: C:\\Users\\<account>` and watched it come out as
    `[redacted]/<account>` -- redacted in form and leaking in substance.
    """
    for value in (os.environ.get("USERNAME", ""), os.environ.get("USER", "")):
        if value and len(value) >= 3:
            return value
    return ""


def _is_absolute_str(raw: str) -> bool:
    """True for a Windows drive-letter root, a UNC root, or a POSIX root."""
    if len(raw) >= 3 and raw[1] == ":" and raw[2] in "\\/" and raw[0].isalpha():
        return True
    if raw.startswith("\\\\") or raw.startswith("//"):
        return True
    return raw.startswith("/")


def portable_path(p, repo_root, out_root=None, account=None) -> str:
    """Render `p` as a host-independent string safe to write into an artifact.

    Never returns an absolute path. Already-portable input is returned
    unchanged, which makes this idempotent — the scrubber runs over a per-file
    log again on every rerun of that file, and a second pass must not turn
    `tests/foo.py` into `[redacted]/foo.py`.
    """
    raw = str(p).replace("\\", "/")
    if not _is_absolute_str(str(p)):
        return raw
    # A trailing separator is not part of the name. Left in, `C:/Users/dev/`
    # yields an empty basename and the account check never sees the name it
    # exists to catch.
    raw = raw.rstrip("/") or raw

    here = _norm(raw)
    root = _norm(repo_root)
    if here == root:
        return "."
    if here.startswith(root + "/"):
        return raw[len(root) + 1:]

    if out_root is not None:
        out = _norm(out_root)
        if here == out:
            return OUTPUT_PREFIX
        if here.startswith(out + "/"):
            return OUTPUT_PREFIX + "/" + raw[len(out) + 1:]

    # The basename is kept because a file name is not host-identifying -- and
    # dropping it would make a failure report unreadable to buy nothing. The
    # one exception is the case the probe found: when the path IS the home
    # directory, its basename is the account name, so keeping it would defeat
    # the whole row while still looking redacted.
    name = raw.rsplit("/", 1)[-1]
    if "/" not in raw or not name:
        return REDACTED_PREFIX  # a bare drive or UNC root has no useful name
    acct = account_name() if account is None else account
    if acct and name.lower() == acct.lower():
        return REDACTED_PREFIX
    return REDACTED_PREFIX + "/" + name


def scrub_text(text: str, repo_root, out_root=None, account=None) -> str:
    """Replace every absolute path inside `text` with its portable form.

    Used on failure messages and on the captured per-file pytest logs, both of
    which quote host paths (`rootdir=`, tracebacks, fixture paths) that no
    consumer reads but that carry the username all the same.

    THREE PASSES, AND THE SECOND ONE IS WHY (GitHub F48, measured 2026-10-05).
    `_ABS_PATH_RE` recognises drive-letter and UNC roots — a limit this module
    already stated. pytest defeats it: it prints paths RELATIVE to its rootdir,
    so a test file outside the repo comes out climbing, and the climb walks
    straight through the home directory:

        ..\\..\\Users\\<account>\\AppData\\Local\\Temp\\2\\f48probe_xxxx\\test_f48_probe.py

    No drive letter, no UNC prefix, so pass one never matched it and the account
    name reached the committed per-file log. `tests/test_ui_suite_path_hygiene.cpp`
    caught it on the first CI run that ever reached core-tests.

    Pass three is a BACKSTOP, deliberately last and deliberately blunt: if the
    account name still appears by any route this function did not anticipate, it
    is replaced outright. A path shape can be missed again; the account name is
    the thing that must not survive, so it gets a check that does not depend on
    recognising a path at all.
    """
    acct = account_name() if account is None else account

    out = _ABS_PATH_RE.sub(
        lambda m: portable_path(m.group(0), repo_root, out_root, account),
        text)

    def _redact_climb(m):
        """`[redacted]/<basename>`, NOT portable_path.

        portable_path only redacts roots it recognises as absolute; handed a
        relative path it just normalises the separators and hands it back —
        measured: `..\\..\\Users\\x\\...` came out `../../Users/x/...`, still
        naming the home directory. So this pass was inert and the account
        backstop was silently doing all the work, which the strengthened
        self-test exposed. Redact directly here.

        The basename-is-the-account check mirrors portable_path's: a home
        directory's basename IS the account name, so `[redacted]/<account>`
        would be redacted in shape and leaking in substance.
        """
        raw = m.group(0).replace("\\", "/").rstrip("/")
        base = raw.rsplit("/", 1)[-1]
        if not base or base in (".", "..") or (acct and base.lower() == acct.lower()):
            return REDACTED_PREFIX
        return REDACTED_PREFIX + "/" + base

    out = _CLIMBING_REL_RE.sub(_redact_climb, out)

    if acct and acct in out:
        out = out.replace(acct, REDACTED_ACCOUNT)
    return out


def scrub_log_file(log_path: Path, repo_root, out_root=None):
    """Rewrite `log_path` with absolute paths replaced. Returns None normally.

    Returns the file size when the log was too large to scrub, so the caller
    can record the miss in the summary rather than let it pass unnoticed.
    """
    try:
        size = log_path.stat().st_size
    except OSError:
        return None
    if size > LOG_SCRUB_MAX_BYTES:
        return size
    try:
        text = log_path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return None
    scrubbed = scrub_text(text, repo_root, out_root)
    if scrubbed != text:
        try:
            log_path.write_text(scrubbed, encoding="utf-8")
        except OSError:
            pass
    return None


def scrub_junit_file(xml_path: Path, repo_root, out_root=None) -> bool:
    """Rewrite `xml_path`'s JUnit XML with absolute paths replaced.

    THE FIFTH EMISSION POINT, and the one the first pass of this fix missed.
    pytest writes `--junitxml` itself, so its content never passes through this
    module: `rootdir`, the failing test's file path and the whole assertion
    traceback land in the `message` attribute and the failure body verbatim.
    Measured on a probe: **4 account-name occurrences and 5 absolute paths** in
    a per-file XML that the JSON summary beside it reported clean. Scrubbing
    `collect_failures`'s derived `msg` cleaned the COPY and left the original.

    Both the per-file XMLs and the merged `ui-junit-merged.xml` built from them
    are uploaded as CI artifacts, so both are in scope. Merging happens after
    this, from these files, so scrubbing here covers the merged one too.

    Goes through the parser rather than the raw text on purpose: the markers
    and any replacement must be XML-escaped correctly, and a text substitution
    that happens to land inside a tag would produce a file no consumer can
    parse. Returns True if the file was rewritten.
    """
    try:
        tree = ET.parse(str(xml_path))
    except (ET.ParseError, OSError):
        # A crashed pytest can leave a truncated XML. Nothing to scrub that we
        # can do safely; the caller reports the miss rather than hiding it.
        return False

    changed = False

    def fix(value):
        nonlocal changed
        if not value or not _ABS_PATH_RE.search(value):
            return value
        changed = True
        return scrub_text(value, repo_root, out_root)

    for el in tree.getroot().iter():
        for key, val in list(el.attrib.items()):
            new = fix(val)
            if new != val:
                el.set(key, new)
        el.text = fix(el.text)
        el.tail = fix(el.tail)

    if changed:
        try:
            tree.write(str(xml_path), encoding="utf-8", xml_declaration=True)
        except OSError:
            return False
    return changed


def _read_status_map(csv_path: Path) -> dict[str, str]:
    out: dict[str, str] = {}
    try:
        with open(csv_path, "r", encoding="utf-8", newline="") as f:
            for row in csv.DictReader(f):
                tid = (row.get("test_id") or "").strip()
                if tid:
                    out[tid] = (row.get("status") or "").strip()
    except (OSError, csv.Error):
        pass
    return out


def aggregate_matrix(committed_csv: Path, new_runtime_csvs: list[Path],
                     out_path: Path) -> dict:
    """Overlay this run's per-file matrix CSVs onto the committed closeout matrix.

    Each per-file pytest subprocess writes a full matrix CSV in which the rows IT
    recorded carry a real status (``green``/``red``/``blocked``/``wont-fix``) and
    every OTHER row carries ``no-run`` (pipeline e5b9c231 Phase 5 — the honest
    emission that replaced carry-forward-committed). ``no-run`` is a sentinel for
    "this subprocess did not assert this row", NOT evidence of a result.

    Because untouched rows are now ``no-run`` (not a copy of the committed
    status), there is no carry-forward noise to filter and no need to pool the
    committed status as if it were live evidence. We build, per row, a POOL of
    every REAL per-file status (excluding ``no-run``/blank) and resolve to the
    best under green-wins precedence (``_STATUS_PRECEDENCE``: red dominates,
    green beats blocked/skip). A row with NO real result anywhere this sweep
    resolves to ``no-run`` — a committed ``green`` that nobody exercised this
    run is NOT resurrected as green (the H4 "runtime rebuilt from committed
    green" defect). ``check_matrix_health.py --require-ran`` then flags such
    rows on a full sweep, while a partial leg tolerates them.
    """
    committed_status = _read_status_map(committed_csv)
    # tid -> set of REAL per-file statuses recorded this sweep (no-run excluded).
    observed: dict[str, set[str]] = {}
    for csvp in new_runtime_csvs:
        for tid, st in _read_status_map(csvp).items():
            if st in _NON_RESULT_STATUSES:
                continue  # not a recorded result this subprocess
            observed.setdefault(tid, set()).add(st)

    # Resolve each committed row to the best REAL status this sweep, or 'no-run'
    # when nothing exercised it. Committed status is NOT pooled as evidence.
    changes: dict[str, str] = {}
    for tid, cst in committed_status.items():
        pool = observed.get(tid)
        best = min(pool, key=lambda s: _STATUS_PRECEDENCE.get(s, 99)) \
            if pool else "no-run"
        if best != cst:
            changes[tid] = best

    # Write the committed matrix verbatim with the recorded changes overlaid.
    with open(committed_csv, "r", encoding="utf-8", newline="") as f:
        reader = csv.DictReader(f)
        fieldnames = reader.fieldnames or []
        rows = list(reader)
    for row in rows:
        tid = (row.get("test_id") or "").strip()
        if tid in changes:
            row["status"] = changes[tid]
    out_path.parent.mkdir(parents=True, exist_ok=True)
    with open(out_path, "w", encoding="utf-8", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)

    green = sum(1 for r in rows if (r.get("status") or "").strip() == "green")
    return {
        "aggregate_csv": str(out_path),
        "rows": len(rows),
        "green": green,
        "rows_changed_from_committed": len(changes),
    }


def merge_shard_summaries(summaries: list) -> dict:
    """Combine per-shard ui-suite summaries into one run-wide summary.

    GitHub #278. Every shard writes a summary describing ITS slice, and the three
    files have the same name, so collecting them into one directory leaves one
    shard's numbers standing for the sweep. #277 records the cost: "2,500 tests,
    16 failures" was reported twice before someone noticed a later shard had
    replaced the file with "560 tests, 14 failures".

    This is a pure function over already-parsed summaries, so it can be tested
    without running the suite -- the point being that a merge nobody can exercise
    is how the overwrite survived in the first place.

    What it guarantees, and what the tests pin:
      * totals are SUMMED, not taken from any one shard;
      * `per_file`, `failures` and `files_with_problems` are CONCATENATED, so no
        shard's rows are lost;
      * `files_run` is the sum, not the max;
      * the result declares the shard set it was built from, and is marked
        partial if any expected shard is MISSING -- a merge of two shards out of
        three is not a sweep either, and saying so is the whole point.
    """
    if not summaries:
        raise ValueError("no shard summaries to merge")

    merged_totals = {"tests": 0, "failures": 0, "errors": 0, "skipped": 0,
                     "time": 0.0}
    per_file: list = []
    failures: list = []
    problems: list = []
    shards_seen = set()
    shards_expected = 0
    wall_clock = 0.0

    for s in summaries:
        for key in merged_totals:
            merged_totals[key] += (s.get("totals") or {}).get(key, 0) or 0
        per_file.extend(s.get("per_file") or [])
        failures.extend(s.get("failures") or [])
        problems.extend(s.get("files_with_problems") or [])
        wall_clock += s.get("wall_clock_s") or 0.0
        if s.get("shard"):
            shards_seen.add(int(s["shard"]))
        shards_expected = max(shards_expected, int(s.get("shards") or 0))

    merged_totals["time"] = round(merged_totals["time"], 3)
    missing = sorted(set(range(1, shards_expected + 1)) - shards_seen) \
        if shards_expected else []

    return {
        "files_run": len(per_file),
        "wall_clock_s": round(wall_clock, 1),
        "totals": merged_totals,
        "failure_count": len(failures),
        "per_file": per_file,
        "failures": failures,
        "files_with_problems_count": len(problems),
        "files_with_problems": problems,
        # GitHub #301. The SECOND site. A per-shard summary gained
        # `app_crash_count`, and a merge that did not carry it would leave a
        # sharded run -- which is how CI runs, 3 shards -- with the key absent:
        # a `.get("app_crash_count", 0)` reader then sees 0 and the crash is
        # invisible again one level up, exactly the #278 mistake repeated.
        # Derived from the CONCATENATED rows rather than summed from the
        # shards, so it cannot disagree with `per_file`.
        "app_crash_count": len([r for r in per_file
                                if r.get("unhealable_app_crash")
                                or r.get("app_crashed")]),
        "shards_merged": sorted(shards_seen),
        "shards_expected": shards_expected,
        "shards_missing": missing,
        # A merge missing a shard is still not the sweep. Readers that only look
        # at totals would otherwise repeat the original mistake one level up.
        "is_partial": bool(missing) or not shards_seen,
    }


def file_has_problem(r: dict) -> bool:
    """Did this file fail to produce a trustworthy pass?

    One definition, used by the retry loop and by the summary, so a test can
    exercise the REAL predicate instead of a copy of it that drifts.

    GitHub #265: a file whose `pdfe.exe` faulted stays a problem even if a
    later attempt passed. Without that clause the crash heals into a green
    summary, which is the whole defect.
    """
    if r.get("unhealable_app_crash"):
        return True
    if r.get("timed_out"):
        return True
    rc = r.get("returncode")
    return rc is not None and rc != 0


def run_one(
    pytest_target: Path,
    junit_path: Path,
    log_path: Path,
    timeout_s: int,
    repo_root: Path,
    pdfe_exe: str,
    reap: bool = True,
    crash_dump_dir: Path | None = None,
) -> dict:
    """Run pytest on a single test file in a subprocess.

    `reap` controls the machine-wide pdfe.exe cleanup below; see the comment
    at that call. Leave it True for any real sweep.

    `crash_dump_dir` (GitHub #265) points `PDFE_CRASH_DUMP_DIR` at a directory
    for THIS attempt, so a `pdfe.exe` that faults writes a minidump there and
    the result carries `app_crash_dumps`. Without it there is no signal at all:
    the application under test is a separate process, so its death reaches this
    runner only as a failing or timed-out pytest file -- exactly the shape the
    retry loop is built to absorb.

    Optional and defaulted off so the two other importers
    (`run_render_suite.py`, `tools/gate/f48_producer_probe.py`) are unaffected.
    """
    cmd = [
        sys.executable, "-m", "pytest",
        str(pytest_target),
        f"--junitxml={junit_path}",
        "--timeout=120",
        "--timeout-method=thread",
        "-ra",
        "-p", "no:cacheprovider",
    ]
    env = os.environ.copy()
    env["PDFE_EXE"] = pdfe_exe
    if crash_dump_dir is not None:
        crash_dump_dir.mkdir(parents=True, exist_ok=True)
        env["PDFE_CRASH_DUMP_DIR"] = str(crash_dump_dir)
    start = time.monotonic()
    try:
        with open(log_path, "w", encoding="utf-8", errors="replace") as logf:
            proc = subprocess.run(
                cmd,
                cwd=str(repo_root),
                stdout=logf,
                stderr=subprocess.STDOUT,
                timeout=timeout_s,
                env=env,
            )
        rc = proc.returncode
        timed_out = False
    except subprocess.TimeoutExpired:
        rc = -1
        timed_out = True
    elapsed = time.monotonic() - start

    # Defensive cleanup: kill any pdfe.exe leaked by a hung test so
    # the next file's tests don't inherit a dead window.
    #
    # `reap` exists for ONE caller: tools/gate/f48_producer_probe.py, which
    # drives this function from inside the gtest suite over a test file that
    # launches no application. That suite runs 16 shards CONCURRENTLY and
    # several of them (ShellAndInstanceLaunchTest, DllDependencyTest,
    # ProcessMitigations) have a live pdfe.exe child. A machine-wide
    # `taskkill /IM pdfe.exe` from the probe would kill another shard's child
    # and red a test that has nothing to do with it -- a cross-shard failure
    # that would look like flake and be hunted as one. Default stays True; the
    # real sweep is unchanged.
    if reap:
        try:
            subprocess.run(
                ["taskkill", "/F", "/IM", "pdfe.exe"],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                timeout=15,
            )
        except Exception:
            pass

    # F48. The per-file log is pytest's own stdout, which quotes host paths
    # (`rootdir=`, tracebacks, fixture paths). Scrub it after the subprocess
    # has exited rather than filtering the stream: nothing reads the log until
    # now, and a post-pass keeps the streaming write path unchanged.
    #
    # `out_root` is derived rather than passed so that run_render_suite.py --
    # which imports run_one and lays its artifacts out identically -- gets the
    # same treatment without a second call site to keep in step. Sibling drift
    # between two copies of one rule is the shape DCA 0015 records.
    out_root = junit_path.parent.parent
    oversize = scrub_log_file(log_path, repo_root, out_root)
    # The JUnit XML pytest wrote, before anything reads or merges it.
    scrub_junit_file(junit_path, repo_root, out_root)

    result = {
        "file": portable_path(pytest_target, repo_root, out_root),
        "junit": portable_path(junit_path, repo_root, out_root),
        "log": portable_path(log_path, repo_root, out_root),
        "returncode": rc,
        "elapsed_s": round(elapsed, 1),
        "timed_out": timed_out,
    }
    if oversize is not None:
        result["log_scrub_skipped_bytes"] = oversize
    if crash_dump_dir is not None:
        # GitHub #265. Names only -- a minidump holds process heap, which for
        # this application can include document passwords, decrypted content
        # and absolute host paths. It stays in the gitignored runtime tree and
        # is never copied into a report.
        dumps = sorted(p.name for p in crash_dump_dir.glob("*.dmp"))
        result["app_crash_dumps"] = dumps
        result["app_crashed"] = bool(dumps)
    return result


def inject_app_crash_cases(merged_root: "ET.Element", results: list) -> int:
    """GitHub #301 — an app crash must be RED in the merged artifact.

    THE DEFECT. During the p96 phase-7 re-measurement, `pdfe.exe` faulted and
    wrote a 46 MB minidump 9 s into `test_final_phase8[annot_select_delete]`,
    and `tests/ui_automation/test_final_phase8.py` reported **11 passed, 0
    failed**. The harness was not blind — `ui-suite-summary.json` carried
    `"app_crashed": true, "unhealable_app_crash": true` and
    `file_has_problem()` counted it among "hidden problems: 17" — but nothing
    turned it into a FAILURE, so it never reached the headline count and the
    merged JUnit stayed green.

    A green JUnit over a process that dumped core is the same class as #288 and
    #300: the data layer knew and the verdict did not. The fix is the same
    shape, and for the same reason — the classification is written into the
    artifact a reader consumes, as a `<testcase>` with an `<error>`, because
    that is the one thing no JUnit reader ignores.

    WHY NOT CHANGE THE EXIT CODE. `main()` ends `return 0  # always succeed --
    failure detail lives in the artifacts`, and that is a deliberate contract:
    the sweep is a measurement harness whose callers continue to the
    aggregation step. Flipping it would change behaviour well beyond this
    issue. Making the artifact honest closes #301 without touching that.

    The synthetic case is named for the FILE whose app crashed, because that is
    what the harness actually knows. #301 identified the individual test from
    the launch/dump timeline, which the harness cannot do.

    Returns the number of cases injected.
    """
    crashed = [r for r in results if r.get("unhealable_app_crash")
               or r.get("app_crashed")]
    if not crashed:
        return 0
    suite = ET.SubElement(merged_root, "testsuite", {
        "name": "ApplicationCrash",
        "tests": str(len(crashed)),
        "failures": "0",
        "errors": str(len(crashed)),
        "skipped": "0",
        "time": "0",
    })
    for r in crashed:
        # `file` is the key run_one writes (already path-scrubbed by
        # portable_path, so this does not leak the account name into an
        # artifact -- check_tracked_path_hygiene.py cares).
        target = str(r.get("file") or "unknown")
        safe = target.replace("\\", "/").replace("/", "__").replace(".py", "")
        msg = (f"pdfe.exe FAULTED while {target} was driving it. The tests in "
               f"that file may each have reported PASS; a process that dumps "
               f"core has not passed. Look for a minidump under this run's "
               f"dumps directory. Recorded here because the merged JUnit "
               f"otherwise stays green over a crashed application "
               f"(GitHub #301).")
        tc = ET.SubElement(suite, "testcase", {
            "name": f"app_crashed_during_{safe}",
            "classname": "ApplicationCrash",
            "status": "run",
            "time": "0",
        })
        err = ET.SubElement(tc, "error", {
            "message": msg,
            "type": "app-crash",
        })
        err.text = msg
    return len(crashed)


def merge_junits(per_file_xmls: list[Path], merged_path: Path,
                 results: list | None = None) -> dict:
    """Merge per-file JUnit XML into a single result document.

    `results` is optional only so an older call site cannot break; pass it.
    When given, a file whose application crashed contributes a synthetic error
    case so the merged artifact is not green over a process that dumped core
    (GitHub #301 -- see `inject_app_crash_cases`).
    """
    merged_root = ET.Element("testsuites")
    totals = {"tests": 0, "failures": 0, "errors": 0, "skipped": 0, "time": 0.0}
    for xml_path in per_file_xmls:
        if not xml_path.exists():
            continue
        try:
            tree = ET.parse(str(xml_path))
            root = tree.getroot()
        except ET.ParseError:
            continue
        # JUnit XML root may be <testsuites> or directly <testsuite>
        if root.tag == "testsuites":
            for ts in root.findall("testsuite"):
                merged_root.append(ts)
        elif root.tag == "testsuite":
            merged_root.append(root)
        for ts in merged_root.findall("testsuite"):
            pass  # placeholder
    # GitHub #301 -- before totals, so the crash reaches the root attributes
    # too. An attribute-trusting reader is the other door to the same false
    # green (GAP-A15 cuts both ways).
    if results:
        inject_app_crash_cases(merged_root, results)
    # Recompute totals from the actual merged tree.
    for ts in merged_root.findall("testsuite"):
        totals["tests"] += int(ts.get("tests", "0") or 0)
        totals["failures"] += int(ts.get("failures", "0") or 0)
        totals["errors"] += int(ts.get("errors", "0") or 0)
        totals["skipped"] += int(ts.get("skipped", "0") or 0)
        try:
            totals["time"] += float(ts.get("time", "0") or 0)
        except ValueError:
            pass
    merged_root.set("tests", str(totals["tests"]))
    merged_root.set("failures", str(totals["failures"]))
    merged_root.set("errors", str(totals["errors"]))
    merged_root.set("skipped", str(totals["skipped"]))
    merged_root.set("time", f"{totals['time']:.2f}")
    ET.ElementTree(merged_root).write(
        str(merged_path), encoding="utf-8", xml_declaration=True
    )
    return totals


def collect_failures(per_file_xmls: list[Path], repo_root=None,
                     out_root=None) -> list[dict]:
    """Extract every failing or erroring testcase across all junit files.

    `repo_root` is optional only so an older call site cannot break; pass it.
    When given, each failure message is scrubbed of absolute paths (F48) --
    assertion text is the other place host paths reach the summary artifact,
    and it is the one that is easy to forget because it is prose, not a field
    whose name says "path".
    """
    out: list[dict] = []
    for xml_path in per_file_xmls:
        if not xml_path.exists():
            continue
        try:
            tree = ET.parse(str(xml_path))
        except ET.ParseError:
            continue
        for tc in tree.iter("testcase"):
            failed = tc.find("failure") is not None
            errored = tc.find("error") is not None
            if not (failed or errored):
                continue
            kind = "failure" if failed else "error"
            node = tc.find(kind)
            msg = (node.get("message", "") if node is not None else "")[:200]
            if repo_root is not None:
                msg = scrub_text(msg, repo_root, out_root)
            out.append({
                "classname": tc.get("classname", ""),
                "name": tc.get("name", ""),
                "time_s": float(tc.get("time", "0") or 0),
                "kind": kind,
                "msg": msg,
            })
    return out


def self_test() -> int:
    """Assert the F48 path-portability contract. Returns the number of checks.

    This exists as a mode of the producer rather than as a separate test file
    because the GoogleTest guard that gates it
    (`UiSuitePathHygiene.TheProducerRefusesToEmitHostRootedPaths`) has to
    exercise the REAL functions. A C++ test that re-implemented the rule, or
    that grepped this file for a call, would pass while the producer was
    broken -- and a sweep keyed on phrasing always comes back.
    """
    repo = r"C:\Users\somedev\source\PDFE"
    outside_out = r"D:\ci\artifacts\ui-out"
    failures: list[str] = []
    checks = 0

    def check(label, got, want):
        nonlocal checks
        checks += 1
        if got != want:
            failures.append(f"{label}: got {got!r}, want {want!r}")

    # 1. Under the repo -> repo-relative, forward-slashed.
    check("repo file",
          portable_path(repo + r"\tests\test_ui_annotations.py", repo),
          "tests/test_ui_annotations.py")
    check("repo output artifact",
          portable_path(repo + r"\tests\_runtime\d8\per-file\a.xml", repo,
                        repo + r"\tests\_runtime\d8"),
          "tests/_runtime/d8/per-file/a.xml")
    check("repo root itself", portable_path(repo, repo), ".")

    # 2. Under an output dir that is NOT inside the repo -> [output]/...
    check("out-of-repo artifact",
          portable_path(outside_out + r"\per-file\a.log", repo, outside_out),
          OUTPUT_PREFIX + "/per-file/a.log")

    # 3. Anything else -> basename only, behind [redacted]/.
    check("unrelated absolute",
          portable_path(r"C:\Users\somedev\AppData\Local\Temp\x.pdf", repo),
          REDACTED_PREFIX + "/x.pdf")
    check("UNC",
          portable_path(r"\\fileserver\share\secret\y.pdf", repo),
          REDACTED_PREFIX + "/y.pdf")

    # 3b. THE HOLE THE END-TO-END PROBE FOUND. `[redacted]/<basename>` is safe
    #     for a file and unsafe for the home directory itself, whose basename
    #     IS the account name. pytest prints `rootdir: C:\Users\<account>` when
    #     it cannot find a project root, and that came out as
    #     `[redacted]/<account>` -- redacted in form, leaking in substance.
    #     `account` is injected here so this case tests the real rule rather
    #     than whoever happens to be running the suite.
    check("home directory basename is the account name",
          portable_path(r"C:\Users\somedev", repo, account="somedev"),
          REDACTED_PREFIX)
    check("home directory, trailing separator",
          portable_path("C:/Users/somedev/", repo, account="somedev"),
          REDACTED_PREFIX)
    check("a file under home still keeps its name",
          portable_path(r"C:\Users\somedev\notes.txt", repo,
                        account="somedev"),
          REDACTED_PREFIX + "/notes.txt")
    check("account match is case-insensitive",
          portable_path(r"C:\Users\SomeDev", repo, account="somedev"),
          REDACTED_PREFIX)
    check("scrubbed rootdir line carries no account name",
          "somedev" in scrub_text("rootdir: C:\\Users\\somedev", repo,
                                  account="somedev"),
          False)

    # 4. Idempotence. The scrubber runs again over the same log on every rerun
    #    of a file; a second pass must not degrade `tests/foo.py` to
    #    `[redacted]/foo.py`.
    once = portable_path(repo + r"\tests\test_ui_forms.py", repo)
    check("idempotent", portable_path(once, repo), once)
    check("idempotent redacted",
          portable_path(REDACTED_PREFIX + "/x.pdf", repo),
          REDACTED_PREFIX + "/x.pdf")

    # 5. Case-insensitive root match: Windows hands back mixed case (`c:\users`
    #    vs `C:\Users`) depending on who resolved the path.
    check("case-insensitive root",
          portable_path(repo.lower() + r"\tests\a.py", repo), "tests/a.py")

    # 6. Text scrubbing, which is where a failure MESSAGE leaks a path.
    msg = (f"assert failed at {repo}\\tests\\test_ui_forms.py:12 "
           f"opening C:\\Users\\somedev\\AppData\\Temp\\t.pdf now")
    got = scrub_text(msg, repo)
    check("scrub repo path", "tests/test_ui_forms.py:12" in got, True)
    check("scrub keeps trailing prose", got.endswith(" now"), True)
    check("scrub redacts foreign path",
          REDACTED_PREFIX + "/t.pdf" in got, True)
    check("scrub leaves no drive letter", ":\\" in got or ":/" in got, False)
    check("scrub idempotent", scrub_text(got, repo), got)

    # 6b. CLIMBING RELATIVE PATHS (F48, 2026-10-05). pytest prints test files
    #     relative to its rootdir, so a file outside the repo arrives as
    #     `..\..\Users\<account>\...` — no drive letter, so pass one never saw
    #     it and the account name reached the committed per-file log. Caught in
    #     CI by UiSuitePathHygiene.ARealRunnerInvocationEmitsNothingHostRooted,
    #     on the first run that ever got as far as core-tests.
    climb = (r"..\..\Users\somedev\AppData\Local\Temp\2\f48probe_x"
             r"\test_f48_probe.py . [ 50%]")
    climbed = scrub_text(climb, repo, None, "somedev")
    #     ASSERTS THE PATH COLLAPSED, NOT MERELY THAT THE NAME IS GONE. A first
    #     version checked only `"somedev" not in climbed`, which the pass-three
    #     backstop satisfies on its own — so disabling the climbing pass left the
    #     check GREEN. Verified by mutation. With only the backstop the text
    #     still reads `..\..\Users\[redacted-account]\AppData\...`: host-rooted
    #     in shape, and still disclosing the directory layout.
    check("climbing relative path collapses to the redacted form",
          REDACTED_PREFIX + "/test_f48_probe.py" in climbed, True)
    check("climbing relative leaves no home-directory segments",
          "AppData" in climbed or "Users" in climbed, False)
    check("climbing relative path is scrubbed",
          "somedev" in climbed, False)
    check("climbing relative keeps trailing prose",
          climbed.endswith("[ 50%]"), True)
    #     NEGATIVE CONTROL: an ordinary relative path is already portable and
    #     must survive untouched, or every log line would come out redacted.
    check("plain relative path is NOT touched",
          scrub_text("tests/test_ui_forms.py:12 passed", repo),
          "tests/test_ui_forms.py:12 passed")
    check("climbing scrub idempotent", scrub_text(climbed, repo, None,
                                                  "somedev"), climbed)

    # 6c. The ACCOUNT BACKSTOP. Last pass, blunt on purpose: a path shape can be
    #     missed again, but the account name must not survive by any route. This
    #     input has no path shape at all, so only the backstop can catch it.
    bare = scrub_text("user=somedev logged in", repo, None, "somedev")
    check("bare account name is redacted", "somedev" in bare, False)
    check("backstop marks itself distinctly",
          REDACTED_ACCOUNT in bare, True)
    check("backstop leaves other text alone",
          bare.startswith("user=") and bare.endswith(" logged in"), True)

    # 7. The property the whole row is about: no username survives.
    check("no username", "somedev" in scrub_text(msg, repo), False)

    # 7b. THE PROSE MUST NAME THE REAL MARKERS. The `karen` gate FAILED this
    #     phase over exactly this: the markers changed from angle brackets to
    #     square ones for the XML reason below, and the "THE RULE" comment ten
    #     lines above the constants still described the old behaviour in the
    #     present tense. No functional impact, and precisely the defect class
    #     this project exists to catch — a record that no longer matches the
    #     code it describes. A comment cannot be trusted to stay true; this
    #     makes it fail out loud instead.
    try:
        _own_source = Path(__file__).read_text(encoding="utf-8")
    except OSError:
        _own_source = ""
    _rule_start = _own_source.find("# THE RULE, applied uniformly")
    _rule_end = _own_source.find("# STATED LIMIT.", _rule_start + 1) \
        if _rule_start != -1 else -1
    _rule = _own_source[_rule_start:_rule_end] if _rule_end != -1 else ""
    check("the rule block was located", bool(_rule), True)
    check("the rule block names the redacted marker",
          REDACTED_PREFIX in _rule, True)
    check("the rule block names the output marker",
          OUTPUT_PREFIX in _rule, True)
    check("the rule block does not describe a stale marker",
          "<redacted>" in _rule or "<output>" in _rule, False)

    # 8. The markers must be writable into XML without escaping. `<redacted>`
    #    was the first choice and is illegal in XML character data, so a
    #    per-file JUnit XML carrying it would round-trip as `&lt;redacted&gt;`.
    check("redacted marker is XML-safe",
          "<" in REDACTED_PREFIX or ">" in REDACTED_PREFIX, False)
    check("output marker is XML-safe",
          "<" in OUTPUT_PREFIX or ">" in OUTPUT_PREFIX, False)

    # 9. scrub_junit_file end-to-end on a synthetic JUnit document: the paths
    #    go, the structure and the surrounding prose stay, and it stays
    #    parseable. This is the point the first pass of the fix missed
    #    entirely — the derived copy was cleaned and the original was not.
    import tempfile as _tempfile
    _tmp = Path(_tempfile.mkdtemp(prefix="uisuite_selftest_"))
    try:
        _xml = _tmp / "probe.xml"
        _xml.write_text(
            '<?xml version="1.0" encoding="utf-8"?>\n'
            '<testsuites><testsuite name="s" tests="1" failures="1">'
            f'<testcase classname="c" name="t" file="{repo}\\tests\\a.py">'
            f'<failure message="boom at {repo}\\tests\\a.py:3">'
            f'trace through C:\\Users\\somedev\\AppData\\x.pdf here'
            '</failure></testcase></testsuite></testsuites>',
            encoding="utf-8")
        changed = scrub_junit_file(_xml, repo, _tmp, )
        body = _xml.read_text(encoding="utf-8")
        check("junit scrub reports a change", changed, True)
        check("junit scrub removes the account name", "somedev" in body, False)
        check("junit scrub removes drive letters",
              bool(_ABS_PATH_RE.search(body)), False)
        check("junit scrub keeps the prose", "boom at" in body, True)
        check("junit scrub keeps the structure",
              ET.parse(str(_xml)).getroot().find(".//failure") is not None,
              True)
        check("junit scrub is idempotent",
              scrub_junit_file(_xml, repo, _tmp), False)
    finally:
        import shutil as _shutil
        _shutil.rmtree(_tmp, ignore_errors=True)

    # --- shard partition contract (phase-77, criterion 1) -------------------
    # These pin the properties the sharding claim rests on. Without them
    # "every file runs in exactly one shard" is a comment, and a partition bug
    # would present as tests silently not running -- which is SKIP=FAIL by
    # accident, and invisible because the sweep would still report green.
    _sample = [Path("tests") / n for n in (
        "test_ui_compliance_accessibility.py",   # 2700s override
        "test_ui_annotations.py",                # 2400s override
        "test_ui_journey_soak.py",               # 2700s override
        "test_ui_text_editing.py",               # 1800s override
        "test_ui_forms.py",                      # 1800s override
        "test_ui_alpha.py", "test_ui_beta.py", "test_ui_gamma.py",
        "test_ui_delta.py", "test_ui_epsilon.py", "test_ui_zeta.py",
    )]
    for _n in (2, 3, 4):
        _union: list[Path] = []
        for _s in range(1, _n + 1):
            _union.extend(partition_files_for_shard(_sample, _s, _n))
        # A partition: no file lost, no file run twice.
        check(f"shard {_n}: covers every file", sorted(map(str, _union)),
              sorted(map(str, _sample)))
        check(f"shard {_n}: no duplicates", len(_union), len(_sample))
        # Independent of INPUT ORDER. Comparing a call to itself would only
        # prove the function is deterministic within one process, which even an
        # enumeration-order-dependent implementation satisfies -- and the real
        # input comes from globs, whose order is not guaranteed. So shuffle the
        # input and require the same partition back.
        _shuffled = list(reversed(_sample[:5])) + list(_sample[5:][::-1])
        for _s in range(1, _n + 1):
            check(f"shard {_n}/{_s}: input order does not matter",
                  sorted(str(p) for p in
                         partition_files_for_shard(_shuffled, _s, _n)),
                  sorted(str(p) for p in
                         partition_files_for_shard(_sample, _s, _n)))
        # Cost-weighted: the three 2700/2400s files must not all land together,
        # which is exactly what a naive round-robin would do here.
        _heavy = {"test_ui_compliance_accessibility.py",
                  "test_ui_journey_soak.py", "test_ui_annotations.py"}
        _worst = max(
            len([p for p in partition_files_for_shard(_sample, _s, _n)
                 if p.name in _heavy])
            for _s in range(1, _n + 1)
        )
        check(f"shard {_n}: heavy files spread", _worst <= 2, True)
    # shards == 1 is the identity, so the default path is untouched.
    check("shard 1/1 is identity", partition_files_for_shard(_sample, 1, 1),
          _sample)
    # Out-of-range shard indices must raise, not silently run everything.
    for _bad in ((0, 3), (4, 3), (1, 0)):
        try:
            partition_files_for_shard(_sample, _bad[0], _bad[1])
            check(f"shard {_bad} rejected", "no raise", "ValueError")
        except ValueError:
            check(f"shard {_bad} rejected", "ValueError", "ValueError")

    if failures:
        print("[ui-suite] self-test FAILED:", file=sys.stderr)
        for f in failures:
            print("  - " + f, file=sys.stderr)
        return -1
    print(f"[ui-suite] self-test: {checks} checks passed")
    return checks


def main() -> int:
    # Handled before argparse's required arguments so the hygiene guard can
    # invoke the producer without inventing a repo root and an output dir.
    if "--self-test" in sys.argv[1:]:
        return 0 if self_test() > 0 else 1

    ap = argparse.ArgumentParser()
    ap.add_argument("--self-test", action="store_true",
                    help="Verify the F48 path-portability contract and exit; "
                         "emits '[ui-suite] self-test: N checks passed'.")
    ap.add_argument("--repo-root", required=True, type=Path)
    ap.add_argument("--output-dir", required=True, type=Path)
    ap.add_argument("--pdfe-exe", required=True)
    ap.add_argument("--file-timeout", type=int, default=DEFAULT_FILE_TIMEOUT_S)
    # Transient-UIA-flake mitigation. A large pywinauto suite on a headless
    # self-hosted Server-2019 runner trips a DIFFERENT ~5-7 marginal tests on
    # every ~5h full run — every one of which passes in isolation (verified
    # repeatedly: file_dialog_flows 9/0, dialog_reachability 142/0, etc.). The
    # flakiness is UIA contention under load, not product defects. When a file
    # reports failures/timeout, re-run that whole file up to --reruns extra
    # times; a transient flake self-heals on a fresh launch, while a GENUINE
    # failure fails every attempt. This is rerun-on-failure (standard practice
    # for flaky end-to-end UI suites), NOT skipping — SKIP=FAIL is preserved.
    ap.add_argument("--reruns", type=int, default=2,
                    help="Re-run a file up to N extra times if it has "
                         "failures/timeout (transient-UIA-flake mitigation).")
    # --- sharding (phase-77 / follow-up 77, criterion 1) --------------------
    # WHY THIS EXISTS. The full sweep is ~126 files at a measured ~9.23 min/file
    # (609 min completed 66 files on run 31359621883) = ~1163 min. A GitHub job
    # is capped at 720 min, of which ~100 goes on the cold vcpkg build, leaving
    # ~600. The sweep therefore CANNOT fit in one job and is killed at the cap,
    # reported `cancelled` -- a status that alerts nobody.
    #
    # The cap is PER JOB, not per workflow, so splitting the sweep across
    # several dispatches fixes it WITHOUT needing a second machine:
    #     ceil(1163 / 600) = 2 minimum; 3 gives headroom
    #     3 shards -> ~100 min build + ~388 min UI = ~488 min per run, of 720.
    # Three runs on ONE runner is fine -- they serialise, and CI here is
    # manual-only by standing decision anyway. A second runner makes them
    # parallel; nothing about this flag has to change for that.
    #
    # NOT a skip: every file runs, in exactly one shard. The partition is
    # deterministic (sorted input) and cost-weighted rather than round-robin,
    # because file runtimes differ by 20x -- three known files carry 30-45 min
    # caps and round-robin can land them together, putting one shard back over
    # the budget this flag exists to respect.
    ap.add_argument("--shard", type=int, default=1,
                    help="1-based shard index to run (see --shards).")
    ap.add_argument("--shards", type=int, default=1,
                    help="Total shard count. The file list is partitioned "
                         "deterministically and cost-weighted; every file runs "
                         "in exactly one shard. Default 1 = the whole sweep.")
    ap.add_argument("--only", default="",
                    help="Comma-separated substrings; when set, run ONLY UI "
                         "test files whose path contains one of them (targeted "
                         "subset validation, e.g. after a harness fix). Empty "
                         "= run the full suite. NB: the matrix-health gate is "
                         "meaningless on a subset, so the CI workflow skips it "
                         "when --only is used.")
    ap.add_argument("--merge-summaries", default="",
                    help="Directory of collected per-shard "
                         "ui-suite-summary.shard*.json files. Merges them into "
                         "<output-dir>/ui-suite-summary-merged.json and exits "
                         "without running anything (GitHub #278). Prints "
                         "shards_missing so a partial merge cannot read as a "
                         "sweep.")
    args = ap.parse_args()

    # #278. Merge mode runs nothing: it is the step that turns three shard
    # summaries into one honest run-wide number, and it is deliberately a
    # separate invocation so a human can run it over downloaded artifacts.
    if args.merge_summaries:
        src = Path(args.merge_summaries).resolve()
        shard_files = sorted(src.glob("ui-suite-summary.shard*.json"))
        if not shard_files:
            print("[ui-suite] no ui-suite-summary.shard*.json under %s" % src,
                  file=sys.stderr)
            return 2
        loaded = []
        for p in shard_files:
            with open(p, encoding="utf-8") as fh:
                loaded.append(json.load(fh))
        merged = merge_shard_summaries(loaded)
        out_dir = args.output_dir.resolve()
        out_dir.mkdir(parents=True, exist_ok=True)
        dest = out_dir / "ui-suite-summary-merged.json"
        with open(dest, "w", encoding="utf-8") as fh:
            json.dump(merged, fh, indent=2)

        # ...AND OVER THE UNSUFFIXED NAME. GitHub #278 is about
        # `ui-suite-summary.json` specifically: every shard writes it, so after
        # a sharded run it holds only the LAST shard's numbers, and that is the
        # file readers and uploaded artifacts actually reach for. Writing the
        # aggregate under a new name alone leaves the named artifact still
        # wrong -- which is what the p97 phase-5 validator gate failed this
        # phase for, correctly.
        #
        # SAFE BECAUSE OF WHEN THIS RUNS. The in-shard gate inside ui-e2e reads
        # the unsuffixed file moments after its own shard writes it, and the
        # merge runs only once every shard has finished, so that read has
        # already happened. Nothing that legitimately wants per-shard numbers
        # is reading this file by the time it is replaced -- and the
        # shard-qualified `ui-suite-summary.shard<N>of<M>.json` files remain
        # untouched for anyone who does.
        #
        # A PARTIAL merge still writes it. That is deliberate: `is_partial` and
        # `shards_missing` travel INSIDE the document, so a reader cannot
        # mistake it for a sweep, whereas leaving a stale last-shard summary in
        # place would look complete and be wrong. The command still exits
        # non-zero below.
        unsuffixed = out_dir / "ui-suite-summary.json"
        with open(unsuffixed, "w", encoding="utf-8") as fh:
            json.dump(merged, fh, indent=2)

        print("[ui-suite] merged %d shard summary file(s) -> %s"
              % (len(loaded), portable_path(dest, args.repo_root.resolve(), out_dir)))
        print("[ui-suite] the run-wide aggregate also replaces %s, which "
              "otherwise holds only the LAST shard (GitHub #278)"
              % portable_path(unsuffixed, args.repo_root.resolve(), out_dir))
        print("[ui-suite] shards merged: %s of %s"
              % (merged["shards_merged"], merged["shards_expected"]))
        print("[ui-suite] tests=%d failures=%d errors=%d skipped=%d files_run=%d"
              % (merged["totals"]["tests"], merged["totals"]["failures"],
                 merged["totals"]["errors"], merged["totals"]["skipped"],
                 merged["files_run"]))
        if merged["is_partial"]:
            print("[ui-suite] PARTIAL: shard(s) %s missing — this is NOT the "
                  "sweep." % (merged["shards_missing"],), file=sys.stderr)
            return 1
        return 0

    repo_root = args.repo_root.resolve()
    out_dir = args.output_dir.resolve()
    per_file_dir = out_dir / "per-file"
    per_file_dir.mkdir(parents=True, exist_ok=True)

    files = collect_ui_test_files(repo_root)
    print(f"[ui-suite] discovered {len(files)} UI test files")
    only_terms = [t.strip() for t in (args.only or "").split(",") if t.strip()]
    if only_terms:
        files = [f for f in files
                 if any(term in str(f).replace("\\", "/") for term in only_terms)]
        print(f"[ui-suite] --only {only_terms!r} -> {len(files)} file(s): "
              f"{[f.name for f in files]}")
    # Shard AFTER --only, so the two compose: a targeted subset can itself be
    # split if it is large. Before the emptiness check, because an empty shard
    # is a real configuration error worth reporting rather than a silent no-op.
    if args.shards > 1:
        total = len(files)
        files = partition_files_for_shard(files, args.shard, args.shards)
        print(f"[ui-suite] shard {args.shard}/{args.shards} -> "
              f"{len(files)} of {total} file(s)")
        if not files:
            print(f"[ui-suite] shard {args.shard}/{args.shards} is EMPTY — "
                  f"more shards than files ({total}); reduce --shards",
                  file=sys.stderr)
            return 1

    if not files:
        print("[ui-suite] no UI test files found — exiting", file=sys.stderr)
        return 0

    # Snapshot the pre-existing per-run matrix CSVs so we can identify only the
    # ones THIS sweep produces and aggregate them (Phase 10 matrix-health input).
    runtime_matrix_dir = repo_root / "tests" / "_runtime"
    pre_existing_matrix = set(
        runtime_matrix_dir.glob("ui_test_matrix_*.csv")
    ) if runtime_matrix_dir.is_dir() else set()

    results = []
    junits: list[Path] = []
    suite_start = time.monotonic()
    for i, f in enumerate(files, 1):
        rel = f.relative_to(repo_root)
        # Replace separators so the output filename is flat.
        slug = str(rel).replace("\\", "__").replace("/", "__").replace(".py", "")
        junit_path = per_file_dir / f"{slug}.xml"
        log_path = per_file_dir / f"{slug}.log"
        eff_timeout = FILE_TIMEOUT_OVERRIDES_S.get(f.name, args.file_timeout)
        extra = f" (override {eff_timeout}s)" if eff_timeout != args.file_timeout else ""
        print(f"[ui-suite] ({i}/{len(files)}) {rel}{extra}", flush=True)
        def _has_problem(r: dict) -> bool:
            return bool(r["timed_out"]) or (
                r["returncode"] != 0 and r["returncode"] is not None)

        dumps_dir = per_file_dir / f"{slug}.dumps"
        result = run_one(
            f, junit_path, log_path,
            eff_timeout, repo_root, args.pdfe_exe,
            crash_dump_dir=dumps_dir / "attempt1",
        )
        marker = "TIMEOUT" if result["timed_out"] else f"rc={result['returncode']}"
        first_marker = marker
        app_crashed = bool(result.get("app_crashed"))
        if app_crashed:
            print(f"[ui-suite]   pdfe.exe FAULTED (minidump written): "
                  f"{result.get('app_crash_dumps')}", flush=True)
        print(f"[ui-suite]   {marker} in {result['elapsed_s']}s", flush=True)

        # Transient-UIA-flake mitigation: re-run a problem file so a flake that
        # passed in isolation self-heals on a fresh launch.
        #
        # GitHub #265 changed two things about this loop.
        #
        # 1. It used to re-run into the SAME junit/log, overwriting. The passing
        #    retry then replaced the failing artifact, so afterwards nothing
        #    recorded that anything had failed -- not the reason, not the fact.
        #    Each attempt now writes its own pair and the final one is copied to
        #    the canonical name the merge reads, so the evidence survives and
        #    the merge input is unchanged.
        #
        # 2. A run in which pdfe.exe FAULTED is not healable, whatever a later
        #    attempt does. This is #264's rule on the UI surface: re-running in
        #    isolation and treating a pass as proof of flakiness is sound for
        #    order-independent flakiness and inverted for a crash, and here the
        #    thing that died is a separate process whose death reaches this
        #    runner only as a failing or timed-out file.
        attempt = 0
        while _has_problem(result) and attempt < args.reruns:
            attempt += 1
            # Preserve the attempt we are about to supersede.
            for src, suffix in ((junit_path, "xml"), (log_path, "log")):
                if src.exists():
                    keep = per_file_dir / f"{slug}.attempt{attempt}.{suffix}"
                    shutil.copy2(src, keep)
            # run_one() already hard-kills leaked pdfe.exe on exit, so the
            # retry launches into a clean process state.
            print(f"[ui-suite]   retry {attempt}/{args.reruns} for {rel} "
                  f"(prev {marker})", flush=True)
            result = run_one(
                f, junit_path, log_path,
                eff_timeout, repo_root, args.pdfe_exe,
                crash_dump_dir=dumps_dir / f"attempt{attempt + 1}",
            )
            marker = ("TIMEOUT" if result["timed_out"]
                      else f"rc={result['returncode']}")
            if result.get("app_crashed"):
                app_crashed = True
                print(f"[ui-suite]   pdfe.exe FAULTED on retry {attempt}: "
                      f"{result.get('app_crash_dumps')}", flush=True)
            print(f"[ui-suite]   retry {attempt} {marker} in "
                  f"{result['elapsed_s']}s", flush=True)
            result["reruns_used"] = attempt

        if attempt:
            result["first_attempt_outcome"] = first_marker
            if not _has_problem(result):
                result["healed_on_retry"] = True

        # An application fault anywhere in the sequence keeps the file a
        # problem even if the last attempt passed (#265).
        if app_crashed:
            result["app_crashed"] = True
            result["unhealable_app_crash"] = True
            if result.get("healed_on_retry"):
                print(f"[ui-suite]   {rel}: PASSED on retry but pdfe.exe "
                      f"FAULTED during this file. NOT healed -- a crash that "
                      f"passes on a fresh launch is the signature of an "
                      f"order-dependent fault, not evidence of a flake "
                      f"(GitHub #265/#264). Dumps: {dumps_dir}", flush=True)

        results.append(result)
        junits.append(junit_path)

    suite_elapsed = time.monotonic() - suite_start

    merged_path = out_dir / "ui-junit-merged.xml"
    totals = merge_junits(junits, merged_path, results)
    failures = collect_failures(junits, repo_root, out_dir)

    # Files where pytest exited non-zero or hit the outer timeout
    # represent hidden test failures: pytest typically crashed mid-
    # test (e.g. pytest-timeout's hard-kill on a hung UIA query)
    # before writing complete junit output, so per-test failures
    # don't appear in `failures` but the file as a whole did NOT pass.
    # Phase 3 close validation (test-runner gate) flagged this gap;
    # surface it explicitly so future runs can't claim "0 failures"
    # while files actually crashed.
    files_with_problems = [r for r in results if file_has_problem(r)]
    summary = {
        "files_run": len(results),
        "wall_clock_s": round(suite_elapsed, 1),
        "totals": totals,
        "failure_count": len(failures),
        "per_file": results,
        "failures": failures,
        "files_with_problems_count": len(files_with_problems),
        # GitHub #301: a crash is its own class of problem, not a flavour of
        # "rc!=0 or timeout" -- a crashed app can leave rc==0 and every test
        # reporting PASS, which is exactly how this one hid.
        "app_crash_count": len([r for r in results
                                if r.get("unhealable_app_crash")
                                or r.get("app_crashed")]),
        "files_with_problems": [
            {
                "file": p["file"],
                "rc": p["returncode"],
                "timed_out": p["timed_out"],
                "elapsed_s": p["elapsed_s"],
            }
            for p in files_with_problems
        ],
    }
    # Aggregate the per-file matrix CSVs this sweep produced into one
    # run-wide matrix (Phase 10). check_matrix_health.py consumes this single
    # aggregate; the individual per-file CSVs each only carry their own file's
    # recorded rows overlaid on the committed closeout matrix.
    post_matrix = set(runtime_matrix_dir.glob("ui_test_matrix_*.csv")) \
        if runtime_matrix_dir.is_dir() else set()
    new_matrix_csvs = sorted(post_matrix - pre_existing_matrix)
    committed_matrix = (repo_root / "tests" / "ui_automation" / "matrix"
                        / "ui_test_matrix.csv")
    matrix_aggregate = None
    if new_matrix_csvs and committed_matrix.exists():
        try:
            matrix_aggregate = aggregate_matrix(
                committed_matrix, new_matrix_csvs,
                out_dir / "ui_test_matrix_aggregate.csv")
        except Exception as exc:  # noqa: BLE001
            print(f"[ui-suite] matrix aggregation failed: {exc}",
                  file=sys.stderr)
    if matrix_aggregate:
        # F48. aggregate_matrix() is left returning the real path because
        # tests/ui_automation/test_aggregation_precedence.py calls it directly
        # and works with the file it names; the portable form is applied here,
        # at the boundary where the value enters an artifact.
        matrix_aggregate = dict(matrix_aggregate)
        matrix_aggregate["aggregate_csv"] = portable_path(
            matrix_aggregate["aggregate_csv"], repo_root, out_dir)
    summary["matrix_aggregate"] = matrix_aggregate

    # GitHub #278 -- SAY WHICH SHARD THIS IS, AND DO NOT COLLIDE.
    #
    # Every shard used to write `ui-suite-summary.json` and nothing else, so the
    # file could not say whether it described the sweep or one third of it. In CI
    # the three shards are separate matrix jobs with their own workspaces, so
    # nothing is overwritten *on disk* -- the collision happens when the three
    # identically-named artifacts are collected, and then one shard's numbers
    # read as the whole run. #277 records the consequence: "2,500 tests, 16
    # failures" reported twice before noticing a later shard had replaced the
    # file with "560 tests, 14 failures".
    #
    # Two changes, both small:
    #   * the summary now CARRIES its shard identity, so a reader can tell;
    #   * a sharded run ALSO writes a shard-qualified name, which cannot collide
    #     when artifacts are gathered into one directory.
    # The unqualified name is still written, because single-shard runs and the
    # existing CI step both read it and a sharded run's unqualified file is still
    # a truthful description of that shard.
    summary["shard"] = args.shard
    summary["shards"] = args.shards
    summary["is_partial"] = args.shards > 1

    summary_path = out_dir / "ui-suite-summary.json"
    with open(summary_path, "w", encoding="utf-8") as f:
        json.dump(summary, f, indent=2)
    if args.shards > 1:
        shard_path = out_dir / ("ui-suite-summary.shard%dof%d.json"
                                % (args.shard, args.shards))
        with open(shard_path, "w", encoding="utf-8") as f:
            json.dump(summary, f, indent=2)

    print()
    print(f"[ui-suite] files run:  {summary['files_run']}")
    print(f"[ui-suite] wall clock: {summary['wall_clock_s']}s "
          f"({summary['wall_clock_s']/60:.1f} min)")
    print(f"[ui-suite] tests:      {totals['tests']}")
    print(f"[ui-suite] failures:   {totals['failures']}")
    print(f"[ui-suite] errors:     {totals['errors']}")
    print(f"[ui-suite] skipped:    {totals['skipped']}")
    print(f"[ui-suite] failing:    {summary['failure_count']}")
    print(f"[ui-suite] hidden problems "
          f"(rc!=0, timeout or crash): "
          f"{summary['files_with_problems_count']}")
    # GitHub #301. Its own line, because this one was invisible: the crash was
    # inside "hidden problems: 17" and the label said only "rc!=0 or timeout",
    # so a reader had no reason to look for a fault there. The count is also
    # now part of `totals['errors']` above via the merged artifact.
    print(f"[ui-suite] app crashes: {summary['app_crash_count']}"
          + ("  <-- pdfe.exe FAULTED; the merged junit is RED for this"
             if summary['app_crash_count'] else ""))
    print()
    # Portable here too: this console output is Tee-Object'd to a log that
    # windows-validation.yml uploads as an artifact, so it is an artifact.
    print(f"[ui-suite] merged junit:  "
          f"{portable_path(merged_path, repo_root, out_dir)}")
    print(f"[ui-suite] summary json:  "
          f"{portable_path(summary_path, repo_root, out_dir)}")
    if matrix_aggregate:
        print(f"[ui-suite] matrix aggregate: {matrix_aggregate['aggregate_csv']} "
              f"(green={matrix_aggregate['green']}, "
              f"changed={matrix_aggregate['rows_changed_from_committed']})")

    return 0  # always succeed — failure detail lives in the artifacts


if __name__ == "__main__":
    sys.exit(main())
