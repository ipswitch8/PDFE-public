#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""check_crash_dumps.py — a run that crashed PDFE is not a passing run.

GitHub #237. `windows-validation.yml` purges stale crash dumps, collects them
and uploads them to private R2 — and **no step fails when one appears**. So the
application can terminate any number of times during a validation run and the
lane stays green, provided the tests report the same statuses they would have
anyway.

That is not hypothetical. The 2026-09-19 sweep produced **13 dumps** across two
distinct defects (#234, #235) and reported neither as a crash. #235 is the
sharper case: fixing it changed *no test result at all*. Measured on the same
file, before and after the fix —

    before:  2 crash dumps,  2 failing tests
    after :  0 crash dumps,  2 failing tests  (the same two)

— so the only evidence that defect existed anywhere was the files in
`ui-out/crashdumps/`. Nothing read them.

Compounding it, a crash does not even look like one in the suite output: it
surfaces as `WIN-45 reachability blocked: <control> not reachable`, because a
control hosted by a dead process genuinely is unreachable (DCA 0083). Between
that and this, a crash could be fully absorbed and produce no signal.

WHY THE EPOCH MARKER. The dump directory is shared by a three-shard matrix on
one machine, so it deliberately is NOT purged per shard — `.crashdump-epoch`
records which run owns the directory. This check honours the same rule: dumps
count only when the marker names the run being judged. A previous run's dumps
must not fail this one, and — more importantly — must not be silently credited
to it either.

WHAT IT DOES NOT DO. It never opens a dump. They carry heap, which can include
document passwords, decrypted content from copy-restricted PDFs and OCSP/CRL
tokens; that is why they go to a private bucket rather than a GitHub artifact.
This reports counts and file names only.

Usage:
  python tools/gate/check_crash_dumps.py --dump-dir ui-out/crashdumps \\
      --epoch ui-out/.crashdump-epoch --run-id "$env:GITHUB_RUN_ID"
  python tools/gate/check_crash_dumps.py --self-test
Exit 0 = no dumps owned by this run, 1 = the run crashed, 2 = usage error.
"""
from __future__ import annotations

import argparse
import pathlib
import sys
import tempfile

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")


def owned_dumps(dump_dir: pathlib.Path, epoch: pathlib.Path,
                run_id: str) -> tuple[list[str], str]:
    """Return (dump names owned by *run_id*, the marker's owner).

    Pure enough to unit-test with a temp directory; no global state, no clock.
    A missing directory or marker is "no dumps", not an error: a run that never
    started the UI leg has nothing to answer for.
    """
    owner = ""
    if epoch.is_file():
        owner = epoch.read_text(encoding="utf-8", errors="replace").strip()
    if not dump_dir.is_dir():
        return [], owner
    names = sorted(p.name for p in dump_dir.glob("*.dmp") if p.is_file())
    if not names:
        return [], owner
    # Dumps exist. They belong to this run only if the marker says so.
    if run_id and owner and owner != run_id:
        return [], owner
    return names, owner


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--dump-dir", default="ui-out/crashdumps")
    ap.add_argument("--epoch", default="ui-out/.crashdump-epoch")
    ap.add_argument("--run-id", default="")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()

    if args.self_test:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            dumps = root / "crashdumps"
            dumps.mkdir()
            epoch = root / ".crashdump-epoch"

            # Nothing at all -> clean.
            assert owned_dumps(dumps, epoch, "42") == ([], "")

            # A dump with a marker naming THIS run -> reported.
            (dumps / "pdfe_crash_a.dmp").write_text("x", encoding="utf-8")
            epoch.write_text("42\n", encoding="utf-8")
            names, owner = owned_dumps(dumps, epoch, "42")
            assert names == ["pdfe_crash_a.dmp"], names
            assert owner == "42", owner

            # The SAME dump, marker naming a DIFFERENT run -> not ours.
            epoch.write_text("41\n", encoding="utf-8")
            assert owned_dumps(dumps, epoch, "42") == ([], "41")

            # No marker at all but dumps present -> reported, because the
            # cautious reading of an unowned dump is that it is ours. Failing a
            # green run is recoverable; passing a crashed one is what #237 is.
            epoch.unlink()
            names, _ = owned_dumps(dumps, epoch, "42")
            assert names == ["pdfe_crash_a.dmp"], names

            # Missing directory -> clean, not an error.
            assert owned_dumps(root / "nope", epoch, "42") == ([], "")

            # A non-dump file is not a crash.
            (dumps / "pdfe_crash_a.dmp").unlink()
            (dumps / "readme.txt").write_text("x", encoding="utf-8")
            assert owned_dumps(dumps, epoch, "42") == ([], "")

        print("[check-crash-dumps] self-test: 6 checks passed")
        return 0

    dumps, owner = owned_dumps(pathlib.Path(args.dump_dir),
                               pathlib.Path(args.epoch), args.run_id)
    if not dumps:
        print(f"[check-crash-dumps] OK: no crash dumps owned by run "
              f"{args.run_id or '<unset>'} (marker owner: {owner or '<none>'})")
        return 0

    print(f"[check-crash-dumps] FAIL: PDFE crashed {len(dumps)} time(s) during "
          f"this run. A run that produced a crash dump is not a passing run "
          f"(GitHub #237).", file=sys.stderr)
    for n in dumps:
        print(f"    {n}", file=sys.stderr)
    print("  The dumps are uploaded to the private R2 bucket. Symbolize them "
          "BEFORE rebuilding: a dump whose pdb no longer matches is a "
          "timestamp and an exception code (DCA 0083).", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
