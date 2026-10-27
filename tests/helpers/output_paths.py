# -*- coding: utf-8 -*-
"""Test output path helpers — every E2E test that writes to disk MUST
use these so outputs are unique-per-run, timestamp-tagged, and
auto-cleanable.

Naming convention: <base>_<run_id>_<ts_us>.<ext>
  run_id : 8-hex digits derived from time.time_ns() at session start
           (one value per pytest session, exported via env var
           PDFE_TEST_RUN_ID for cross-process consistency).
  ts_us  : ISO-8601-ish timestamp to microsecond precision,
           e.g. 20260510T231245p123456 (no colons — Windows-safe).

Example: out_a3f7c1e9_20260510T231245p123456.pdf

Why this matters
----------------
The PDFE E2E suite drives native Save / SaveAs / Optimize / Encrypt /
Batch / Sign dialogs.  When a test accepts the suggested filename, the
output is written next to the source fixture and overwrites it.  Using a
unique per-call output path eliminates the failure mode entirely: every
``set_save_dialog_filename(..., gen_out(tmp_path, ...))`` call writes to
a path that has never existed before, so no overwrite prompt can fire
and no fixture can be polluted.

Pairs with:
  * ``tests/conftest.py`` ``_fixture_integrity_guard`` — fails the
    session if any tracked fixture changed sha256 / mtime.
  * ``tests/conftest.py`` ``_janitor`` — sweeps stale outputs from
    previous runs (>24h) at session start so disk doesn't fill.
  * ``tests/helpers/file_dialog.py`` overwrite-prompt watchdog — if
    pytest's per-run naming is ever broken and a save would overwrite,
    we screenshot + click No + raise ``OverwritePromptError``.
"""

from __future__ import annotations

import datetime as _dt
import os
import re
import secrets
import time
from pathlib import Path
from typing import Optional

# ---------------------------------------------------------------------------
# Run ID — one value per pytest session, propagated via env var so
# subprocesses (PDFE.exe etc) and pytest-xdist workers share it.
# ---------------------------------------------------------------------------

_RUN_ID_ENV = "PDFE_TEST_RUN_ID"


def _derive_run_id() -> str:
    """Return the existing run id from env, or mint a fresh one.

    Uses ``secrets.token_hex(4)`` (8 hex chars) so collisions across
    parallel sessions are effectively impossible without leaking the
    full nanosecond timestamp into output filenames.  The value is
    exported into ``os.environ`` so child processes pick it up.
    """
    existing = os.environ.get(_RUN_ID_ENV, "").strip().lower()
    if existing and re.fullmatch(r"[0-9a-f]{8}", existing):
        return existing
    minted = secrets.token_hex(4)
    os.environ[_RUN_ID_ENV] = minted
    return minted


RUN_ID: str = _derive_run_id()


# ---------------------------------------------------------------------------
# Visual failure artifacts — RUN-SCOPED (GitHub #289)
# ---------------------------------------------------------------------------

def visual_failure_dir(create: bool = True) -> Path:
    """The directory this run writes visual-regression artifacts into.

    GitHub #289. Every visual failure used to land in
    ``tests/visual/_failure_artifacts/<baseline_name>_diff.png`` — a path
    derived ONLY from the baseline name, so a second run of the same test
    overwrote the first run's evidence in place. The directory is gitignored, so
    nothing was versioned either.

    That is not hypothetical. It bit during p96 phase-2: a diff was measured,
    its numbers quoted on an issue, and a later unrelated run rewrote the same
    filenames. The tell was two scripts reporting 2773 and 3456 differing pixels
    for nominally the same file — same path, different file, no indication
    anything had changed.

    The codebase already knew. ``tests/conftest.py``'s janitor carries the
    comment "Visual _failure_artifacts have non-per-run names so we apply a
    second rule there", and capped them by mtime instead — which bounds growth
    but does not stop one run destroying another's evidence.

    So artifacts are scoped by ``RUN_ID``, the same value the janitor and the
    integrity snapshot already print, minted once per session and exported via
    ``PDFE_TEST_RUN_ID`` so the app's own child processes agree on it. Two runs
    produce two directories and both remain retrievable.

    The cap moves with it: ``conftest`` now prunes whole run DIRECTORIES by
    mtime rather than loose files, because a per-file cap over run-scoped
    subdirectories would never match anything and the artifacts would grow
    without bound.
    """
    d = _TESTS_DIR_FOR_ARTIFACTS / "visual" / "_failure_artifacts" / RUN_ID
    if create:
        d.mkdir(parents=True, exist_ok=True)
    return d


#: Resolved once. This module lives at tests/helpers/output_paths.py, so the
#: tests directory is TWO levels up -- asserted rather than assumed, because a
#: wrong root here would scatter artifacts somewhere nobody looks for them.
_TESTS_DIR_FOR_ARTIFACTS: Path = Path(__file__).resolve().parents[1]
assert (_TESTS_DIR_FOR_ARTIFACTS / "visual").is_dir(), (
    f"tests dir misresolved to {_TESTS_DIR_FOR_ARTIFACTS!r}")


# ---------------------------------------------------------------------------
# Timestamp helpers
# ---------------------------------------------------------------------------

def _now_ts() -> str:
    """Return a Windows-safe microsecond timestamp.

    Format: YYYYMMDDThhmmsspuuuuuu (no colons, no slashes; ``p`` separates
    seconds from microseconds, since ``.`` would be read as an extension
    boundary on some filesystems).
    """
    now = _dt.datetime.now()
    # strftime cannot emit microseconds directly without a leading "."
    # so we compose it manually for filesystem safety.
    return now.strftime("%Y%m%dT%H%M%S") + "p" + f"{now.microsecond:06d}"


# Regex matching the timestamp slug we emit.  Used by match_any_run_pattern
# and tests that need to verify their own outputs.
TS_REGEX = re.compile(r"\d{8}T\d{6}p\d{6}")
RUN_ID_REGEX = re.compile(r"[0-9a-f]{8}")
COMBINED_SLUG_REGEX = re.compile(
    r"_(?P<run>[0-9a-f]{8})_(?P<ts>\d{8}T\d{6}p\d{6})",
)


# ---------------------------------------------------------------------------
# Public API
# ---------------------------------------------------------------------------

def gen_out(tmp_path: Path, base: str, ext: str = ".pdf") -> Path:
    """Return a unique output Path under ``tmp_path``.

    ``tmp_path``
        The pytest ``tmp_path`` fixture (per-test temp directory under
        ``tests/_tmp/`` once pytest.ini's --basetemp is in effect).
    ``base``
        Human-readable stem (e.g. ``"encrypted"`` or ``"signed_round_trip"``)
        — appears as the leading segment so the file is greppable.
    ``ext``
        File extension *including* the leading dot.  Defaults to ".pdf".

    Returns
    -------
    Path
        ``tmp_path / "<base>_<RUN_ID>_<ts>.<ext>"`` — guaranteed unique
        per call (microsecond-precision timestamp).

    Notes
    -----
    The returned Path's parent (``tmp_path``) is created by pytest; the
    file itself is not created — the caller is responsible for either
    writing to it or passing the string form to the application.
    """
    if not ext.startswith("."):
        ext = "." + ext
    # Strip path separators from base to prevent accidental subdirectory
    # creation outside tmp_path.
    safe_base = base.replace("/", "_").replace("\\", "_").strip("_") or "out"
    return Path(tmp_path) / f"{safe_base}_{RUN_ID}_{_now_ts()}{ext}"


def gen_out_name(base: str, ext: str = ".pdf") -> str:
    """Filename-only variant of ``gen_out`` for callers that need to
    compose paths themselves (e.g. when the target directory comes from
    another helper).  Same naming convention.
    """
    if not ext.startswith("."):
        ext = "." + ext
    safe_base = base.replace("/", "_").replace("\\", "_").strip("_") or "out"
    return f"{safe_base}_{RUN_ID}_{_now_ts()}{ext}"


def match_pattern() -> str:
    """Glob matching every output emitted by THIS session.

    Used by the integrity guard to permit per-run output files inside a
    clearly-marked tmp subdirectory of tests/fixtures (if a generator
    ever needs to land output there) and by the janitor to know which
    files are ours vs. someone else's stale junk.
    """
    return f"*_{RUN_ID}_*"


def match_any_run_pattern() -> str:
    """Glob matching outputs from any session that used this helper.

    The eight ``?`` wildcards match an 8-hex run id; the timestamp
    portion is matched by literal length-precision wildcards.  Use this
    for cross-session cleanup ("remove anything that looks like one of
    our outputs, older than N hours").
    """
    return "*_????????_????????T??????p??????.*"


def is_our_output(path: Path) -> bool:
    """Return True if ``path``'s filename matches the per-run naming
    convention.  Used by janitor + integrity guard.
    """
    return bool(COMBINED_SLUG_REGEX.search(path.name))


def parse_run_id(path: Path) -> Optional[str]:
    """Return the run id embedded in ``path``'s filename, or None."""
    m = COMBINED_SLUG_REGEX.search(path.name)
    return m.group("run") if m else None


def parse_timestamp(path: Path) -> Optional[_dt.datetime]:
    """Parse the embedded timestamp from ``path``.  Returns None if the
    filename does not match the convention or the timestamp is invalid.
    """
    m = COMBINED_SLUG_REGEX.search(path.name)
    if not m:
        return None
    raw = m.group("ts")
    try:
        head, micros = raw.split("p", 1)
        dt = _dt.datetime.strptime(head, "%Y%m%dT%H%M%S")
        return dt.replace(microsecond=int(micros))
    except (ValueError, IndexError):
        return None


def age_seconds(path: Path, now: Optional[float] = None) -> float:
    """Return ``path``'s age in seconds based on its embedded timestamp.

    Falls back to mtime if the filename does not embed a timestamp.
    Used by the janitor to identify stale outputs across runs.
    """
    if now is None:
        now = time.time()
    ts = parse_timestamp(path)
    if ts is not None:
        return now - ts.timestamp()
    try:
        return now - path.stat().st_mtime
    except OSError:
        return 0.0
