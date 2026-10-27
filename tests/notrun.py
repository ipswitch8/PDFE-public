# -*- coding: utf-8 -*-
"""NOT-RUN reporting: separating "ran and asserted" from "skipped clean".

Gaps A1-64 (UI-SKIP-01), A1-66 (UI-SKIP-03) and A1-83 (advisory/self-skipping
checks read as coverage).

The problem
===========

pytest reports a skipped test with a dot-class ``s`` and a green session. A
suite that skips 100% of its assertions therefore looks exactly like a suite
that ran and passed them — the reader sees "0 failed" and moves on. Two whole
files in this repo (``test_ui_encryption_flows_wad.py``,
``test_ui_file_dialog_flows_wad.py``) plus the visual-baseline module have
skipped every run for months while contributing to a green board.

The rule
========

An environment-gated test that skips has **NOT RUN**. It is neither a pass nor
a failure — it is an absence of evidence, and the report must say so in a way
that is impossible to misread. This module provides:

* :func:`skip_not_run` — a ``pytest.skip`` wrapper that stamps the reason with
  the :data:`NOT_RUN_PREFIX` marker and a machine-readable cause tag.
* a pytest plugin (wired from ``tests/conftest.py``) that collects every such
  skip, prints a loud terminal section, and writes a JSON artifact CI can
  assert on — so "ran and asserted" and "skipped clean" are distinguishable
  downstream, not just visually.

Nothing here makes a skipped test pass, and nothing here suppresses a skip.
The only thing it changes is that the absence becomes visible.
"""
from __future__ import annotations

import json
import os
from pathlib import Path
from typing import Any, Dict, List, Optional

#: Every NOT-RUN skip reason starts with this. Grep-able in CI logs.
NOT_RUN_PREFIX = "NOT-RUN"

#: Where the machine-readable report is written.
REPORT_PATH = Path(__file__).resolve().parent / "_runtime" / "not_run_report.json"

#: Environment variable that turns any NOT-RUN skip into a hard failure. Set it
#: in the lane that is SUPPOSED to have the prerequisites (e.g. the WinAppDriver
#: lane) so a silently-degraded runner cannot report green.
REQUIRE_ENV_VAR = "PDFE_REQUIRE_RUNNABLE"


def format_reason(cause: str, detail: str,
                  remedy: Optional[str] = None) -> str:
    """Build a NOT-RUN skip reason string.

    *cause* is a short machine-readable tag (``winappdriver-service-down``,
    ``no-live-ui``, ``unsupported-dpi``, ...). *detail* explains the observed
    state. *remedy* — if the situation is fixable — says exactly how.
    """
    text = f"{NOT_RUN_PREFIX} [{cause}]: {detail}"
    if remedy:
        text += f" — REMEDY: {remedy}"
    return text


def skip_not_run(cause: str, detail: str,
                 remedy: Optional[str] = None) -> None:
    """Skip the current test, stamped as NOT-RUN (never as a pass).

    Raises ``pytest.fail`` instead of skipping when
    ``PDFE_REQUIRE_RUNNABLE=1``: in a lane that is meant to have the
    prerequisites, an unmet prerequisite is a broken lane, not an excuse.
    """
    import pytest
    reason = format_reason(cause, detail, remedy)
    if os.environ.get(REQUIRE_ENV_VAR, "").strip().lower() in (
            "1", "true", "yes", "on"):
        pytest.fail(
            f"{reason}\n({REQUIRE_ENV_VAR} is set: this lane is required to "
            f"be able to RUN this test, so an unmet prerequisite is a "
            f"failure, not a skip.)")
    pytest.skip(reason)


def is_not_run(reason: str) -> bool:
    return bool(reason) and reason.lstrip().startswith(NOT_RUN_PREFIX)


def parse_cause(reason: str) -> str:
    """Extract the ``[cause]`` tag from a NOT-RUN reason, or "unknown"."""
    if "[" in reason and "]" in reason:
        return reason[reason.index("[") + 1:reason.index("]")]
    return "unknown"


def summarise(entries: List[Dict[str, Any]]) -> Dict[str, Any]:
    """Aggregate collected NOT-RUN entries into the report payload.

    Pure — unit-tested directly in ``tests/test_notrun_reporting.py``.
    """
    by_cause: Dict[str, int] = {}
    by_file: Dict[str, int] = {}
    for e in entries:
        by_cause[e["cause"]] = by_cause.get(e["cause"], 0) + 1
        by_file[e["file"]] = by_file.get(e["file"], 0) + 1
    return {
        "not_run_total": len(entries),
        "by_cause": dict(sorted(by_cause.items())),
        "by_file": dict(sorted(by_file.items())),
        "entries": entries,
    }


# ---------------------------------------------------------------------------
# pytest plugin hooks — imported and re-exported by tests/conftest.py
# ---------------------------------------------------------------------------

_COLLECTED: List[Dict[str, Any]] = []


def record_report(report) -> None:
    """``pytest_runtest_logreport`` body: capture NOT-RUN skips."""
    if getattr(report, "when", None) not in ("setup", "call"):
        return
    if not getattr(report, "skipped", False):
        return
    longrepr = getattr(report, "longrepr", None)
    reason = ""
    if isinstance(longrepr, tuple) and len(longrepr) == 3:
        reason = str(longrepr[2])
    elif longrepr is not None:
        reason = str(longrepr)
    if reason.startswith("Skipped: "):
        reason = reason[len("Skipped: "):]
    if not is_not_run(reason):
        return
    nodeid = getattr(report, "nodeid", "")
    _COLLECTED.append({
        "nodeid": nodeid,
        "file": nodeid.split("::", 1)[0] if "::" in nodeid else nodeid,
        "cause": parse_cause(reason),
        "reason": reason,
    })


def terminal_summary(terminalreporter) -> None:
    """``pytest_terminal_summary`` body: loud section + JSON artifact."""
    payload = summarise(list(_COLLECTED))
    try:
        REPORT_PATH.parent.mkdir(parents=True, exist_ok=True)
        REPORT_PATH.write_text(
            json.dumps(payload, indent=2, ensure_ascii=False),
            encoding="utf-8")
    except OSError:
        pass
    if not _COLLECTED:
        return
    w = terminalreporter.write_line
    terminalreporter.write_sep("=", "NOT RUN (no evidence produced)")
    w(f"{payload['not_run_total']} test(s) were SKIPPED for missing "
      f"prerequisites. These are NOT passes — nothing was asserted.")
    for cause, n in payload["by_cause"].items():
        w(f"  {cause:34s} {n:4d}")
    w("")
    for path, n in payload["by_file"].items():
        w(f"  {n:4d}  {path}")
    w("")
    w(f"Machine-readable report: {REPORT_PATH}")
    w(f"Set {REQUIRE_ENV_VAR}=1 to make these hard failures in a lane that "
      f"is expected to be able to run them.")
