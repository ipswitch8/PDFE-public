#!/usr/bin/env bash
# Verify that pipeline p95 phase-10 did not weaken the three product oracles of
# tests/test_ui_stress.py::test_stress_1000_annotation_undo_redo.
#
# WHY THIS EXISTS AS A SCRIPT. criteria-status.md originally published a `diff`
# of two hardcoded LINE RANGES with "-> no output" next to it. Two problems, and
# the claim-check gate failed the phase for them:
#
#   1. The ranges were wrong on arrival -- the file had shifted under later
#      edits -- so running the published command produced output and a reviewer
#      would conclude the oracles HAD been weakened. That is the same
#      line-citation defect this pipeline was already failed for in phase-6,
#      where five of seven cited line numbers were displaced by the very edit
#      they described.
#   2. More fundamentally, hardcoded line numbers are a COPY of the claim, not a
#      check of it. A check that needs updating whenever the file moves is a
#      second thing to get wrong.
#
# So: anchor on the assertion TEXT, never on line numbers, and compare against
# the pre-phase commit. Uses grep -F (fixed strings) deliberately -- my first
# attempt at this used grep -E with unescaped parens, so `int(0.95 * N)` parsed
# as a REGEX GROUP, matched nothing, and the empty result reported a false
# "DIFFERS". A check whose own escaping can silently invert its answer is worse
# than no check.
#
# Usage: bash tools/gate/check_stress_oracles_unchanged.sh [base-commit]
# Exit 0 = all three oracles byte-identical to the base. Exit 1 = changed.

set -u

BASE="${1:-e2337902}"
F="tests/test_ui_stress.py"
# 14 lines. HEADROOM, not a fix -- and the distinction matters because the first
# version of this comment got it wrong.
#
# The claim-check gate raised a concern that a 5-line window could miss a
# weakening further DOWN an oracle body, and I widened the window and wrote that
# 5 "reached the end of the shortest body and not the longest". That was not
# true: the gate then inspected all three and found every body ends at anchor+4,
# so `grep -A 5` already covered each one in full. It retired its own concern and
# said so.
#
# 14 is kept anyway: it is strictly stronger, costs nothing, and leaves room for
# a body that grows. But the honest reason is slack against future edits, not a
# hole that was being closed. A guard justified by a reason that is false is one
# a later reader can act on wrongly, which is the defect class this whole phase
# kept tripping over.
#
# Verified able to fail at this width: mutating the create-floor MESSAGE line at
# anchor+3 exits 1 with the diff printed.
CONTEXT=14

# The three product oracles, by their assertion text.
ANCHORS=(
  'if n_create < int(0.95 * N):'
  'if n_undo != 0:'
  'if n_redo != n_create:'
)

rc=0
for anchor in "${ANCHORS[@]}"; do
  old=$(git show "${BASE}:${F}" | grep -F -A "$CONTEXT" -- "$anchor" | tr -d '\r')
  new=$(grep -F -A "$CONTEXT" -- "$anchor" "$F" | tr -d '\r')

  if [ -z "$old" ]; then
    echo "ERROR: anchor not found in ${BASE}: ${anchor}"
    echo "       (the base commit may be wrong, or the oracle was renamed --"
    echo "        either way this script cannot answer, so it fails rather than"
    echo "        reporting a green it did not establish)"
    rc=1
    continue
  fi
  if [ -z "$new" ]; then
    echo "REMOVED: ${anchor}"
    rc=1
    continue
  fi
  if [ "$old" = "$new" ]; then
    echo "UNCHANGED: ${anchor}"
  else
    echo "CHANGED:   ${anchor}"
    diff <(printf '%s\n' "$old") <(printf '%s\n' "$new") || true
    rc=1
  fi
done

if [ "$rc" -eq 0 ]; then
  echo
  echo "All three product oracles are byte-identical to ${BASE}."
  echo "Phase-10 raised the pytest timeout watchdog and two helper timeouts; it"
  echo "did not weaken an oracle. It also ADDED two: the arming-failure check and"
  echo "the undo/redo performed-count floors, both of which run BEFORE these so a"
  echo "harness drop fails as a harness drop rather than as lost product data."
fi
exit "$rc"
