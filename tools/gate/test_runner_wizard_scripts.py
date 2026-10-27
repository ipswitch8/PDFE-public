#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Coverage for the three runner-deployment wizard scripts.

WHY THIS EXISTS
---------------
Deploy-PdfeRunner.ps1, Register-PdfeRunner.ps1 and Start-PdfeRunner.ps1 were
written, committed, and referenced from the README as the supported way to stand
up a runner -- and NOTHING executed them. Not a test, not a CI job. An ad-hoc
review put it as "exactly no coverage", which was accurate; only
check_label_drift.py even mentioned them, and that reads them as text.

Two defect classes had already shipped through that hole:

  * 8 Join-Path calls on parameters defaulting to drive-qualified paths, across
    all three files. Join-Path RESOLVES its drive; the rule is
    [IO.Path]::Combine. A check for exactly this existed and ran in CI, and I
    shipped it red twice without noticing.
  * Install-PdfeRunner.ps1 would not PARSE under PowerShell 5.1 -- BOM-less
    UTF-8 with non-ASCII inside string literals, which 5.1 decodes as the ANSI
    codepage. A script that does not parse is a deployment that cannot start,
    and nothing would have caught it before an operator hit it on a new host.

So: parse every one of them with the real parser, and actually RUN each one down
its -WhatIf path, which is the only route that touches control flow without
provisioning a machine.

WHAT -WhatIf DOES AND DOES NOT PROVE
------------------------------------
It reaches argument resolution and the branch structure around each effectful
call. It does NOT prove the effectful calls work -- no runner is registered, no
token is exchanged, no service starts. Test-InstallerDryRun.py makes the
equivalent point about the installer, and learned the harder lesson there: a dry
run that reached 1 of 11 requirements still exited zero. Reaching the end is not
the same as doing the work.
"""
from __future__ import annotations

import io
import os
import shutil
import subprocess
import sys
import tempfile

import pytest

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
PKG = os.path.join(REPO, "tools", "runner-deployment")
WIZARDS = ["Deploy-PdfeRunner.ps1", "Register-PdfeRunner.ps1", "Start-PdfeRunner.ps1"]

pytestmark = pytest.mark.skipif(
    os.name != "nt", reason="PowerShell deployment scripts; nothing to exercise elsewhere"
)


def _powershell() -> str:
    # powershell.exe (5.1) SPECIFICALLY, not pwsh. 5.1 is what ships on a fresh
    # Windows host, it is what the Desktop shortcut invokes, and it is the one
    # with the ANSI-codepage decoding behaviour that broke Install-PdfeRunner.
    # Testing under pwsh 7 would have missed that bug entirely.
    p = shutil.which("powershell.exe")
    if not p:
        pytest.skip("powershell.exe (5.1) not on PATH")
    return p


@pytest.mark.parametrize("name", WIZARDS)
def test_wizard_parses_under_windows_powershell(name: str):
    """ParseFile, from a FILE, under 5.1.

    Not ParseInput on a string this process decoded -- that reads the bytes with
    Python's idea of the encoding and gives a FALSE GREEN for exactly the
    BOM-less case being guarded against. Measured: the string route reported 0
    errors for a file the file route rejected."""
    ps = _powershell()
    path = os.path.join(PKG, name)
    probe = (
        "$e=$null; $t=$null; "
        "$null=[System.Management.Automation.Language.Parser]::ParseFile("
        "'%s',[ref]$t,[ref]$e); "
        "if(@($e).Count){ @($e)[0].ToString(); exit 1 } else { exit 0 }"
        % path.replace("'", "''")
    )
    p = subprocess.run([ps, "-NoProfile", "-Command", probe],
                       capture_output=True, text=True, timeout=120)
    assert p.returncode == 0, "%s does not parse under PS 5.1:\n%s" % (name, p.stdout + p.stderr)


@pytest.mark.parametrize("name", WIZARDS)
def test_wizard_has_a_bom(name: str):
    """The fix for the 5.1 decoding class, pinned so it cannot silently regress.

    A parse test alone would not catch it: whether BOM-less non-ASCII is fatal
    depends on WHERE it sits -- a mis-decoded comment is harmless, a mis-decoded
    string literal breaks the quoting. That makes the parse test pass or fail on
    an incidental property of where somebody put an em dash. The BOM removes the
    question."""
    with open(os.path.join(PKG, name), "rb") as fh:
        assert fh.read(3) == b"\xef\xbb\xbf", (
            "%s has no UTF-8 BOM; PS 5.1 will decode it as the ANSI codepage and "
            "any non-ASCII inside a string literal becomes a parse error on a "
            "fresh host" % name)


@pytest.mark.parametrize("name", WIZARDS)
def test_wizard_runs_its_whatif_path_without_erroring(name: str):
    """The part that was 'exactly no coverage': actually invoke them."""
    ps = _powershell()
    with tempfile.TemporaryDirectory() as d:
        cmd = [ps, "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
               os.path.join(PKG, name), "-WhatIf"]
        if name != "Deploy-PdfeRunner.ps1":
            cmd += ["-RunnerDir", os.path.join(d, "runner")]
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=300,
                           stdin=subprocess.DEVNULL, cwd=d)
    out = p.stdout + p.stderr
    # A -WhatIf run must not throw. It MAY exit non-zero on a host that is not a
    # runner (nothing to start, no registration present), so the exit code is
    # not the oracle -- an unhandled exception is.
    for bad in ("Unable to find type", "is not recognized as the name of a cmdlet",
                "CommandNotFoundException", "ParameterBindingException",
                "MethodInvocationException", "You cannot call a method on a null"):
        assert bad not in out, "%s -WhatIf hit %r:\n%s" % (name, bad, out[-3000:])


def test_whatif_never_writes_a_token_or_a_shortcut():
    """-WhatIf must be inert. Registration tokens are single-use and must never
    be persisted at all; a shortcut written by a preview would be a real change
    from a run that promised none."""
    ps = _powershell()
    with tempfile.TemporaryDirectory() as d:
        before = set()
        for root, _, files in os.walk(d):
            before.update(os.path.join(root, f) for f in files)
        subprocess.run([ps, "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                        os.path.join(PKG, "Start-PdfeRunner.ps1"),
                        "-WhatIf", "-CreateShortcut",
                        "-RunnerDir", os.path.join(d, "runner")],
                       capture_output=True, text=True, timeout=300,
                       stdin=subprocess.DEVNULL, cwd=d)
        after = set()
        for root, _, files in os.walk(d):
            after.update(os.path.join(root, f) for f in files)
    assert after == before, "-WhatIf -CreateShortcut created files: %s" % (after - before)


def test_only_the_one_annotated_site_puts_a_token_on_a_command_line():
    """`--token $x` puts the token in the process table, where any local user can
    read it with Get-CimInstance Win32_Process.

    This does NOT assert zero. There is exactly one site, and it is unavoidable:
    the GitHub runner configurator has no stdin path for the token, so
    registering means passing it as an argument. Asserting zero would redden
    correct code, which gets a guard muted and then it is not there for the case
    that matters.

    So the rule is: one site, and it must carry the TOKEN-ON-CMDLINE annotation
    explaining itself. A new unannotated site reddens. The token there is
    single-use, redacted in the echoed command, and cleared in a finally."""
    # Match an ACTUAL expansion -- `--token $something` -- not the substring
    # "--token". The first version of this test flagged five lines that were all
    # operator instructions and redacted display text ("--token <REMOVAL-TOKEN>",
    # "--token <REDACTED>"). A guard that reddens on correct code is worse than
    # no guard: it gets muted, and then it is not there for the real case.
    import re
    pat = re.compile(r"--token\s+[\"']?\$")
    offenders, allowed = [], []
    for name in WIZARDS:
        src = io.open(os.path.join(PKG, name), encoding="utf-8-sig").read()
        for i, line in enumerate(src.splitlines(), 1):
            s = line.strip()
            if s.startswith("#") or s.startswith(".") or "<REDACTED>" in s:
                continue
            if pat.search(s):
                # Annotated? The marker must be in the 10 lines above it, so it
                # sits with the call rather than anywhere in the file.
                window = src.splitlines()[max(0, i - 11):i - 1]
                if any("TOKEN-ON-CMDLINE" in w for w in window):
                    allowed.append("%s:%d" % (name, i))
                else:
                    offenders.append("%s:%d: %s" % (name, i, s))
    assert not offenders, (
        "token expanded on a command line with no TOKEN-ON-CMDLINE "
        "justification:\n" + "\n".join(offenders))
    assert len(allowed) == 1, (
        "expected exactly one annotated token-on-cmdline site (the runner "
        "configurator, which has no stdin path); found %d: %s. If a second is "
        "genuinely needed, widen this deliberately -- do not let the count "
        "drift." % (len(allowed), allowed))


if __name__ == "__main__":
    sys.exit(pytest.main([__file__, "-q"]))
