# -*- coding: utf-8 -*-
"""Exercise Set-R2Config.ps1's decision logic against a MOCK aws CLI.

WHY THIS EXISTS. An ad-hoc karen review found that Set-R2Config.ps1 was the only
script in this package with no harness at all -- `Install-PdfeRunner.ps1` has
Test-InstallerDryRun.py, the pinned installers have Test-InstallerUrls.ps1 and
its mutation suite, and this had nothing. Its most important behaviour is a
BRANCH, not a value: read-only is CORRECT on a consume machine and a FAILURE on
a build machine, and getting that backwards pushes operators toward giving every
machine write access -- which is the exact outcome the read-only rule exists to
prevent. A branch nobody tests is a branch that flips silently.

HOW IT AVOIDS NEEDING A REAL BUCKET. `aws` is shimmed: a .cmd on PATH ahead of
any real client, scripted per-subcommand to return chosen exit codes. That makes
"token can read but not write" expressible without provisioning anything, which
is the whole point -- the case that matters most is the one hardest to arrange
for real.

EVERY RUN USES -WhatIf, so the machine environment is never touched. The script
sets nothing until validation passes, and this harness must not be the thing
that discovers otherwise.

Run (Windows only):
    python tools/runner-deployment/Test-SetR2Config.py
"""
from __future__ import annotations

import io
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPT = os.path.join(HERE, "Set-R2Config.ps1")

# Exit codes the shim returns per subcommand. `head-bucket` is reachability,
# `cp` is write-or-read depending on argument order, `list-objects-v2` is the
# read fallback used when a write was refused.


def make_shim(d: str, head: int, put: int, get: int, list_: int) -> str:
    """Write aws.cmd into d and return d (to be prepended to PATH)."""
    # A GET must actually PRODUCE THE DESTINATION FILE, not merely exit 0.
    #
    # The script verifies the round trip by reading the downloaded file back and
    # comparing its contents -- which is the correct thing for it to do, and
    # exactly what a shim that only fakes exit codes fails to satisfy. The first
    # version of this mock returned 0 without writing anything, so every
    # read-back check failed and five cases reported a defect that was in the
    # TEST. A mock has to emulate the OBSERVABLE behaviour the code under test
    # actually inspects, or it tests the mock.
    # FLAT, WITH GOTO LABELS AND BARE `exit`. Not a style preference -- measured.
    #
    # The first version nested the `cp` branch two blocks deep and used
    # `exit /b N`. `head-bucket` (ONE level of nesting) propagated its code
    # correctly; `cp` (TWO levels) always returned 0, so a "read-only token"
    # could not be expressed at all and three cases reported a defect in the
    # SCRIPT that was really in the MOCK.
    #
    # `flat + goto + bare exit` was measured against three alternatives and is
    # the shape that reliably carries a non-zero code out through PATH
    # resolution. Bare `exit` ends cmd.exe with the code rather than merely
    # returning from the batch, which removes the nesting question entirely.
    body = ("@echo off\r\n"
            "setlocal enabledelayedexpansion\r\n"
            "if \"%1\"==\"--version\" goto version\r\n"
            "if \"%1\"==\"s3api\" goto s3api\r\n"
            "if \"%1\"==\"s3\" goto s3\r\n"
            "exit 0\r\n"
            "\r\n"
            ":version\r\n"
            "echo aws-cli/2.28.10 mock\r\n"
            "exit 0\r\n"
            "\r\n"
            ":s3api\r\n"
            "if \"%2\"==\"head-bucket\" exit {head}\r\n"
            "if \"%2\"==\"list-objects-v2\" exit {list}\r\n"
            "exit 0\r\n"
            "\r\n"
            ":s3\r\n"
            "if \"%2\"==\"rm\" exit 0\r\n"
            "if not \"%2\"==\"cp\" exit 0\r\n"
            # `cp <local> s3://...` is a PUT; `cp s3://... <local>` is a GET.
            "set \"SRC=%~3\"\r\n"
            "if \"!SRC:~0,5!\"==\"s3://\" goto get\r\n"
            "exit {put}\r\n"
            "\r\n"
            ":get\r\n"
            # A GET must PRODUCE the destination file, not merely exit 0 -- the
            # script reads it back and compares contents, which is the right
            # thing for it to do and what an exit-code-only mock fails.
            "if not {get}==0 exit {get}\r\n"
            ">%4 echo pdfe r2 preflight\r\n"
            "exit 0\r\n").format(head=head, put=put, get=get, list=list_)
    with io.open(os.path.join(d, "aws.cmd"), "w", encoding="ascii", newline="") as fh:
        fh.write(body)
    return d


def run(envdir: str, shimdir: str, *args) -> tuple[int, str]:
    env = dict(os.environ)
    env["PATH"] = shimdir + os.pathsep + env.get("PATH", "")
    cmd = ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass",
           "-File", SCRIPT,
           "-EnvFile", os.path.join(envdir, ".env"),
           "-WhatIf"] + list(args)
    p = subprocess.run(cmd, capture_output=True, text=True,
                       encoding="utf-8", errors="replace", env=env, cwd=envdir)
    return p.returncode, (p.stdout or "") + (p.stderr or "")


def write_env(d: str, **kw) -> None:
    lines = ["PDFE_R2_ACCOUNT_ID=acct123",
             "PDFE_R2_BUCKET=pdfe-build-store",
             "PDFE_R2_ACCESS_KEY_ID=AKIDEXAMPLE",
             "PDFE_R2_SECRET_ACCESS_KEY=secretexample"]
    for k, v in kw.items():
        lines = [l for l in lines if not l.startswith(k + "=")]
        if v is not None:
            lines.append("%s=%s" % (k, v))
    with io.open(os.path.join(d, ".env"), "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(lines) + "\n")


def main() -> int:
    if os.name != "nt":
        # Set-R2Config.ps1 writes MACHINE-scope environment variables and shells
        # out to a .cmd shim. Off-Windows every case would "pass" for the wrong
        # reason -- the same fake-green this project has been bitten by before.
        print("[set-r2] REFUSING to run on %s: this exercises Windows-only "
              "behaviour, so any result here would be meaningless." % os.name,
              file=sys.stderr)
        return 2

    results: list[tuple[str, bool, str]] = []

    def check(label, ok, detail=""):
        results.append((label, ok, detail))

    tmp = tempfile.mkdtemp(prefix="pdfe-setr2-")
    try:
        envdir = os.path.join(tmp, "cfg")
        os.makedirs(envdir)
        write_env(envdir)

        # --- the branch that matters most -------------------------------
        # read+write, build profile -> correct
        d = make_shim(os.path.join(tmp, "s_rw"), 0, 0, 0, 0) \
            if os.makedirs(os.path.join(tmp, "s_rw"), exist_ok=True) is None else None
        rc, out = run(envdir, os.path.join(tmp, "s_rw"), "-RunnerProfile", "build")
        check("build + read/write token -> accepted",
              rc == 0 and "correct for a BUILD machine" in out, out[-400:])

        # read-only, consume profile -> correct (PUT refused, list works)
        os.makedirs(os.path.join(tmp, "s_ro"), exist_ok=True)
        make_shim(os.path.join(tmp, "s_ro"), 0, 1, 0, 0)
        rc, out = run(envdir, os.path.join(tmp, "s_ro"), "-RunnerProfile", "consume")
        check("consume + READ-ONLY token -> accepted as correct",
              rc == 0 and "correct for a CONSUME machine" in out, out[-400:])

        # read-only, BUILD profile -> failure (a builder must publish)
        rc, out = run(envdir, os.path.join(tmp, "s_ro"), "-RunnerProfile", "build")
        check("build + read-only token -> REFUSED (a builder must publish)",
              rc != 0 and "must be able to WRITE" in out, out[-400:])

        # read+write, CONSUME profile -> warned, not silently accepted
        rc, out = run(envdir, os.path.join(tmp, "s_rw"), "-RunnerProfile", "consume")
        check("consume + WRITE token -> warned, not silently accepted",
              rc != 0 and "should hold a READ-ONLY token" in out, out[-400:])

        # ...and -Force overrides that warning deliberately
        rc, out = run(envdir, os.path.join(tmp, "s_rw"),
                      "-RunnerProfile", "consume", "-Force")
        check("consume + WRITE token + -Force -> allowed deliberately",
              rc == 0, out[-300:])

        # --- unreachable bucket -----------------------------------------
        os.makedirs(os.path.join(tmp, "s_dead"), exist_ok=True)
        make_shim(os.path.join(tmp, "s_dead"), 1, 1, 1, 1)
        rc, out = run(envdir, os.path.join(tmp, "s_dead"), "-RunnerProfile", "build")
        check("unreachable bucket -> refused",
              rc != 0 and "could not be reached" in out, out[-300:])

        # --- .env plumbing ----------------------------------------------
        rc, out = run(envdir, os.path.join(tmp, "s_rw"), "-RunnerProfile", "build")
        check(".env values are actually used (bucket from file)",
              "pdfe-build-store" in out, out[-300:])
        check("endpoint derived from PDFE_R2_ACCOUNT_ID",
              "https://acct123.r2.cloudflarestorage.com" in out, out[-300:])
        check("secret from .env is NEVER echoed",
              "secretexample" not in out, "secret leaked into output")

        rc, out = run(envdir, os.path.join(tmp, "s_rw"),
                      "-RunnerProfile", "build", "-Bucket", "overridden-bucket")
        check("explicit -Bucket OVERRIDES the .env value",
              "overridden-bucket" in out and "bucket   : pdfe-build-store" not in out,
              out[-300:])

        # --- refusals before any network call ---------------------------
        rc, out = run(envdir, os.path.join(tmp, "s_rw"),
                      "-Endpoint", "https://x.r2.cloudflarestorage.com/pdfe-build-store",
                      "-RunnerProfile", "build")
        check("endpoint with the bucket appended -> refused",
              rc != 0 and "ends with the bucket name" in out, out[-300:])

        rc, out = run(envdir, os.path.join(tmp, "s_rw"),
                      "-Endpoint", "http://insecure.example", "-RunnerProfile", "build")
        check("non-https endpoint -> refused",
              rc != 0 and "must be https" in out, out[-300:])

        # missing required value
        bare = os.path.join(tmp, "bare")
        os.makedirs(bare, exist_ok=True)
        write_env(bare, PDFE_R2_ACCESS_KEY_ID=None)
        rc, out = run(bare, os.path.join(tmp, "s_rw"), "-RunnerProfile", "build")
        check("missing AccessKeyId -> refused with the key name",
              rc != 0 and "PDFE_R2_ACCESS_KEY_ID" in out, out[-300:])

        # --- nothing is persisted under -WhatIf -------------------------
        # Every case above ran with -WhatIf. If the script wrote anyway, the
        # machine variables would now be set -- which is the one failure this
        # harness must never cause and must therefore check.
        probe = subprocess.run(
            ["powershell", "-NoProfile", "-Command",
             "[Environment]::GetEnvironmentVariable('PDFE_R2_BUCKET','Machine')"],
            capture_output=True, text=True, encoding="utf-8", errors="replace")
        check("-WhatIf wrote NOTHING to the machine environment",
              not (probe.stdout or "").strip(),
              "PDFE_R2_BUCKET is now set at machine scope: %r" % probe.stdout)

    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    print("")
    failed = 0
    for label, ok, detail in results:
        print("  %-58s %s" % (label, "PASS" if ok else "FAIL"))
        if not ok:
            failed += 1
            print("      " + str(detail).replace("\n", "\n      "))
    print("")
    if failed:
        print("RESULT: %d of %d FAILED" % (failed, len(results)))
        return 1
    print("RESULT: all %d checks pass" % len(results))
    return 0


if __name__ == "__main__":
    sys.exit(main())
