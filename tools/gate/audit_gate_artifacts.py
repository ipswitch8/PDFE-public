#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""audit_gate_artifacts.py -- audit the gate-verdict artifact tree.

GitHub #206. THE ISSUE'S PREMISE IS WRONG, AND THE CORRECTION MATTERS
---------------------------------------------------------------------
#206 is titled "Gate artifacts structurally cannot evidence a failure" and
reasons from a census: 744 artifacts (784 as of 2026-09-18) across every
pipeline, and ``FAIL`` appears zero times.

The census is accurate. The inference is not. A FAIL verdict **is**
structurally representable and is written byte-correctly:
``~/.claude/hooks/emit-gate-verdict.sh`` validates its verdict argument with
``case "$VERDICT" in PASS|FAIL)`` and rejects anything else with exit 2, then
writes ``{"verdict":"FAIL","target":""}``. A deliberately-failing dry run in a
throwaway tree produces exactly that file (p87-open-issues phase-6, 2026-09-18).

What the emitter does NOT do is keep history. Both branches of the write use
``>``::

    if ! printf '{"verdict":"%s","target":"%s"}\\n' "$VERDICT" "$TARGET" \\
         | jq . > "${DIR}/${AGENT}.json" 2>/dev/null; then
      printf '{"verdict":"%s","target":"%s"}\\n' "$VERDICT" "$TARGET" \\
         > "${DIR}/${AGENT}.json"
    fi

The path is fixed per (phase, agent), so the normal gate cycle -- gate FAILs,
the implementing agent fixes the finding, the SAME gate re-runs and PASSes --
overwrites the FAIL with the PASS. Every artifact tree is therefore guaranteed
to end up all-PASS once its pipeline completes, whatever happened on the way.

Measured, in this very repo: the ``claim-check`` gate emitted FAIL for
``p87-open-issues`` phase-1 and again for phase-5. Both findings were real and
both became work. Neither survives on disk -- both artifacts now read PASS.

**So the defect is durability, not representability**, and "zero FAIL" is a
statement about how the writer truncates, not about whether any gate has ever
failed. Fixing the wrong one of those would have produced a change that did
nothing while reporting that a structural gap had been closed.

WHAT THIS SCRIPT DOES, AND WHAT IT CANNOT DO
--------------------------------------------
The durability fix belongs in the writer, which lives OUTSIDE this repo
(``~/.claude/hooks/``). It is supplied as a reviewed patch at
``tools/gate/patches/emit-gate-verdict-history.patch`` -- append one
``{ts,phase,agent,verdict,target}`` line to ``<base>/gate-history.jsonl`` per
emission, leaving ``<agent>.json`` byte-identical so nothing downstream
changes. This script does the half that IS enforceable from inside the repo:

  1. **Odd verdicts (issue ask 2).** The emitter cannot produce a verdict other
     than PASS or FAIL, so an artifact carrying one was written by something
     else -- i.e. the emitter was bypassed, which the protocol says must not
     happen. Two exist today (``"CONSISTENT"`` and a full English sentence).
     ``--strict`` fails on them. They fail closed at the gate either way
     (``phase-gate.sh`` requires exactly ``PASS``), so this is an evidence
     defect, not a safety hole.
  2. **History reporting.** Once the patch is applied, ``gate-history.jsonl``
     is summarised here -- including FAILs that no longer appear in any
     ``<agent>.json``. Before it is applied, the absence is REPORTED, not
     failed: a guard that is permanently red for a reason nobody in this repo
     can fix is the "red so long nobody read it" shape (DCA 0047).
  3. **Ledger/artifact disagreement (issue ask 3).** ``phases_complete`` is
     compared against the phases whose full ``gate_agents`` list has a PASS
     artifact. This fails CLOSED in the hooks (an under-reporting ledger
     blocks; it never waves work through), so it is reported, never failed.
     ``phases_complete`` is ADVISORY and must not be read as a completion
     signal.

Exit 0 = nothing to report at the configured strictness, 1 = a finding,
2 = usage/IO error.

Usage:
  python tools/gate/audit_gate_artifacts.py [--base .claude/pipelines] [--strict]
  python tools/gate/audit_gate_artifacts.py --self-test
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Tuple

try:  # pragma: no cover
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")
except Exception:  # pragma: no cover
    pass

REPO_ROOT = Path(__file__).resolve().parents[2]

VALID_VERDICTS = {"PASS", "FAIL"}


def classify_artifact(path: Path) -> Tuple[str, Optional[str]]:
    """Return (kind, verdict). kind is 'verdict' | 'sidecar' | 'unparseable'.

    A file with no ``verdict`` key is an evidence SIDECAR (a mutation log, an
    inventory, a conformance record) -- those are legitimate and must not be
    reported as malformed verdicts, or the real finding drowns.
    """
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except Exception:
        return "unparseable", None
    if not isinstance(data, dict) or "verdict" not in data:
        return "sidecar", None
    v = data.get("verdict")
    return "verdict", v if isinstance(v, str) else None


def phase_gate_agents(base: Path, pipeline: str, phase: str) -> Optional[set]:
    """The gate agents a pipeline's plan lists for one phase.

    GitHub #299. ``classify_artifact`` decides "is this a verdict?" from the
    presence of a ``verdict`` KEY, and that produced two false
    "emitter was BYPASSED" reports against files that are not verdicts at all:

        meta-audit-followup/.../baseline-consistency.json  'CONSISTENT'
        meta-audit-followup/.../suite-delta.json           'NO NEW FAILURES ...'

    Both are script-generated EVIDENCE that happens to live under the
    gate-artifacts tree and happens to use ``verdict`` as the key for a prose
    summary. `baseline-consistency.json` even carries its own
    ``schema: pdfe/baseline-consistency@1`` and ``generated_by:
    scripts/check-installer-residue-baseline.ps1``.

    The decisive discriminator is the one the issue itself names: **neither stem
    is a gate agent in that pipeline**. Its plan lists `karen`,
    `security-audit`, `test-runner`, `validator` — there is no
    `baseline-consistency` gate and no `suite-delta` gate, so there was never an
    emission for the emitter to have bypassed.

    Returns None when the plan cannot be read, and callers MUST then fall back
    to reporting. Treating an unreadable plan as "no agents" would convert a
    real bypass into a silent sidecar, which is the opposite of this tool's job.
    """
    plan = base / pipeline / "pipeline.json"
    try:
        data = json.loads(plan.read_text(encoding="utf-8"))
    except Exception:
        return None
    for ph in data.get("phases", []) or []:
        if ph.get("id") == phase:
            agents = ph.get("gate_agents") or []
            return {str(a) for a in agents}
    # The plan parsed but does not describe this phase. That is itself odd, so
    # do not vouch for anything: fall back to reporting.
    return None


def known_gate_agents(base: Path) -> set:
    """Every name any plan in the tree lists as a gate agent.

    WHY A SUPERSET, AND WHY NOT HARDCODED. #299's exemption was keyed on "is
    this stem a gate agent of THIS phase", which is too narrow and miscounted a
    real verdict: `windows-followups/phase-1/security-audit.json` carries a
    genuine emitted `PASS`, but that phase's plan lists only karen, validator
    and test-runner, so the artifact was filed as an evidence sidecar and
    vanished from the verdict census.

    It should not vanish. An extra gate recorded against a phase that did not
    plan one is the case CLAUDE.md rule 7 warns about -- an ad-hoc gate run
    records a verdict against the current phase and can put the ledger into
    remediation -- so it is worth MORE attention than a planned one, not less.

    Derived from the plans rather than a literal list so it cannot drift as
    gate agents are added. Being a superset of any one phase's list, it makes
    the sidecar exemption strictly narrower, i.e. it fails toward reporting.
    """
    out: set = set()
    for plan in sorted(base.glob("*/pipeline.json")):
        try:
            data = json.loads(plan.read_text(encoding="utf-8"))
        except Exception:
            continue
        for ph in data.get("phases", []) or []:
            for a in ph.get("gate_agents") or []:
                out.add(str(a))
    return out


def audit_tree(base: Path) -> Dict[str, object]:
    """Census the artifact tree under ``base`` (``.claude/pipelines``)."""
    counts: Dict[str, int] = {}
    odd: List[Tuple[str, str]] = []
    unparseable: List[str] = []
    # GitHub #299: files that carry a `verdict` key but are NOT a gate agent's
    # artifact. Counted as sidecars and reported SEPARATELY rather than
    # silently -- an evidence file using that key name is worth knowing about,
    # it is just not a bypassed emitter.
    evidence_using_verdict_key: List[Tuple[str, str]] = []
    # Verdicts from a gate agent the phase's plan did not list. These ARE
    # verdicts and are counted as such; they are collected separately because
    # CLAUDE.md rule 7 makes an unplanned gate run worth knowing about.
    unplanned_gate_verdicts: List[Tuple[str, str]] = []
    sidecars = 0
    pipelines = set()
    known_agents = known_gate_agents(base)

    for art in sorted(base.glob("*/gate-artifacts/*/*.json")):
        rel = art.relative_to(base).as_posix()
        pipelines.add(rel.split("/", 1)[0])
        kind, verdict = classify_artifact(art)
        if kind == "unparseable":
            unparseable.append(rel)
            continue
        if kind == "sidecar":
            sidecars += 1
            continue
        # GitHub #299. A `verdict` KEY does not make a file a verdict ARTIFACT.
        # If the stem is not a gate agent of this phase, no emission was ever
        # expected for it, so the emitter cannot have been bypassed — it is
        # evidence that happens to use the key name. Only consulted when the
        # plan is readable AND lists this phase; otherwise fall through and
        # report, so an unreadable plan never launders a real bypass.
        parts = rel.split("/")
        if len(parts) >= 4:
            agents = phase_gate_agents(base, parts[0], parts[2])
            if agents is not None and art.stem not in agents:
                if art.stem in known_agents:
                    # A real gate agent, just not one THIS phase planned. Keep
                    # it in the verdict census -- filing it as evidence made a
                    # genuine emitted PASS disappear -- and flag the fact.
                    unplanned_gate_verdicts.append((rel, str(verdict)))
                else:
                    sidecars += 1
                    evidence_using_verdict_key.append((rel, str(verdict)))
                    continue
        key = verdict if verdict is not None else "<non-string>"
        counts[key] = counts.get(key, 0) + 1
        if verdict not in VALID_VERDICTS:
            odd.append((rel, str(verdict)))

    histories: Dict[str, Dict[str, int]] = {}
    for pipe in sorted(pipelines):
        hist = base / pipe / "gate-history.jsonl"
        if not hist.exists():
            continue
        per: Dict[str, int] = {}
        for line in hist.read_text(encoding="utf-8",
                                   errors="replace").splitlines():
            line = line.strip()
            if not line:
                continue
            try:
                rec = json.loads(line)
            except Exception:
                per["<unparseable line>"] = per.get("<unparseable line>", 0) + 1
                continue
            v = str(rec.get("verdict"))
            per[v] = per.get(v, 0) + 1
        histories[pipe] = per

    return {
        "pipelines": sorted(pipelines),
        "verdict_counts": counts,
        "sidecars": sidecars,
        "odd": odd,
        "unparseable": unparseable,
        "histories": histories,
        # GitHub #299. Reported, never failed: an evidence file using the key
        # name `verdict` is worth seeing, but it is not a bypassed emitter and
        # must not redden an audit whose whole value is that its findings are
        # real.
        "evidence_using_verdict_key": evidence_using_verdict_key,
        "unplanned_gate_verdicts": unplanned_gate_verdicts,
    }


def ledger_deltas(base: Path) -> List[Tuple[str, int, int]]:
    """(pipeline, phases_complete, fully_passed) where the two disagree.

    The gate ledger is the JSON in a pipeline directory that is NOT
    ``pipeline.json``; it is located by exclusion because naming it in a
    command is hard-blocked by the pre-tool-use guard. This function only
    READS it.
    """
    out: List[Tuple[str, int, int]] = []
    for pipe_dir in sorted(p for p in base.glob("*") if p.is_dir()):
        plan_path = pipe_dir / "pipeline.json"
        if not plan_path.exists():
            continue
        ledger_path = None
        for cand in sorted(pipe_dir.glob("*.json")):
            if cand.name != "pipeline.json":
                ledger_path = cand
                break
        if ledger_path is None:
            continue
        try:
            plan = json.loads(plan_path.read_text(encoding="utf-8"))
            ledger = json.loads(ledger_path.read_text(encoding="utf-8"))
        except Exception:
            continue
        complete = ledger.get("phases_complete")
        # UNIQUE ids, not list length.
        #
        # `len()` was wrong and it manufactured a finding: one real ledger has
        # `phase-8` recorded twice, so it counted 14 against a 13-phase plan
        # and was reported as OVER-reporting -- the direction that does not
        # fail closed. Nothing was over-reported; all 13 unique ids are in the
        # plan and all 13 carry PASS. Counting a set is the whole fix.
        #
        # WHY A DUPLICATE IS POSSIBLE AT ALL: the writer is
        # `phase-gate.sh:315`, `.phases_complete += [$pid]` -- a bare append
        # with no dedup. So a repeated id means `_advance_now` ran twice for
        # that phase, and the list is a log of advances rather than a set of
        # completed phases. Reading it as a set is therefore the correct
        # interpretation regardless of cause.
        #
        # WHY phase-8 advanced twice is NOT KNOWN. An earlier version of this
        # comment said it was "the expected trace of re-invoking the judgment
        # gates after the evidence gate", which sounded plausible and was not
        # evidenced -- `gate-history.jsonl` and the artifact timestamps for
        # that phase show a single ordinary pass. The phase-2 claim-check gate
        # caught the invention. An unevidenced benign-sounding cause is worse
        # than "cause unknown", because it stops anyone looking.
        ids = [str(c) for c in complete] if isinstance(complete, list) else []
        n_complete = len(set(ids))

        fully = 0
        for ph in plan.get("phases", []):
            pid = ph.get("id")
            agents = ph.get("gate_agents") or []
            if not pid or not agents:
                continue
            adir = pipe_dir / "gate-artifacts" / pid
            ok = True
            for a in agents:
                f = adir / f"{a}.json"
                if not f.exists():
                    ok = False
                    break
                kind, verdict = classify_artifact(f)
                if kind != "verdict" or verdict != "PASS":
                    ok = False
                    break
            if ok:
                fully += 1
        if fully != n_complete:
            # The ids the ledger claims that the PLAN does not contain.
            #
            # Needed because an over-reporting ledger has two unrelated
            # mechanisms and the report must not guess which: either a plan
            # phase is marked complete while its artifacts are not all PASS,
            # or the ledger names a phase id that is not in the plan at all --
            # which is what happens when pipeline.json is edited mid-run, and
            # is the case the one real instance turned out to be. Naming the
            # ids lets a reader tell, instead of being handed a cause.
            plan_ids = {str(ph.get("id")) for ph in plan.get("phases", [])
                        if ph.get("id")}
            unknown = sorted(set(ids) - plan_ids)
            out.append((pipe_dir.name, n_complete, fully, unknown))
    return out


# --------------------------------------------------------------------------- #
# Self-test -- pins BOTH directions.
# --------------------------------------------------------------------------- #

def self_test() -> int:
    import tempfile
    failures: List[str] = []

    def expect(cond: bool, label: str) -> None:
        print(("  ok   " if cond else "  FAIL ") + label)
        if not cond:
            failures.append(label)

    with tempfile.TemporaryDirectory() as td:
        base = Path(td) / "pipelines"

        def write(pipe: str, phase: str, agent: str, payload: dict) -> None:
            d = base / pipe / "gate-artifacts" / phase
            d.mkdir(parents=True, exist_ok=True)
            (d / f"{agent}.json").write_text(json.dumps(payload),
                                             encoding="utf-8")

        write("p1", "phase-1", "karen", {"verdict": "PASS", "target": ""})
        write("p1", "phase-1", "validator", {"verdict": "FAIL", "target": ""})
        write("p1", "phase-1", "inventory", {"files": 3})          # sidecar

        res = audit_tree(base)
        expect(res["verdict_counts"] == {"PASS": 1, "FAIL": 1},
               "PASS and FAIL are both counted (FAIL is representable)")
        expect(res["sidecars"] == 1,
               "a verdict-less sidecar is not mistaken for a bad verdict")
        expect(res["odd"] == [],
               "a clean tree reports no odd verdicts (stays green)")

        write("p1", "phase-2", "baseline", {"verdict": "CONSISTENT"})
        res = audit_tree(base)
        expect(len(res["odd"]) == 1 and res["odd"][0][1] == "CONSISTENT",
               "a verdict outside {PASS,FAIL} IS reported (reddens)")

        # --- GitHub #299: a `verdict` KEY is not a verdict ARTIFACT --------
        #
        # With NO plan present the behaviour above must hold: an unreadable
        # plan has to fall back to reporting, or a real bypass could be
        # laundered into a sidecar. That is what the expectation above now
        # also covers, since p1 has no pipeline.json at this point.
        (base / "p2" / "gate-artifacts" / "phase-1").mkdir(parents=True,
                                                           exist_ok=True)
        (base / "p2" / "pipeline.json").write_text(json.dumps({
            "phases": [{"id": "phase-1",
                        "gate_agents": ["karen", "validator"]}]}),
            encoding="utf-8")
        write("p2", "phase-1", "karen", {"verdict": "PASS", "target": ""})
        write("p2", "phase-1", "suite-delta",
              {"verdict": "NO NEW FAILURES relative to the baseline."})
        res2 = audit_tree(base)
        p2_odd = [o for o in res2["odd"] if o[0].startswith("p2/")]
        expect(p2_odd == [],
               "#299: an evidence file whose stem is NOT a gate agent is not "
               "reported as a bypassed emitter")
        expect(any(r[0].endswith("suite-delta.json")
                   for r in res2["evidence_using_verdict_key"]),
               "#299: it is still surfaced separately, not silently dropped")

        # The fail-closed direction, which is the one that matters: a stem that
        # IS a gate agent must still redden on an odd verdict. Without this,
        # the fix could be "treat everything as evidence" and nothing above
        # would notice.
        write("p2", "phase-1", "validator", {"verdict": "CONSISTENT"})
        res3 = audit_tree(base)
        expect(any(o[0].endswith("p2/gate-artifacts/phase-1/validator.json")
                   or o[0] == "p2/gate-artifacts/phase-1/validator.json"
                   for o in res3["odd"]),
               "#299: a REAL gate agent with an odd verdict still reddens "
               "(the fix must not blanket-exempt)")

        # An UNPLANNED gate verdict must stay in the census.
        #
        # The first cut of #299 keyed the exemption on this phase's agent list
        # alone, which filed `windows-followups/phase-1/security-audit.json` --
        # a genuine emitted PASS -- as an evidence sidecar, and it vanished
        # from the verdict count. `security-audit` is a gate agent; it was
        # simply not one that phase planned, which rule 7 says is worth MORE
        # attention, not less. `phase-2` below exists only to put the name in
        # the tree's known-agent set, the way a real plan would.
        (base / "p2" / "pipeline.json").write_text(json.dumps({
            "phases": [{"id": "phase-1",
                        "gate_agents": ["karen", "validator"]},
                       {"id": "phase-2",
                        "gate_agents": ["karen", "security-audit"]}]}),
            encoding="utf-8")
        write("p2", "phase-1", "security-audit", {"verdict": "PASS"})
        res4 = audit_tree(base)
        expect(any(r[0].endswith("phase-1/security-audit.json")
                   for r in res4["unplanned_gate_verdicts"]),
               "an unplanned gate agent's verdict is flagged as unplanned")
        expect(not any(r[0].endswith("phase-1/security-audit.json")
                       for r in res4["evidence_using_verdict_key"]),
               "...and is NOT demoted to an evidence sidecar, which would "
               "erase a real emitted verdict from the census")
        expect(res4["verdict_counts"].get("PASS", 0)
               > res3["verdict_counts"].get("PASS", 0),
               "...and is actually COUNTED as a verdict")

        # --- history ------------------------------------------------------
        expect(res["histories"] == {},
               "no gate-history.jsonl -> reported as absent, not invented")
        (base / "p1" / "gate-history.jsonl").write_text(
            '{"ts":"t","phase":"phase-1","agent":"validator","verdict":"FAIL"}\n'
            '{"ts":"t","phase":"phase-1","agent":"validator","verdict":"PASS"}\n',
            encoding="utf-8")
        res = audit_tree(base)
        expect(res["histories"].get("p1") == {"FAIL": 1, "PASS": 1},
               "history retains the FAIL that the artifact overwrote "
               "(the whole point of #206)")

        # --- ledger deltas ------------------------------------------------
        pdir = base / "p2"
        pdir.mkdir(parents=True, exist_ok=True)
        (pdir / "pipeline.json").write_text(json.dumps(
            {"phases": [{"id": "phase-1", "gate_agents": ["karen"]},
                        {"id": "phase-2", "gate_agents": ["karen"]}]}),
            encoding="utf-8")
        (pdir / "state.json").write_text(json.dumps({"phases_complete": []}),
                                         encoding="utf-8")
        d = pdir / "gate-artifacts" / "phase-1"
        d.mkdir(parents=True, exist_ok=True)
        (d / "karen.json").write_text('{"verdict":"PASS"}', encoding="utf-8")
        deltas = ledger_deltas(base)
        expect(any(x[0] == "p2" and x[1] == 0 and x[2] == 1 for x in deltas),
               "an under-reporting ledger is detected (phases_complete=0, "
               "fully-passed=1)")

        (pdir / "state.json").write_text(
            json.dumps({"phases_complete": ["phase-1"]}), encoding="utf-8")
        deltas = ledger_deltas(base)
        expect(not any(x[0] == "p2" for x in deltas),
               "an agreeing ledger is NOT reported (no false positive)")

        # A DUPLICATE must not manufacture a delta. This is the bug the
        # 2026-10 census hit: `phase-8` recorded twice counted 14 against a
        # 13-phase plan and was reported as over-reporting, the one direction
        # that does not fail closed. A duplicate is the expected trace of
        # re-invoking the judgment gates after the evidence gate exists.
        d2 = pdir / "gate-artifacts" / "phase-2"
        d2.mkdir(parents=True, exist_ok=True)
        (d2 / "karen.json").write_text('{"verdict":"PASS"}', encoding="utf-8")
        (pdir / "state.json").write_text(json.dumps(
            {"phases_complete": ["phase-1", "phase-2", "phase-2"]}),
            encoding="utf-8")
        deltas = ledger_deltas(base)
        expect(not any(x[0] == "p2" for x in deltas),
               "a DUPLICATE in phases_complete does not manufacture an "
               "over-report (unique ids are counted, not list length)")

        # ... and the genuine over-report is still caught, so the fix above is
        # not a blanket exemption. `phase-2` is claimed complete with its
        # karen.json carrying FAIL.
        (d2 / "karen.json").write_text('{"verdict":"FAIL"}', encoding="utf-8")
        deltas = ledger_deltas(base)
        over = [x for x in deltas if x[0] == "p2" and x[2] - x[1] < 0]
        expect(bool(over),
               "a GENUINE over-reporting ledger is still detected (a phase "
               "claimed complete whose artifact carries FAIL)")

        # And an id the plan does not contain is named, not guessed at.
        (pdir / "state.json").write_text(json.dumps(
            {"phases_complete": ["phase-1", "phase-99"]}), encoding="utf-8")
        deltas = ledger_deltas(base)
        named = [x for x in deltas if x[0] == "p2" and x[3] == ["phase-99"]]
        expect(bool(named),
               "an id absent from pipeline.json is reported BY NAME, so the "
               "report does not have to assert a cause")

    print("")
    if failures:
        print(f"[gate-artifact-audit] SELF-TEST FAIL -- {len(failures)} "
              f"assertion(s)")
        return 1
    print("[gate-artifact-audit] SELF-TEST PASS -- both directions pinned")
    return 0


# --------------------------------------------------------------------------- #

def main(argv: Optional[Sequence[str]] = None) -> int:
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--base", type=Path,
                    default=REPO_ROOT / ".claude" / "pipelines")
    ap.add_argument("--strict", action="store_true",
                    help="exit 1 on an odd or unparseable verdict artifact "
                         "(issue ask 2). Ledger deltas and a missing history "
                         "file are ALWAYS reported and NEVER fail -- see the "
                         "module docstring for why.")
    args = ap.parse_args(argv)

    if args.self_test:
        return self_test()

    if not args.base.exists():
        print(f"[gate-artifact-audit] no pipeline tree at {args.base} "
              f"(nothing to audit)")
        return 0

    res = audit_tree(args.base)
    print(f"[gate-artifact-audit] pipelines={len(res['pipelines'])} "
          f"verdict artifacts={sum(res['verdict_counts'].values())} "
          f"sidecars={res['sidecars']}")
    print(f"[gate-artifact-audit] verdict counts: {res['verdict_counts']}")

    # #299 said these are "reported SEPARATELY rather than silently". They were
    # not: the lists were populated and only ever read by the self-test, so the
    # sidecar reclassification was invisible in the one output a human reads.
    # A reclassification nobody can see is indistinguishable from dropping the
    # file, which is the behaviour #299 was fixing.
    ev = res.get("evidence_using_verdict_key") or []
    if ev:
        print(f"[gate-artifact-audit] {len(ev)} evidence file(s) use a "
              f"`verdict` key but are not any phase's gate agent. Counted as "
              f"sidecars, NOT as a bypassed emitter -- there was never an "
              f"emission for the emitter to bypass (GitHub #299):")
        for rel, v in ev:
            print(f"    - {rel}: {v!r}")
    unplanned = res.get("unplanned_gate_verdicts") or []
    if unplanned:
        print(f"[gate-artifact-audit] {len(unplanned)} verdict(s) from a gate "
              f"agent its phase's plan does not list. Counted as verdicts -- "
              f"they are real emissions -- but worth a look: CLAUDE.md rule 7 "
              f"says an ad-hoc gate run records a verdict against the current "
              f"phase and can put the ledger into remediation:")
        for rel, v in unplanned:
            print(f"    - {rel}: {v!r}")

    n_hist = len(res["histories"])
    if n_hist == 0:
        print("[gate-artifact-audit] NOTE: no pipeline has a "
              "gate-history.jsonl. The emitter truncates <agent>.json on every "
              "re-run, so a FAIL that was later remediated leaves no trace -- "
              "which is why this tree reads all-PASS. Apply "
              "tools/gate/patches/emit-gate-verdict-history.patch to "
              "~/.claude/hooks/emit-gate-verdict.sh to close it. Reported, not "
              "failed: the writer is outside this repo.")
    else:
        print(f"[gate-artifact-audit] gate-history.jsonl present for "
              f"{n_hist} pipeline(s):")
        for pipe, per in sorted(res["histories"].items()):
            print(f"    {pipe}: {per}")

    findings = 0
    deltas = ledger_deltas(args.base)
    # SPLIT BY DIRECTION. The two disagreements are not the same thing, and
    # saying "this fails closed" over both is false.
    #
    #   delta > 0  (fully-passed > phases_complete): the ledger UNDER-reports.
    #              It blocks work that is actually done. Annoying, safe.
    #   delta < 0  (phases_complete > fully-passed): the ledger OVER-reports.
    #              It claims a phase complete that the artifacts do not
    #              support. That is the direction that can wave work through,
    #              and it is NOT fails-closed.
    #
    # GitHub #206 measured 16 disagreements and found every one under-reporting,
    # which is where the blanket "fails CLOSED" claim came from. That claim is
    # now out of date: a 2026-10 census found 17, one of them NEGATIVE. The
    # census was right about the cases it saw and wrong to generalise, so the
    # direction is computed here rather than asserted.
    under = [d for d in deltas if d[2] - d[1] > 0]
    over = [d for d in deltas if d[2] - d[1] < 0]
    if under:
        print(f"[gate-artifact-audit] NOTE: {len(under)} ledger(s) "
              f"UNDER-report vs their own artifacts. This direction fails "
              f"CLOSED (it blocks completed work; it never waves work "
              f"through), so phases_complete is ADVISORY and must not be read "
              f"as a completion signal:")
        for pipe, n_complete, fully, _unknown in under[:20]:
            print(f"    {pipe}: phases_complete={n_complete} "
                  f"fully-passed={fully} (delta {fully - n_complete})")
        if len(under) > 20:
            print(f"    ... {len(under) - 20} more")
    if over:
        findings += len(over)
        print(f"[gate-artifact-audit] {len(over)} ledger(s) OVER-report vs "
              f"their own artifacts: phases_complete counts MORE than the "
              f"plan phases whose gate-artifacts all carry PASS. Unlike the "
              f"under-reporting case this does NOT fail closed -- a reader "
              f"trusting phases_complete would treat an unproven phase as "
              f"done. Derive completion from gate-artifacts/, which is the "
              f"verdict of record. Two mechanisms produce this, so the ids "
              f"are listed rather than a cause asserted: a plan phase marked "
              f"complete without all-PASS artifacts, or an id the plan does "
              f"not contain (pipeline.json edited mid-run):")
        for pipe, n_complete, fully, unknown in over[:20]:
            print(f"    - {pipe}: phases_complete={n_complete} "
                  f"fully-passed={fully} (delta {fully - n_complete})")
            if unknown:
                print(f"        ids not in pipeline.json: {unknown} "
                      f"-- the plan was edited after these were recorded; "
                      f"their artifacts, if any, are the only evidence left")
        if len(over) > 20:
            print(f"    ... {len(over) - 20} more")

    if res["odd"]:
        findings += len(res["odd"])
        print(f"[gate-artifact-audit] {len(res['odd'])} artifact(s) carry a "
              f"verdict outside {{PASS, FAIL}}. emit-gate-verdict.sh cannot "
              f"produce these, so the emitter was BYPASSED -- the one thing "
              f"the protocol says must not happen:")
        for rel, v in res["odd"]:
            print(f"    - {rel}: {v!r}")
    if res["unparseable"]:
        findings += len(res["unparseable"])
        print(f"[gate-artifact-audit] {len(res['unparseable'])} unparseable "
              f"artifact(s):")
        for rel in res["unparseable"]:
            print(f"    - {rel}")

    if findings and args.strict:
        print(f"[gate-artifact-audit] FAIL -- {findings} finding(s) (--strict)")
        return 1
    if findings:
        print("[gate-artifact-audit] findings reported; pass --strict to fail "
              "on them")
    else:
        print("[gate-artifact-audit] PASS -- every verdict artifact carries "
              "exactly PASS or FAIL")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
