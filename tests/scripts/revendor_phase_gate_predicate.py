# -*- coding: utf-8 -*-
"""Re-vendor the `phase-gate.sh` predicate after the hook legitimately changes.

Run this, review the diff, and commit it in the same change that re-ports
`_gate_would_accept` in `tests/scripts/test_run_gtest_teardown_evidence.py`:

    python tests/scripts/revendor_phase_gate_predicate.py

WHY A SCRIPT RATHER THAN "copy it by hand". The vendored file is evidence that
the port matches a specific version of the hook. Hand-copying is where a
transcription slip becomes a test that agrees with itself -- and the whole
point of the vendored reference is to be a second, independent witness. This
extracts with the same anchors the drift test uses, so the two cannot disagree
about what "the hook's predicate" means.

It REFUSES rather than guessing if the anchors do not match, because a
re-vendor that silently writes the wrong region would replace a real control
with a copy of nothing.
"""
from __future__ import annotations

import io
import os
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
HOOK = Path(os.path.expanduser("~/.claude/hooks/phase-gate.sh"))
DEST = HERE / "fixtures" / "phase_gate_validate_junit_reference.py"

HEADER = '''# -*- coding: utf-8 -*-
# VENDORED, DO NOT EDIT BY HAND.
#
# A verbatim copy of the `validate_junit_xml` predicate embedded in
# `~/.claude/hooks/phase-gate.sh`. That hook is the real evidence gate for
# pipeline phases, and it lives OUTSIDE this repo, so CI has no access to it.
# This copy exists so the equivalence battery in
# `tests/scripts/test_phase_gate_port_has_not_drifted.py` can run EVERYWHERE,
# including on a GitHub-hosted runner, instead of being skipped wherever the
# hook is absent -- a drift control that only runs on one workstation is a
# drift control for one workstation.
#
# Invoked as a subprocess exactly as the hook invokes it:
#     python <this file> <xml-path> <target>
# exit 0 = accept, 1 = reject, 2 = unparseable.
#
# TO RE-VENDOR after the hook legitimately changes:
#     python tests/scripts/revendor_phase_gate_predicate.py
# `test_the_vendored_reference_matches_the_live_hook` FAILS until you do, which
# is the point: the staleness is reported, not absorbed.
'''


def extract(hook_text: str) -> str:
    """The same extraction the drift test performs. Raises on anchor rot."""
    start = hook_text.find("validate_junit_xml() {")
    if start < 0:
        raise SystemExit(
            "REFUSING: validate_junit_xml() not found in the hook. It has "
            "been restructured; update the anchors in both this script and "
            "tests/scripts/test_phase_gate_port_has_not_drifted.py.")
    m = re.search(r"<<'PY'[^\n]*\r?\n(.*?)\r?\nPY\r?\n", hook_text[start:],
                  re.DOTALL)
    if not m:
        raise SystemExit(
            "REFUSING: the <<'PY' heredoc could not be located inside "
            "validate_junit_xml(); anchors have changed.")
    return m.group(1)


def main() -> int:
    if not HOOK.exists():
        print("REFUSING: ~/.claude/hooks/phase-gate.sh does not exist on this "
              "machine, so there is nothing to vendor FROM. Run this where the "
              "pipeline hooks are installed.")
        return 2
    body = extract(HOOK.read_text(encoding="utf-8", errors="replace"))
    before = DEST.read_text(encoding="utf-8") if DEST.exists() else ""
    after = HEADER + body + "\n"
    if before == after:
        print("already current: %s" % DEST.name)
        return 0
    io.open(DEST, "w", encoding="utf-8", newline="\n").write(after)
    print("re-vendored %s (%d chars of predicate)" % (DEST.name, len(body)))
    print("Now re-port `_gate_would_accept` in "
          "tests/scripts/test_run_gtest_teardown_evidence.py to match, and "
          "commit both together.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
