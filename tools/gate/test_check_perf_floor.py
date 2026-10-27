#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Unit tests for the performance-regression floor gate (C-07).

Proves the floor actually GATES (like tools/gate/test_mutation_kill_floor.py
proves the mutation floor): a below-baseline (regressed) measurement returns a
non-zero exit; a within-threshold measurement returns zero; a measured run that
drops a baseline metric is a HARD error (exit 2); missing/unparseable inputs are
HARD errors. The CI perf lane turns the non-zero exit into a failed build, so
this is the unit-level proof the floor is a gate, not a report.

Run:
    python -m pytest tools/gate/test_check_perf_floor.py -q
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import check_perf_floor  # noqa: E402


BASELINES = {
    "threshold_multiplier": 3.0,
    "metrics": {
        "document_open_ms": {"baseline_ms": 10.0},
        "save_routing_ms": {"baseline_ms": 20.0, "threshold_multiplier": 2.0},
        "search_ms": {"baseline_ms": 5.0},
        "tile_render_ms": {"baseline_ms": 8.0},
    },
}


def _write(tmp_path: Path, name: str, obj) -> Path:
    p = tmp_path / name
    p.write_text(json.dumps(obj), encoding="utf-8")
    return p


def _run(tmp_path, measured, baselines=BASELINES, json_out=False):
    b = _write(tmp_path, "perf_baselines.json", baselines)
    m = _write(tmp_path, "perf-measured.json", measured)
    argv = ["--baselines", str(b), "--measured", str(m)]
    if json_out:
        argv += ["--json-out", str(tmp_path / "report.json")]
    return check_perf_floor.main(argv)


def test_within_threshold_passes(tmp_path):
    """Every metric at/under ba*threshold -> exit 0."""
    measured = {"document_open_ms": 11.0,   # 1.1x  (<3x)
                "save_routing_ms": 39.0,     # 1.95x (<2x per-metric)
                "search_ms": 12.0,           # 2.4x  (<3x)
                "tile_render_ms": 8.0}       # 1.0x
    assert _run(tmp_path, measured) == 0


def test_regression_beyond_threshold_fails(tmp_path):
    """A metric past its threshold -> exit 1 (the gate blocks the build)."""
    # document_open regresses to 4.0x (> 3x default) -> regression.
    measured = {"document_open_ms": 40.0,
                "save_routing_ms": 20.0,
                "search_ms": 5.0,
                "tile_render_ms": 8.0}
    assert _run(tmp_path, measured) == 1


def test_per_metric_threshold_is_tighter(tmp_path):
    """save_routing has a 2.0x per-metric threshold that overrides the 3x default."""
    # 2.5x of baseline: under the 3x default but OVER the 2.0x per-metric cap.
    measured = {"document_open_ms": 10.0,
                "save_routing_ms": 50.0,   # 2.5x > 2.0x -> regression
                "search_ms": 5.0,
                "tile_render_ms": 8.0}
    assert _run(tmp_path, measured) == 1


def test_missing_measured_metric_is_hard_error(tmp_path):
    """A baseline metric absent from the measured run -> exit 2 (never a pass)."""
    measured = {"document_open_ms": 10.0,
                "save_routing_ms": 20.0,
                "search_ms": 5.0}  # tile_render_ms dropped
    assert _run(tmp_path, measured) == 2


def test_missing_files_are_hard_errors(tmp_path):
    b = _write(tmp_path, "perf_baselines.json", BASELINES)
    assert check_perf_floor.main(
        ["--baselines", str(b), "--measured", str(tmp_path / "nope.json")]) == 2
    m = _write(tmp_path, "perf-measured.json", {"document_open_ms": 10.0})
    assert check_perf_floor.main(
        ["--baselines", str(tmp_path / "nope.json"), "--measured", str(m)]) == 2


def test_empty_metrics_is_hard_error(tmp_path):
    assert _run(tmp_path, {"x": 1.0}, baselines={"metrics": {}}) == 2


def test_json_report_records_results(tmp_path):
    measured = {"document_open_ms": 40.0, "save_routing_ms": 20.0,
                "search_ms": 5.0, "tile_render_ms": 8.0}
    _run(tmp_path, measured, json_out=True)
    report = json.loads((tmp_path / "report.json").read_text(encoding="utf-8"))
    assert report["exit_code"] == 1
    row = {r["metric"]: r for r in report["results"]}
    assert row["document_open_ms"]["status"] == "regressed"
    assert row["search_ms"]["status"] == "pass"
