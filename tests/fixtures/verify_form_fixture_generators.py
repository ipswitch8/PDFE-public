# -*- coding: utf-8 -*-
"""Prove the GitHub #210 form-fixture generators reproduce their fixtures.

    python tests/fixtures/verify_form_fixture_generators.py
    python tests/fixtures/verify_form_fixture_generators.py --self-test

Exit 0 = every generator reproduces its committed fixture's field inventory.
Exit 1 = at least one diverged (the divergence is printed field-by-field).
Exit 2 = usage / environment error.

WHAT IT CHECKS AND WHY THAT IS THE RIGHT CHECK

phase-61 refused to write these generators for a good reason: several suites
assert against these fixtures' exact field sets, and a subtly-wrong generator
converts a KNOWN regeneration gap into SILENT fixture drift. A generator with no
oracle would have re-created exactly that risk under the banner of fixing it.

So this runs each generator into a temp directory and compares the result to the
COMMITTED binary over the canonical field inventory (see
`form_field_inventory.py` for what is in it and what is deliberately not).

It does NOT compare bytes. A faithful regeneration differs in object numbering,
stream filters and the producer string, so a byte diff reports a difference for
every correct run and can therefore never distinguish drift from noise.

IT ALSO CHECKS THE TRANSITIVE HALF, which is the part register row A5-20 missed:
`generate_form_with_dv.py` and `gen_f15_form_data_fdf.py` both consume
`test_form.pdf`. Running them against the REGENERATED file (not the committed
one) is the only thing that actually establishes the claim in GitHub #210 --
that a `git clean` of binary fixtures is now recoverable for all five, not
three. Pointing them at the committed file would prove nothing about recovery.
"""
from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parents[1]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from tests.fixtures import form_field_inventory as fi  # noqa: E402

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

# (generator script, fixture filename)
DIRECT = [
    ("generate_test_form.py", "test_form.pdf"),
    ("generate_test_form_js.py", "test_form_js.pdf"),
    ("generate_test_form_with_pushbutton.py", "test_form_with_pushbutton.pdf"),
]


def _run(cmd, cwd=None):
    proc = subprocess.run(cmd, cwd=cwd or str(REPO_ROOT),
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return proc.returncode, proc.stdout.decode("utf-8", "replace")


def check_direct(tmp):
    """Regenerate the three ungenerated fixtures and compare inventories."""
    failures = []
    for script, fixture in DIRECT:
        committed = HERE / fixture
        if not committed.exists():
            failures.append("%s: committed fixture is MISSING -- nothing to "
                            "compare against" % fixture)
            continue
        target = tmp / fixture
        rc, out = _run([sys.executable, str(HERE / script), str(target)])
        if rc != 0:
            failures.append("%s: generator exited %d\n%s" % (script, rc, out))
            continue
        if not target.exists():
            # A generator that ignores its output-path argument and writes over
            # the committed fixture instead would otherwise silently "pass" by
            # comparing the file to itself.
            failures.append("%s: wrote nothing to %s -- it is ignoring its "
                            "output-path argument" % (script, target))
            continue
        problems = fi.diff(fi.inventory(committed), fi.inventory(target))
        if problems:
            failures.append("%s: %d field-inventory difference(s) vs the "
                            "committed fixture:\n    %s"
                            % (fixture, len(problems),
                               "\n    ".join(problems)))
        else:
            print("  ok   %-36s reproduced from %s" % (fixture, script))
    return failures


def check_transitive(tmp):
    """Run the two dependent generators against the REGENERATED test_form.pdf.

    Both read `tests/fixtures/test_form.pdf` by a path relative to their own
    location, so the only way to feed them a regenerated input without touching
    the working tree is to run them inside a temp copy of tests/fixtures/.
    """
    failures = []
    regen = tmp / "test_form.pdf"
    if not regen.exists():
        return ["transitive check skipped: test_form.pdf was not regenerated"]

    sandbox = tmp / "fixtures_sandbox"
    sandbox.mkdir(exist_ok=True)
    for name in ("generate_form_with_dv.py", "gen_f15_form_data_fdf.py"):
        shutil.copy2(HERE / name, sandbox / name)
    shutil.copy2(regen, sandbox / "test_form.pdf")

    rc, out = _run([sys.executable, str(sandbox / "generate_form_with_dv.py")],
                   cwd=str(sandbox))
    produced = sandbox / "test_form_with_dv.pdf"
    if rc != 0 or not produced.exists():
        failures.append(
            "generate_form_with_dv.py could NOT rebuild test_form_with_dv.pdf "
            "from the regenerated test_form.pdf (rc=%d). This is the exact "
            "transitive breakage GitHub #210 describes.\n%s" % (rc, out))
    else:
        committed = HERE / "test_form_with_dv.pdf"
        if committed.exists():
            problems = fi.diff(fi.inventory(committed), fi.inventory(produced))
            if problems:
                failures.append(
                    "test_form_with_dv.pdf rebuilt from the regenerated input "
                    "differs from the committed one:\n    %s"
                    % "\n    ".join(problems))
            else:
                print("  ok   test_form_with_dv.pdf              rebuilt "
                      "transitively from the regenerated test_form.pdf")

    rc, out = _run([sys.executable, str(sandbox / "gen_f15_form_data_fdf.py")],
                   cwd=str(sandbox))
    fdf = sandbox / "f15_form_data.fdf"
    if rc != 0 or not fdf.exists():
        failures.append("gen_f15_form_data_fdf.py could not produce "
                        "f15_form_data.fdf (rc=%d)\n%s" % (rc, out))
    else:
        committed_fdf = HERE / "f15_form_data.fdf"
        if committed_fdf.exists():
            a = committed_fdf.read_bytes().replace(b"\r\n", b"\n")
            b = fdf.read_bytes().replace(b"\r\n", b"\n")
            # The FDF is hand-written ASCII with no object numbering or
            # producer string, so byte equality IS meaningful here -- unlike
            # for the PDFs. CRLF is normalised because core.autocrlf=true
            # rewrites the tracked copy on checkout.
            if a != b:
                failures.append(
                    "f15_form_data.fdf regenerated from the regenerated "
                    "test_form.pdf does not match the committed file")
            else:
                print("  ok   f15_form_data.fdf                   rebuilt "
                      "transitively from the regenerated test_form.pdf")
    return failures


# --------------------------------------------------------------------------
# Self-test. A guard that cannot go red is decoration. This one mutates a
# regenerated fixture and requires the comparison to notice -- the same shape
# of proof the phase asks for from every new test.
# --------------------------------------------------------------------------
def self_test():
    import pikepdf
    failures = 0
    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)
        target = tmp / "test_form.pdf"
        rc, out = _run([sys.executable, str(HERE / "generate_test_form.py"),
                        str(target)])
        if rc != 0:
            print("  FAIL generator exited %d\n%s" % (rc, out))
            return 1

        base = fi.inventory(HERE / "test_form.pdf")

        # Control: unmutated must compare equal, or the guard is a false alarm
        # generator and its reds mean nothing.
        if fi.diff(base, fi.inventory(target)):
            print("  FAIL control: an unmutated regeneration already differs")
            failures += 1
        else:
            print("  ok   control: unmutated regeneration compares equal")

        mutations = [
            ("drop a field", _mut_drop_field),
            ("change a widget rect", _mut_change_rect),
            ("change a field flag word", _mut_change_ff),
            ("drop /MK from FirstName", _mut_drop_mk),
            ("add a /CO calculation order", _mut_add_co),
        ]
        for label, mutate in mutations:
            mutated = tmp / ("mut_%s.pdf" % label.replace(" ", "_")
                             .replace("/", ""))
            shutil.copy2(target, mutated)
            with pikepdf.open(str(mutated), allow_overwriting_input=True) as p:
                mutate(p)
                p.save(str(mutated))
            problems = fi.diff(base, fi.inventory(mutated))
            if problems:
                print("  ok   mutation detected: %-28s (%d difference(s))"
                      % (label, len(problems)))
            else:
                print("  FAIL mutation NOT detected: %s" % label)
                failures += 1

    print("self-test: %s" % ("PASS" if failures == 0 else
                             "FAIL (%d)" % failures))
    return 1 if failures else 0


def _named(pdf, name):
    import pikepdf
    for f in pdf.Root.AcroForm.Fields:
        if "/T" in f and str(f["/T"]) == name:
            return f
    raise KeyError(name)


def _mut_drop_field(pdf):
    import pikepdf
    fields = list(pdf.Root.AcroForm.Fields)
    pdf.Root.AcroForm[pikepdf.Name("/Fields")] = pikepdf.Array(fields[1:])


def _mut_change_rect(pdf):
    import pikepdf
    f = _named(pdf, "FirstName")
    f[pikepdf.Name("/Rect")] = pikepdf.Array([180, 691, 380, 711])


def _mut_change_ff(pdf):
    import pikepdf
    _named(pdf, "Comments")[pikepdf.Name("/Ff")] = 0


def _mut_drop_mk(pdf):
    import pikepdf
    del _named(pdf, "FirstName")[pikepdf.Name("/MK")]


def _mut_add_co(pdf):
    import pikepdf
    pdf.Root.AcroForm[pikepdf.Name("/CO")] = pikepdf.Array(
        [_named(pdf, "FirstName")])


def main(argv):
    ap = argparse.ArgumentParser(
        description="Verify the GitHub #210 form-fixture generators reproduce "
                    "their committed fixtures field-for-field.")
    ap.add_argument("--self-test", action="store_true",
                    help="mutate a regenerated fixture and require detection")
    args = ap.parse_args(argv)

    if args.self_test:
        return self_test()

    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)
        failures = check_direct(tmp)
        failures += check_transitive(tmp)

    if failures:
        print("\nverify_form_fixture_generators: FAIL")
        for f in failures:
            print("  - %s" % f)
        return 1
    print("verify_form_fixture_generators: PASS -- all five fixtures are "
          "reproducible from tracked generators")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
