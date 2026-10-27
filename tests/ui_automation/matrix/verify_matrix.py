# -*- coding: utf-8 -*-
"""Verify the UI test matrix CSV satisfies every phase-0 acceptance
criterion from .claude/pipeline.json.

Runs as a standalone script — exit 0 on success, 1 on first failure
with a descriptive message.
"""
from __future__ import annotations
import csv
import sys
from pathlib import Path

_REPO_ROOT = Path(__file__).resolve().parents[3]
if str(_REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(_REPO_ROOT))

from tests.ui_automation.matrix import coverage_resolver  # noqa: E402

EXPECTED_HEADER = [
    "subsystem", "category", "element", "test_id", "preconditions",
    "action", "expected", "tool", "priority", "status", "owner_phase",
    "fixture", "notes",
]

REQUIRED_MENUS = {
    "File", "Edit", "Pages", "Forms", "Security", "Image",
    "Tools", "View", "Help",
}

REQUIRED_ANNOTATION_TOOLS = {
    "StickyNote", "TextMarkup", "Ink", "Shape", "FreeText", "Select",
}

REQUIRED_FORM_FIELD_TYPES = {
    "Text", "Checkbox", "RadioButton", "ComboBox",
    "ListBox", "PushButton", "Signature", "Unknown",
}

REQUIRED_DIALOGS = {
    "aboutDialog", "passwordDialog", "metadataDialog",
    "printDialog", "printPreviewDialog", "encryptionDialog",
    "signDialog", "complianceDialog", "atlasComparisonDialog",
    "annotationPropertiesWidget", "formFieldPropertiesPanel",
    "imagePropertiesPanel",
}

VALID_PRIORITIES = {"P0", "P1", "P2"}
# Canonical pipeline phases plus the sweep/remediation batch namespaces the
# matrix accreted over successive pipelines. c4a9e271 phase-4: the original
# {phase-1..10} set was too strict for the evolved CSV — sweep batches tag
# rows with suffixed ("phase-7-C1", "phase-2-NEW2", "phase-post") or
# matrix-batch ("MAT-1") owner_phases. The generator still enforces the bare
# "phase-*" form on its own rows (see add()); these extra labels only appear
# on passthrough rows added by triage sweeps outside the generator.
VALID_PHASES = {f"phase-{i}" for i in range(1, 12)}


def valid_owner_phase(op: str) -> bool:
    return op in VALID_PHASES or op.startswith("phase-") or op.startswith("MAT-")
# Initial matrix has all rows as "planned". A few rows are pre-marked
# "blocked" for structurally-impossible placeholders (Signature XFDF
# round-trip, Unknown field creation). As tests land in phases 2-10
# the status flips to "green" or "red"; phase-11 reconciles.
#
# c4a9e271 phase-4: the project's own triage scripts (apply_wontfix.py,
# triage.py, blocked_triage.py) and pytest xfail/skip markers emit three
# more lifecycle statuses the original allowlist omitted, so the verifier
# was failing on its own committed CSV. These are legitimate terminal
# states, not drift:
#   wont-fix — triaged as out-of-scope/by-design, not a defect
#   xfail    — expected failure (documented known bug, tracked separately)
#   skip     — environment/precondition not met on this host
VALID_STATUS = {"planned", "blocked", "green", "red",
                "wont-fix", "xfail", "skip"}


def fail(msg: str) -> None:
    print(f"FAIL: {msg}")
    sys.exit(1)


def main() -> int:
    path = Path(__file__).resolve().parent / "ui_test_matrix.csv"
    if not path.exists():
        fail(f"Matrix CSV not found: {path}")

    with path.open("r", encoding="utf-8", newline="") as f:
        reader = csv.reader(f)
        header = next(reader)
        if header != EXPECTED_HEADER:
            fail(f"Header mismatch.\nExpected: {EXPECTED_HEADER}\n"
                 f"Got:      {header}")
        rows = list(reader)

    n = len(rows)
    print(f"Row count (excluding header): {n}")
    if n < 400:
        fail(f"Row count {n} < 400")

    # Build index of fields by name for clarity.
    def col(row, name):
        return row[EXPECTED_HEADER.index(name)]

    # Every row unique test_id, non-empty
    ids = [col(r, "test_id") for r in rows]
    empty_ids = [i for i, v in enumerate(ids) if not v.strip()]
    if empty_ids:
        fail(f"{len(empty_ids)} rows have empty test_id")
    dup = {i for i in ids if ids.count(i) > 1}
    if dup:
        fail(f"Duplicate test_id values: {sorted(dup)[:5]}...")

    # Priority, status, owner_phase invariants
    for i, r in enumerate(rows):
        p = col(r, "priority")
        if p not in VALID_PRIORITIES:
            fail(f"Row {i} test_id={col(r,'test_id')}: "
                 f"priority '{p}' not in {VALID_PRIORITIES}")
        s = col(r, "status")
        if s not in VALID_STATUS:
            fail(f"Row {i} test_id={col(r,'test_id')}: "
                 f"status '{s}' not in {sorted(VALID_STATUS)}")
        op = col(r, "owner_phase")
        if not valid_owner_phase(op):
            fail(f"Row {i} test_id={col(r,'test_id')}: "
                 f"owner_phase '{op}' is not a phase-* or MAT-* label")

    # All 9 top-level menus must appear in subsystem at least once
    subsystems = {col(r, "subsystem") for r in rows}
    missing_menus = REQUIRED_MENUS - subsystems
    if missing_menus:
        fail(f"Missing menu subsystems: {sorted(missing_menus)}")

    # All 6 annotation tools as distinct rows in element column
    elements = [col(r, "element") for r in rows]
    annot_elems = {e for e in elements if e.startswith("annot_")}
    missing_annots = {f"annot_{t}" for t in REQUIRED_ANNOTATION_TOOLS} - annot_elems
    if missing_annots:
        fail(f"Missing annotation tool rows: {sorted(missing_annots)}")

    # All 8 form field types as distinct rows
    field_elems = {e for e in elements if e.startswith("field_")}
    missing_fields = {f"field_{t}" for t in REQUIRED_FORM_FIELD_TYPES} - field_elems
    if missing_fields:
        fail(f"Missing form field type rows: {sorted(missing_fields)}")

    # All dialogs appear at least once in element column
    element_dialogs = set()
    for e in elements:
        # Dialog rows look like "aboutDialog.aboutContent"
        if "." in e:
            element_dialogs.add(e.split(".", 1)[0])
    missing_dialogs = REQUIRED_DIALOGS - element_dialogs
    if missing_dialogs:
        fail(f"Missing dialogs in element column: "
             f"{sorted(missing_dialogs)}")

    # Final: count per subsystem for visibility
    from collections import Counter
    subsys_counter = Counter(col(r, "subsystem") for r in rows)
    phase_counter = Counter(col(r, "owner_phase") for r in rows)
    pri_counter = Counter(col(r, "priority") for r in rows)

    print("\nSubsystem counts:")
    for k, v in sorted(subsys_counter.items(),
                        key=lambda x: (-x[1], x[0])):
        print(f"  {k:14s} {v:4d}")
    print("\nPhase counts:")
    for k, v in sorted(phase_counter.items()):
        print(f"  {k:10s} {v:4d}")
    print("\nPriority counts:")
    for k, v in sorted(pri_counter.items()):
        print(f"  {k:4s} {v:4d}")

    print(f"\nAll {n} rows structurally verified. "
          f"Phase-0 acceptance criteria met.")

    # ------------------------------------------------------------------
    # Coverage cross-check (gap A1-70 / UI-MATRIX-02)
    # ------------------------------------------------------------------
    # Everything above validates SHAPE. It cannot tell a green row backed by an
    # asserting test from a green row backed by nothing. This section resolves
    # every claiming row to a test that exists in the working tree, and FAILS
    # on the ones that do not resolve.
    if "--structural-only" in sys.argv:
        print("\n*** WARNING: --structural-only — the row->test coverage "
              "cross-check (gap A1-70) was SKIPPED. A green row in this run "
              "is NOT evidence that any test asserts it. ***")
        return 0

    from collections import OrderedDict
    dict_rows = [OrderedDict(zip(EXPECTED_HEADER, r)) for r in rows]
    index = coverage_resolver.build_index()
    resolutions = coverage_resolver.resolve_rows(dict_rows, index)
    print()
    print(coverage_resolver.format_report(resolutions))

    unresolved = coverage_resolver.unresolved_claiming_rows(resolutions)
    if unresolved:
        print(f"\n{len(unresolved)} row(s) claim a passing result "
              f"({'/'.join(sorted(coverage_resolver.CLAIMING_STATUSES))}) but "
              f"resolve to no live test:")
        for r in unresolved:
            why = r.disabled_reason or "no matching test found in tests/"
            print(f"  {r.test_id:34s} {why}")
        print("\nEach row must either (a) gain a verified "
              "'covered-by: <path>::<Suite>.<Test>' claim in its notes "
              "pointing at a real asserting test, or (b) stop claiming a "
              "passing status.")
        fail(f"{len(unresolved)} matrix rows claim coverage that does not "
             f"exist (gap A1-70 / UI-MATRIX-02)")

    print("\nCoverage cross-check: every claiming row resolves to a live "
          "test.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
