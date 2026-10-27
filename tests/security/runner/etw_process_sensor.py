#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""etw_process_sensor.py -- opt-in ETW sensor for process creation (GitHub #62).

WHAT GAP THIS CLOSES

`tests/security_oracles.h` carries a job-object-scoped spawn monitor. It is
good, and it states its own irreducible limit:

    "a spawn brokered ENTIRELY outside our job (created by a different
     process -- e.g. an RPC/DCOM broker -- so it never joins our job) cannot
     be attributed without reintroducing machine-wide false positives."

That residual is real. A PDF exploit that reaches a broker (WMI, DCOM, the
task scheduler) gets its process created by the BROKER, not by us, so the new
process never joins our job object and the job monitor cannot see it. Widening
the job monitor to the whole machine would trade a false negative for constant
false positives on a developer's or CI machine.

An ETW `Microsoft-Windows-Kernel-Process` session filtered on PARENT IMAGE is
the right instrument: the kernel reports every process creation machine-wide
together with its parent, so a brokered spawn is attributable by asking "which
image was the parent" rather than "did it land in my job".

WHY THIS IS OPT-IN AND NOT IN THE DEFAULT SUITE

Enabling the kernel-process provider requires ADMINISTRATOR rights. The default
GoogleTest suite must stay runnable unelevated and broker-agnostic, so this
lives here as its own lane rather than as a gtest.

    Measured 2026-09-06: the development account is NOT elevated
    (`IsInRole(Administrator)` -> False), so the end-to-end capture below has
    NOT been executed in this environment. Its decision logic HAS been tested
    (`--self-test`), and the capture procedure is documented and runnable by
    anyone with an elevated shell.

    That elevation requirement means GitHub #62 is not "individually workable"
    in the sense the 2026-09-06 triage assumed -- verification needs a
    privilege the triage's four factors did not model. Recorded rather than
    papered over.

CORRECTION, MEASURED 2026-09-14 (p85-remaining-open phase 2)
------------------------------------------------------------
The sentence above used to read "starting a kernel ETW SESSION requires
administrator rights". **That is not what the privilege boundary is**, and the
imprecision hid a false-green path. Measured on this host, unelevated, with the
account a member of `Performance Log Users`:

    logman create trace PDFE-ProbeSensor -ets \
        -p Microsoft-Windows-Kernel-Process 0x10 0x5 -o probe.etl ...

      -> prints "Error: Access is denied. Try running this command as an
         administrator." and returns 0x80070005 (-2147024891)
      -> BUT `logman query PDFE-ProbeSensor -ets` then reports the session
         **Status: Running**, with buffers written and a 1 MiB .etl on disk.

So the SESSION is created fine unelevated (`Performance Log Users` grants that);
it is **enabling the kernel provider on it** that is denied. The session is left
running with no provider attached. A control run confirmed the consequence:
spawning `whoami.exe` during such a session and decoding the result gave a CSV
of 3 lines with **0 `ParentImageName` events**, and `tracerpt` **exited 0**.

That is exactly the false green this lane exists to avoid: a blind sensor
decodes cleanly, attributes nothing, and "no spawn detected" is indistinguishable
from "the sensor saw nothing at all". Three things follow, all implemented below:

  1. `is_elevated()` stays as a fast pre-flight, but it is NOT the only gate --
     it answers the wrong question (admin role) rather than the real one
     (can the provider actually be enabled).
  2. A failed `logman create` now STOPS the orphaned session it leaves behind,
     because the measured behaviour is failure-with-a-running-session.
  3. A decode that yields **zero process events of any kind** is reported as
     exit 2 (sensor did not run), never exit 0. On a live Windows machine a
     multi-second machine-wide kernel-process trace always observes some
     process activity; zero means blind, not quiet. This is the positive
     control -- see `capture_verdict()`, which is pure and self-tested.

USAGE

    # decision logic only -- needs no privilege, runs anywhere
    python tests/security/runner/etw_process_sensor.py --self-test

    # real capture -- REQUIRES AN ELEVATED SHELL
    python tests/security/runner/etw_process_sensor.py --capture \
        --parent-image pdfe.exe --seconds 30

Exit 0 = pass, 1 = a spawn was attributed (or self-test failure), 2 = the
sensor could not run (not elevated, tooling missing). Note 2 is DISTINCT from
0: "the sensor did not run" must never be reported as "nothing was spawned",
which is the false-green this whole lane exists to avoid.
"""
import argparse
import ctypes
import os
import re
import subprocess
import sys

SESSION_NAME = "PDFE-ProcSensor"
KERNEL_PROCESS_PROVIDER = "Microsoft-Windows-Kernel-Process"
# Keyword 0x10 = WINEVENT_KEYWORD_PROCESS on this provider.
PROCESS_KEYWORD = "0x10"


# --------------------------------------------------------------------------
# Decision core -- PURE, so it is testable with no privilege and no capture.
# --------------------------------------------------------------------------

def parse_process_events(text):
    """Extract (parent_image, child_image) pairs from a decoded ETW dump.

    Accepts the shape `tracerpt`/`wevtutil` produce once rendered to text.
    Deliberately tolerant about surrounding noise and strict about the two
    fields that matter.
    """
    events = []
    # Fields are delimited by commas/semicolons/end-of-line -- NOT by spaces.
    # A path like "C:\Program Files\PDFE\pdfe.exe" contains a space, so a
    # non-whitespace class truncates it to "C:\Program" and every attribution
    # against an installed build silently misses. The self-test below caught
    # exactly that, which is the reason the class is written this way.
    #
    # Matched per LINE rather than with DOTALL: a dot-all `.*?` between the two
    # fields will happily pair one event's child with a LATER event's parent
    # when a line is missing one of them.
    child_pat = re.compile(r"\bImageName\s*[=:]\s*(?P<v>[^,;\r\n]+)",
                           re.IGNORECASE)
    parent_pat = re.compile(r"\bParentImageName\s*[=:]\s*(?P<v>[^,;\r\n]+)",
                            re.IGNORECASE)

    for line in text.splitlines():
        # ParentImageName also ends in "ImageName", so find the parent first
        # and remove it before looking for the child.
        pm = parent_pat.search(line)
        if not pm:
            continue
        remainder = line[:pm.start()] + line[pm.end():]
        cm = child_pat.search(remainder)
        if not cm:
            continue
        child = os.path.basename(cm.group("v").strip().strip('"'))
        parent = os.path.basename(pm.group("v").strip().strip('"'))
        if not child or not parent:
            continue
        events.append((parent.lower(), child.lower()))
    return events


def attributed_spawns(events, parent_image, ignore=()):
    """Child images created BY `parent_image`, excluding known-benign ones.

    This is the judgement the lane exists to make. Kept separate from capture
    so it can be tested against synthetic input.
    """
    parent_image = os.path.basename(parent_image).lower()
    ignored = {os.path.basename(i).lower() for i in ignore}
    return [child for (parent, child) in events
            if parent == parent_image and child not in ignored]


def capture_verdict(events, parent_image, ignore=()):
    """Turn a decoded capture into (exit_code, message). PURE -- see self-test.

    This is the whole point of the 2026-09-14 correction. It exists as a
    separate pure function, rather than inline in `capture()`, so the
    zero-events rule can be tested with no privilege and no ETW session.

    Exit codes, and why the middle one is not 0:

      0 -- the sensor ran, saw process activity, and attributed none of it to
           `parent_image`. A real, earned negative.
      1 -- the sensor ran and attributed at least one spawn. A finding.
      2 -- the sensor DID NOT RUN USEFULLY. Specifically: the decode contained
           zero process-creation events of any kind. Machine-wide, over a
           multi-second window, a live Windows box always creates some process
           (the measured control run is in this module's docstring). Zero
           therefore means the provider was never enabled -- the denied-but-
           session-still-running case -- and NOT "nothing was spawned".

    Collapsing 2 into 0 is the single defect that would make this lane worse
    than useless: it would report a blind sensor as a clean bill of health.
    """
    if not events:
        return 2, ("etw_process_sensor: DECODED ZERO PROCESS EVENTS. The "
                   "kernel-process provider was almost certainly never enabled "
                   "(unelevated `logman create` is denied the provider while "
                   "still leaving the session running). Exiting 2 (sensor did "
                   "not run) -- deliberately NOT 0, because a blind sensor must "
                   "never be reported as 'nothing was spawned'.")

    spawned = attributed_spawns(events, parent_image, ignore=ignore)
    msg = ("etw_process_sensor: %d process event(s) seen; %d attributed to %s"
           % (len(events), len(spawned), parent_image))
    if spawned:
        for c in sorted(set(spawned)):
            msg += "\n  SPAWNED BY %s: %s" % (parent_image, c)
        return 1, msg
    return 0, msg


# --------------------------------------------------------------------------
# Capture -- requires elevation.
# --------------------------------------------------------------------------

def is_elevated():
    try:
        return bool(ctypes.windll.shell32.IsUserAnAdmin())
    except Exception:
        return False


def _run(cmd):
    return subprocess.run(cmd, shell=False, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, text=True)


def capture(parent_image, seconds, etl_path):
    """Start a kernel-process ETW session, wait, stop, and decode it."""
    if os.name != "nt":
        print("etw_process_sensor: Windows only", file=sys.stderr)
        return 2
    if not is_elevated():
        print("etw_process_sensor: NOT ELEVATED -- enabling the "
              "Microsoft-Windows-Kernel-Process provider needs administrator "
              "rights. (The SESSION itself can be created unelevated by a member "
              "of 'Performance Log Users'; the provider enable is what is denied, "
              "and it fails while LEAVING THE SESSION RUNNING with no provider "
              "attached -- measured 2026-09-14, see the module docstring.) "
              "Exiting 2 (sensor did not run), which is deliberately NOT 0: "
              "'the sensor could not run' must never be reported as 'nothing "
              "was spawned'.", file=sys.stderr)
        return 2

    _run(["logman", "stop", SESSION_NAME, "-ets"])  # best effort cleanup

    start = _run(["logman", "create", "trace", SESSION_NAME, "-ets",
                  "-p", KERNEL_PROCESS_PROVIDER, PROCESS_KEYWORD, "0x5",
                  "-o", etl_path, "-nb", "16", "16", "-bs", "1024"])
    if start.returncode != 0:
        # MEASURED 2026-09-14: this path does NOT mean "no session exists".
        # Unelevated, `logman create` is denied the PROVIDER but still leaves
        # the session Running (query reports Status: Running, buffers written).
        # Returning without stopping it orphans a live ETW session on the
        # machine, so clean up before bailing out.
        _run(["logman", "stop", SESSION_NAME, "-ets"])
        print("etw_process_sensor: could not start session (any orphaned "
              "session has been stopped):\n" + start.stdout, file=sys.stderr)
        return 2

    print("etw_process_sensor: session %s running for %ds; exercise the "
          "product now" % (SESSION_NAME, seconds))
    try:
        subprocess.run(["timeout", "/t", str(seconds), "/nobreak"],
                       stdout=subprocess.DEVNULL)
    except Exception:
        pass
    finally:
        _run(["logman", "stop", SESSION_NAME, "-ets"])

    decoded = etl_path + ".txt"
    rpt = _run(["tracerpt", etl_path, "-o", decoded, "-of", "CSV", "-y"])
    if rpt.returncode != 0 or not os.path.exists(decoded):
        print("etw_process_sensor: decode failed:\n" + rpt.stdout,
              file=sys.stderr)
        return 2

    with open(decoded, encoding="utf-8", errors="replace") as fh:
        events = parse_process_events(fh.read())

    # NOTE: `tracerpt` exits 0 on a provider-less capture (measured), so its
    # return code above cannot distinguish a real trace from a blind one. The
    # zero-events positive control in capture_verdict() is what does that.
    code, msg = capture_verdict(events, parent_image,
                               ignore=["pdfe_preflight.exe"])
    print(msg, file=sys.stderr if code == 2 else sys.stdout)
    return code


# --------------------------------------------------------------------------
# Self-tests -- the sensor must be able to say YES and to say NO.
# --------------------------------------------------------------------------
SAMPLE = """
ImageName=C:\\Windows\\System32\\cmd.exe, ParentImageName=C:\\Program Files\\PDFE\\pdfe.exe
ImageName=C:\\Program Files\\PDFE\\pdfe_preflight.exe, ParentImageName=C:\\Program Files\\PDFE\\pdfe.exe
ImageName=C:\\Windows\\System32\\notepad.exe, ParentImageName=C:\\Windows\\explorer.exe
"""


def self_test():
    failures = 0

    def check(label, got, want):
        nonlocal failures
        ok = got == want
        if not ok:
            failures += 1
        print("  [%s] %-52s got=%r" % ("ok  " if ok else "FAIL", label, got))

    events = parse_process_events(SAMPLE)
    check("parses all three events", len(events), 3)

    # The positive case: cmd.exe brokered under pdfe.exe MUST be attributed.
    check("attributes a hostile child",
          attributed_spawns(events, "pdfe.exe", ignore=["pdfe_preflight.exe"]),
          ["cmd.exe"])

    # The benign case: the preflight helper is ours and must not redden.
    check("ignores the known-good helper",
          "pdfe_preflight.exe" in attributed_spawns(
              events, "pdfe.exe", ignore=["pdfe_preflight.exe"]),
          False)

    # A different parent's child is not ours.
    check("does not attribute another parent's child",
          attributed_spawns(events, "explorer.exe"), ["notepad.exe"])

    # And it must be able to say NO -- a guard that can only redden is as
    # useless as one that can only stay green.
    check("clean capture attributes nothing",
          attributed_spawns(parse_process_events(""), "pdfe.exe"), [])

    # ---- capture_verdict: the 2026-09-14 zero-events positive control ------
    # THE NEGATIVE CASE FIRST. An EMPTY decode must be exit 2 (sensor did not
    # run), never exit 0. This is the measured false-green: unelevated, the
    # provider enable is denied but the session still runs, tracerpt exits 0,
    # and the decode contains no process events at all. If this assertion ever
    # reads 0, a blind sensor is being reported as a clean bill of health.
    check("EMPTY decode is 'did not run' (2), not 'clean' (0)",
          capture_verdict([], "pdfe.exe")[0], 2)
    check("empty-decode message names the blindness",
          "ZERO PROCESS EVENTS" in capture_verdict([], "pdfe.exe")[1], True)

    # A capture that DID see activity, none of it ours, is an earned 0. Without
    # this case the rule above could be satisfied by always returning 2.
    check("populated decode with no attribution is a real pass (0)",
          capture_verdict(parse_process_events(SAMPLE), "pdfe.exe",
                          ignore=["cmd.exe", "pdfe_preflight.exe"])[0], 0)

    # And it must still be able to say YES.
    check("populated decode with an attributed spawn is a finding (1)",
          capture_verdict(parse_process_events(SAMPLE), "pdfe.exe",
                          ignore=["pdfe_preflight.exe"])[0], 1)

    print("self-test: %s" % ("PASS" if failures == 0 else "%d FAILURE(S)" % failures))
    return 1 if failures else 0


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--self-test", action="store_true",
                    help="test the decision logic; needs no privilege")
    ap.add_argument("--capture", action="store_true",
                    help="run a real ETW capture (REQUIRES ELEVATION)")
    ap.add_argument("--parent-image", default="pdfe.exe")
    ap.add_argument("--seconds", type=int, default=30)
    ap.add_argument("--etl", default=os.path.join(
        os.environ.get("TEMP", "."), "pdfe_proc_sensor.etl"))
    args = ap.parse_args(argv)

    if args.self_test:
        return self_test()
    if args.capture:
        return capture(args.parent_image, args.seconds, args.etl)

    ap.print_help()
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
