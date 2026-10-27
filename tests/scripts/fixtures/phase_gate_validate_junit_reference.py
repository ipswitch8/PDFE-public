# -*- coding: utf-8 -*-
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
# -*- coding: utf-8 -*-
import sys, xml.etree.ElementTree as ET
xml_path, target = sys.argv[1], (sys.argv[2] if len(sys.argv) > 2 else "")
try:
    root = ET.parse(xml_path).getroot()
except Exception:
    sys.exit(2)
def ai(el, name):
    try: return int(el.get(name, "0") or "0")
    except Exception: return 0
tf = te = tt = 0
suites = list(root.iter("testsuite"))
for ts in suites:
    tf += ai(ts, "failures"); te += ai(ts, "errors"); tt += ai(ts, "tests")
cases = list(root.iter("testcase"))
if tt == 0:  # single-suite / testsuites-only attrs
    tf += ai(root, "failures"); te += ai(root, "errors"); tt += ai(root, "tests")
def is_run(tc):
    return tc.get("status", "run") == "run"
run_cases = [tc for tc in cases if is_run(tc)]
ok = (tf == 0 and te == 0 and tt > 0 and len(run_cases) > 0)
if target:
    matches = [tc for tc in cases
               if tc.get("name") == target
               or (tc.get("classname", "") + "." + tc.get("name", "")) == target]
    ok = ok and any(is_run(tc) for tc in matches)
sys.exit(0 if ok else 1)
