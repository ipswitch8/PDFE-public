#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Redden when the resolved libMuPDF version changes without the
PDFE-SEC-MUPDF-RECURSION-DOS reproducer being re-run.

GitHub #139.

THE GAP
-------
`PDFE-SEC-MUPDF-RECURSION-DOS` (tests/docs/known_issues.md) records a MuPDF
1.26.10 stack-overflow crash on a crafted page graph, contained at the app
layer by the out-of-process `pdfe_preflight` opener. Its resolution path says:

    (2) on a MuPDF bump, re-run `fuzz_document_open` to confirm

Nothing made that happen. A MuPDF bump can change the finding in either
direction -- fixed upstream, or moved somewhere the containment does not cover
-- and the repository would say exactly the same thing before and after. A
resolution step nobody is prompted to take is not a control.

WHAT THIS MIRRORS, AND WHY
--------------------------
`tools/gate/verify_vcpkg_overlays.py`, whose framing is that re-pinning a hash
is A DECISION to trust re-served bytes, not a refresh -- so the justification
has to be re-derivable rather than a prose comment. The same framing applies
here: bumping MuPDF is a decision to trust a new parser with hostile input, and
the evidence for that decision must be re-derived, not inherited.

So this file has the same two modes that one does:

  --check   offline, fast, fail-closed. The GATE. Compares the pinned libMuPDF
            (version AND port-version) against what the recorded re-run says it
            was run against. Any drift, and any inability to determine either
            side, is a FAILURE.
  --rerun   re-derives the record: generates the reproducer corpus, runs every
            shape through pdfe_preflight, and writes the observed outcomes with
            the CURRENT pin stamped on them.

BOTH FIELDS MATTER. `vcpkg-overlay-ports/libmupdf/vcpkg.json` carries
`version` and `port-version`. A port-version bump changes patches, build flags
and therefore the resolved library just as surely as a version bump does; a
gate keyed only on `version` would sit green through a real change. That is the
specific vacuous shape this file must not have.

FAIL-CLOSED, EVERYWHERE. A missing record, an unparseable pin, a record with
its fields blanked out, a corpus generator that has gone missing -- every one
of those is a RED, never a quiet pass. GitHub #187 catalogued checkers that
exit 0 having examined nothing; a checker that cannot tell you it checked is
indistinguishable from one that approved.

Usage:
    python tools/gate/verify_mupdf_recursion_recheck.py --check
    python tools/gate/verify_mupdf_recursion_recheck.py --rerun \\
        --preflight build/vs2022/bin/Debug/pdfe_preflight.exe --date 2026-09-13
    python tools/gate/verify_mupdf_recursion_recheck.py --self-test
"""
from __future__ import annotations

import argparse
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
PIN_FILE = REPO_ROOT / "vcpkg-overlay-ports" / "libmupdf" / "vcpkg.json"
RECORD_FILE = REPO_ROOT / "docs" / "security" / "mupdf-recursion-dos-recheck.json"
GENERATOR = REPO_ROOT / "tests" / "fixtures" / "gen_mupdf_recursion_dos.py"

FINDING_ID = "PDFE-SEC-MUPDF-RECURSION-DOS"

# Every key a record must carry. A record missing any of them is not a weaker
# record, it is an unusable one, and it fails.
REQUIRED_TOP = ("finding", "libmupdf", "reproducer", "run", "shapes")
REQUIRED_LIBMUPDF = ("version", "port_version", "pin_file")
REQUIRED_RUN = ("date", "preflight", "conclusion")


class Problem(Exception):
    """A determinable failure. Raised, collected, printed, exits 1."""


# ---------------------------------------------------------------------------
# Reading the two sides
# ---------------------------------------------------------------------------

def read_pin(pin_file: Path) -> tuple[str, int]:
    """(version, port_version) as pinned. Raises Problem if undeterminable.

    port-version is OPTIONAL in vcpkg's schema and defaults to 0, so its
    absence is legitimate and reads as 0 -- but a present-and-non-integer value
    is a broken manifest and fails rather than being coerced.
    """
    if not pin_file.exists():
        raise Problem(f"the libMuPDF pin {_rel(pin_file)} does not exist, so the "
                      f"resolved version cannot be determined at all")
    try:
        data = json.loads(io.open(pin_file, encoding="utf-8").read())
    except (ValueError, OSError) as e:
        raise Problem(f"{_rel(pin_file)} is not readable JSON ({e}); the resolved "
                      f"libMuPDF version cannot be determined")
    if not isinstance(data, dict):
        raise Problem(f"{_rel(pin_file)} is not a JSON object")
    version = data.get("version")
    if not isinstance(version, str) or not version.strip():
        raise Problem(f"{_rel(pin_file)} has no usable \"version\" field "
                      f"(got {version!r})")
    port_version = data.get("port-version", 0)
    if not isinstance(port_version, int) or isinstance(port_version, bool):
        raise Problem(f"{_rel(pin_file)} has a non-integer \"port-version\" "
                      f"({port_version!r})")
    return version.strip(), port_version


def read_record(record_file: Path) -> dict:
    if not record_file.exists():
        raise Problem(
            f"no re-run record at {_rel(record_file)}. The {FINDING_ID} "
            f"reproducer has never been run and recorded, so there is nothing "
            f"for a MuPDF bump to invalidate. Produce one with:\n"
            f"    python tools/gate/verify_mupdf_recursion_recheck.py --rerun "
            f"--preflight <pdfe_preflight.exe>")
    try:
        rec = json.loads(io.open(record_file, encoding="utf-8").read())
    except (ValueError, OSError) as e:
        raise Problem(f"{_rel(record_file)} is not readable JSON ({e})")
    if not isinstance(rec, dict):
        raise Problem(f"{_rel(record_file)} is not a JSON object")
    return rec


def _rel(p: Path) -> str:
    try:
        return p.relative_to(REPO_ROOT).as_posix()
    except ValueError:
        return str(p)


# ---------------------------------------------------------------------------
# The gate
# ---------------------------------------------------------------------------

def check(pin_file: Path = PIN_FILE, record_file: Path = RECORD_FILE,
          generator: Path = GENERATOR, quiet: bool = False) -> list[str]:
    """Return the list of problems. Empty list == PASS."""
    problems: list[str] = []

    pin = None
    try:
        pin = read_pin(pin_file)
    except Problem as e:
        problems.append(str(e))

    rec = None
    try:
        rec = read_record(record_file)
    except Problem as e:
        problems.append(str(e))

    if rec is not None:
        for key in REQUIRED_TOP:
            if key not in rec:
                problems.append(f"{_rel(record_file)} is missing the required "
                                f"top-level key \"{key}\"")
        lib = rec.get("libmupdf")
        if not isinstance(lib, dict):
            problems.append(f"{_rel(record_file)}: \"libmupdf\" is not an object")
            lib = {}
        else:
            for key in REQUIRED_LIBMUPDF:
                if key not in lib:
                    problems.append(f"{_rel(record_file)}: libmupdf.{key} is missing")
        run = rec.get("run")
        if not isinstance(run, dict):
            problems.append(f"{_rel(record_file)}: \"run\" is not an object")
            run = {}
        else:
            for key in REQUIRED_RUN:
                if not str(run.get(key, "")).strip():
                    problems.append(f"{_rel(record_file)}: run.{key} is empty or "
                                    f"missing -- a record that does not say when, "
                                    f"with what, or what happened records nothing")
        shapes = rec.get("shapes")
        if not isinstance(shapes, list) or not shapes:
            problems.append(f"{_rel(record_file)}: \"shapes\" must be a non-empty "
                            f"list -- a re-run that exercised no input is not a "
                            f"re-run")
        else:
            for i, s in enumerate(shapes):
                if not isinstance(s, dict) or not str(s.get("name", "")).strip() \
                        or "exit_code" not in s:
                    problems.append(f"{_rel(record_file)}: shapes[{i}] needs at "
                                    f"least a name and an exit_code")

        if rec.get("finding") != FINDING_ID:
            problems.append(f"{_rel(record_file)}: \"finding\" is "
                            f"{rec.get('finding')!r}, expected {FINDING_ID!r} -- "
                            f"this record is about some other finding")

        # THE DRIFT CHECK -- both fields, independently reported.
        if pin is not None and isinstance(lib, dict):
            pin_version, pin_port = pin
            rec_version = lib.get("version")
            rec_port = lib.get("port_version")
            if rec_version != pin_version:
                problems.append(
                    f"libMuPDF VERSION drifted: {_rel(pin_file)} pins "
                    f"{pin_version!r} but the {FINDING_ID} reproducer was last "
                    f"run against {rec_version!r}. A MuPDF bump can move this "
                    f"crash class in either direction. Re-run the reproducer and "
                    f"re-record before this can go green:\n"
                    f"    python tools/gate/verify_mupdf_recursion_recheck.py "
                    f"--rerun --preflight <pdfe_preflight.exe>")
            if rec_port != pin_port:
                problems.append(
                    f"libMuPDF PORT-VERSION drifted: {_rel(pin_file)} pins "
                    f"port-version {pin_port!r} but the {FINDING_ID} reproducer "
                    f"was last run against {rec_port!r}. A port-version bump "
                    f"changes patches and build flags, so it changes the "
                    f"resolved library just as a version bump does. Re-run the "
                    f"reproducer and re-record.")

    if not generator.exists():
        problems.append(
            f"the reproducer generator {_rel(generator)} is missing, so the "
            f"recorded re-run cannot be repeated by anyone. A record whose "
            f"input no longer exists is prose again.")

    if not quiet:
        for p in problems:
            print(f"FAIL: {p}")
        if not problems:
            assert pin is not None
            print(f"OK: the {FINDING_ID} reproducer was re-run against the "
                  f"libMuPDF now pinned (version {pin[0]}, port-version {pin[1]}); "
                  f"record {_rel(record_file)}.")
    return problems


# ---------------------------------------------------------------------------
# Re-derivation
# ---------------------------------------------------------------------------

def rerun(preflight: Path, date: str, timeout: int = 300) -> int:
    """Generate the corpus, run every shape through pdfe_preflight, rewrite the
    record stamped with the CURRENT pin."""
    if not preflight.exists():
        print(f"FAIL: --preflight {preflight} does not exist")
        return 1
    if not GENERATOR.exists():
        print(f"FAIL: generator {_rel(GENERATOR)} does not exist")
        return 1
    try:
        version, port_version = read_pin(PIN_FILE)
    except Problem as e:
        print(f"FAIL: {e}")
        return 1

    tmp = Path(tempfile.mkdtemp(prefix="pdfe_recursion_dos_"))
    try:
        gen = subprocess.run(
            [sys.executable, str(GENERATOR), "--all", "--out-dir", str(tmp)],
            capture_output=True, text=True)
        if gen.returncode != 0:
            print(f"FAIL: corpus generation failed rc={gen.returncode}\n{gen.stderr}")
            return 1

        sys.path.insert(0, str(GENERATOR.parent))
        import gen_mupdf_recursion_dos as gmod   # noqa: E402

        shapes: list[dict] = []
        for name in sorted(gmod.SHAPES):
            pdf = tmp / f"recursion-dos-{name}.pdf"
            entry: dict = {
                "name": name,
                "description": gmod.SHAPES[name][1],
                "bytes": pdf.stat().st_size if pdf.exists() else 0,
            }
            if not pdf.exists():
                entry["exit_code"] = None
                entry["outcome"] = "GENERATOR DID NOT PRODUCE THIS SHAPE"
                shapes.append(entry)
                continue
            try:
                r = subprocess.run([str(preflight), str(pdf)],
                                   capture_output=True, timeout=timeout)
                rc = r.returncode & 0xFFFFFFFF
                entry["exit_code"] = rc
                if rc == 0:
                    entry["outcome"] = ("opened cleanly -- did NOT reproduce the "
                                        "crash")
                elif rc == 1:
                    entry["outcome"] = ("MuPDF refused it with an ordinary error "
                                        "-- did NOT reproduce the crash")
                elif rc >= 0xC0000000:
                    entry["outcome"] = (f"ABNORMAL TERMINATION 0x{rc:08X} -- the "
                                        f"crash REPRODUCED and was contained by "
                                        f"the preflight child")
                else:
                    entry["outcome"] = f"unexpected exit {rc}"
            except subprocess.TimeoutExpired:
                entry["exit_code"] = "timeout"
                entry["outcome"] = (f"the preflight child did not finish within "
                                    f"{timeout}s -- a hang, which the parent also "
                                    f"treats as unsafe")
            shapes.append(entry)

        reproduced = [s for s in shapes
                      if isinstance(s.get("exit_code"), int)
                      and s["exit_code"] >= 0xC0000000]
        if reproduced:
            conclusion = (
                "REPRODUCED. " + ", ".join(s["name"] for s in reproduced) +
                " crashed the preflight child. The containment held (the crash "
                "was confined to the disposable helper), and the underlying "
                "MuPDF recursion is still present at this pin.")
        else:
            conclusion = (
                "NOT REPRODUCED by this corpus. None of the reconstructed shapes "
                "crashed the preflight child at this pin. This is recorded as a "
                "WEAKER result, not a clean bill of health: the shapes are "
                "re-derived from the finding's prose description, not recovered "
                "from the original libFuzzer artifact (which is past its "
                "retention window and is not in the tree). The strong reproducer "
                "remains the windows-fuzz `fuzz_document_open` campaign. This "
                "corpus is a standing CONTROL that must be re-run and re-recorded "
                "on every MuPDF bump.")

        record = {
            "finding": FINDING_ID,
            "_comment": (
                "GENERATED by tools/gate/verify_mupdf_recursion_recheck.py "
                "--rerun. Do not hand-edit: the point of this file is that it "
                "records something that was actually run. "
                "tools/gate/verify_mupdf_recursion_recheck.py --check reddens "
                "when libmupdf.version or libmupdf.port_version below stops "
                "matching the pin."),
            "known_issues_entry": "tests/docs/known_issues.md#" + FINDING_ID.lower(),
            "github_issue": 139,
            "libmupdf": {
                "version": version,
                "port_version": port_version,
                "pin_file": _rel(PIN_FILE),
            },
            "reproducer": {
                "generator": _rel(GENERATOR),
                "command": ("python tests/fixtures/gen_mupdf_recursion_dos.py "
                            "--all --out-dir <dir>"),
                "committed": False,
                "why_not_committed": (
                    "mechanically derivable, and a crash reproducer in a "
                    "repository is a thing people accidentally open"),
            },
            "run": {
                "date": date,
                "preflight": _rel(Path(preflight).resolve()),
                "harness": ("pdfe_preflight <pdf>; exit 0 = opened cleanly, "
                            "1 = ordinary MuPDF error, >=0xC0000000 = abnormal "
                            "termination i.e. the crash"),
                "conclusion": conclusion,
            },
            "shapes": shapes,
        }
        RECORD_FILE.parent.mkdir(parents=True, exist_ok=True)
        io.open(RECORD_FILE, "w", encoding="utf-8", newline="\n").write(
            json.dumps(record, indent=2, ensure_ascii=False) + "\n")
        print(f"[recursion-recheck] wrote {_rel(RECORD_FILE)}")
        for s in shapes:
            print(f"  {s['name']:<14} exit={s['exit_code']!s:<12} {s['outcome']}")
        print(f"  conclusion: {conclusion}")
        return 0
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


# ---------------------------------------------------------------------------
# Self-test -- the gate must be able to go BOTH ways
# ---------------------------------------------------------------------------

def _write(p: Path, obj) -> None:
    p.parent.mkdir(parents=True, exist_ok=True)
    io.open(p, "w", encoding="utf-8", newline="\n").write(
        json.dumps(obj, indent=2) + "\n")


def self_test() -> int:
    """Synthesise pin/record pairs and assert the gate reddens and greens where
    it should. A guard no mutation can redden is decoration; one that can only
    redden is equally useless, so both directions are asserted."""
    failures: list[str] = []
    tmp = Path(tempfile.mkdtemp(prefix="pdfe_recheck_selftest_"))

    def good_record(version="1.26.10", port=4):
        return {
            "finding": FINDING_ID,
            "libmupdf": {"version": version, "port_version": port,
                         "pin_file": "vcpkg-overlay-ports/libmupdf/vcpkg.json"},
            "reproducer": {"generator": "tests/fixtures/gen_mupdf_recursion_dos.py"},
            "run": {"date": "2026-09-13", "preflight": "pdfe_preflight.exe",
                    "conclusion": "NOT REPRODUCED by this corpus."},
            "shapes": [{"name": "chain", "exit_code": 0, "outcome": "clean"}],
        }

    def case(label: str, pin_obj, rec_obj, gen_exists: bool, expect_pass: bool):
        d = tmp / label.replace(" ", "_")
        pin = d / "vcpkg.json"
        rec = d / "record.json"
        gen = d / "gen.py"
        d.mkdir(parents=True, exist_ok=True)
        if pin_obj is not None:
            if isinstance(pin_obj, str):
                io.open(pin, "w", encoding="utf-8").write(pin_obj)
            else:
                _write(pin, pin_obj)
        if rec_obj is not None:
            _write(rec, rec_obj)
        if gen_exists:
            io.open(gen, "w", encoding="utf-8").write("# stub\n")
        problems = check(pin, rec, gen, quiet=True)
        passed = not problems
        if passed != expect_pass:
            failures.append(
                f"{label}: expected {'PASS' if expect_pass else 'FAIL'}, got "
                f"{'PASS' if passed else 'FAIL'}"
                + ("" if passed else " -- " + "; ".join(problems)))
        return problems

    try:
        pin_ok = {"name": "libmupdf", "version": "1.26.10", "port-version": 4}

        # The one case that must go green. Without it the gate could be a
        # constant FAIL and every other assertion here would still pass.
        case("matching", pin_ok, good_record(), True, True)

        # THE PROPERTY: version drift reddens.
        p = case("version_drift",
                 {"name": "libmupdf", "version": "1.27.0", "port-version": 4},
                 good_record(), True, False)
        if not any("VERSION drifted" in x for x in p):
            failures.append("version_drift: reddened, but not for the version")

        # THE OTHER HALF OF THE PROPERTY: port-version drift reddens too. A gate
        # keyed only on `version` passes every assertion above and fails this.
        p = case("port_version_drift",
                 {"name": "libmupdf", "version": "1.26.10", "port-version": 5},
                 good_record(), True, False)
        if not any("PORT-VERSION drifted" in x for x in p):
            failures.append("port_version_drift: reddened, but not for the "
                            "port-version -- the gate is keyed on version alone")

        # A pin that omits port-version means 0, and a record saying 4 is drift.
        case("absent_port_version_is_zero",
             {"name": "libmupdf", "version": "1.26.10"},
             good_record(port=4), True, False)
        case("absent_port_version_matches_zero",
             {"name": "libmupdf", "version": "1.26.10"},
             good_record(port=0), True, True)

        # FAIL CLOSED: everything undeterminable is a red.
        case("no_record", pin_ok, None, True, False)
        case("no_pin", None, good_record(), True, False)
        case("pin_not_json", "{ this is not json", good_record(), True, False)
        case("pin_no_version", {"name": "libmupdf", "port-version": 4},
             good_record(), True, False)
        case("pin_port_version_not_int",
             {"name": "libmupdf", "version": "1.26.10", "port-version": "4"},
             good_record(), True, False)
        case("generator_missing", pin_ok, good_record(), False, False)

        # A record blanked out field-by-field must not read as a re-run.
        for key in REQUIRED_RUN:
            r = good_record()
            r["run"][key] = ""
            case(f"blank_run_{key}", pin_ok, r, True, False)
        r = good_record()
        r["shapes"] = []
        case("no_shapes", pin_ok, r, True, False)
        r = good_record()
        r["shapes"] = [{"outcome": "clean"}]
        case("shape_without_name", pin_ok, r, True, False)
        r = good_record()
        r["finding"] = "SOME-OTHER-FINDING"
        case("wrong_finding", pin_ok, r, True, False)
        r = good_record()
        del r["libmupdf"]["port_version"]
        case("record_without_port_version", pin_ok, r, True, False)

        for f in failures:
            print(f"FAIL: {f}")
        if failures:
            return 1
        print("OK: verify_mupdf_recursion_recheck self-test passed "
              "(greens on a matching record; reddens on version drift, "
              "port-version drift, missing/unparseable pin, missing/blank/"
              "wrong-finding record, empty shape list and a missing generator).")
        return 0
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--check", action="store_true",
                    help="the gate: pin vs recorded re-run, fail-closed")
    ap.add_argument("--rerun", action="store_true",
                    help="re-derive the record by actually running the corpus")
    ap.add_argument("--preflight", default="",
                    help="path to pdfe_preflight(.exe), required by --rerun")
    ap.add_argument("--date", default="", help="run date YYYY-MM-DD for --rerun")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args(argv)

    if args.self_test:
        return self_test()
    if args.rerun:
        if not args.preflight:
            ap.error("--rerun requires --preflight")
        if not args.date:
            ap.error("--rerun requires --date (the record must say WHEN)")
        return rerun(Path(args.preflight), args.date)
    if args.check:
        return 1 if check() else 0
    ap.error("give one of --check, --rerun or --self-test")


if __name__ == "__main__":
    sys.exit(main())
