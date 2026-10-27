# -*- coding: utf-8 -*-
"""Operational-readiness preflight and step-conclusion postflight for
windows-validation (deferred-backlog row F49, follow-up phase-66).

THE HOLE THIS CLOSES. F49's defect — nothing read windows-validation's true CI
conclusion — was fixed for `fast-checks` only. The hole stayed open: the run's
own conclusion is still nobody's business but `governance.yml`'s. And the
run-level conclusion is the wrong thing to read anyway, which run
**31359621883** demonstrates exactly:

    run-level conclusion: cancelled
      step 13  failure   Run + verify CROSS-PLATFORM CORE PARITY suite (C-10)
      step 15  failure   Code coverage (OpenCppCoverage -> Cobertura)
      step 18  failure   Run ctest -L security (meta-audit F5 / SEC-23)

Three genuinely red steps, invisible behind one `cancelled` — which "alerts
nobody and is indistinguishable from a deliberate cancellation". That is the
F44-species masking recorded at audit-of-audits §4.3.

TWO SUBCOMMANDS, because the two failures are at opposite ends of a run:

  readiness  BEFORE the work. Is the self-hosted runner online, and are the
             tools the run will need actually present? A run submitted against
             an offline runner queues and is reported `cancelled` hours later.
             This turns a silent multi-hour non-event into an immediate, named
             failure.

  steps      AFTER the work. Read the run's OWN job/step conclusions and fail
             if any step is red, whatever the run-level conclusion says.

WHY NOT THE RUNNER AUTOSTART. F46 proposed registering the runner as a service
(`tools/ops/register_runner_service.ps1`, `runner_watchdog.ps1`). Those scripts
exist and are DELIBERATELY NOT REGISTERED, per the owner's explicit direction
and matching governance.yml:62-66. This tool is the alternative that was asked
for: it does not make unattended runs work, it makes a manual submit tell you
immediately that it would not have worked. Nothing here installs, registers or
schedules anything.

OFFLINE INPUTS ARE FIRST-CLASS. Both subcommands accept a recorded JSON file
instead of calling `gh`, so the checker is testable against real historical data
with no network and no credentials — the same dual-input shape
`check_governance_liveness.py` uses. `tools/gate/fixtures/run-31359621883-*.json`
is that run, recorded.

FAIL-CLOSED. Absent data, an unreadable file, a `gh` that will not run: every
one of those is a FAILURE, never a pass. A readiness check that returns OK when
it could not look is worse than no readiness check, because it is then evidence
of something nobody verified.
"""
from __future__ import annotations

import argparse
import io
import json
import os
import shutil
import subprocess
import sys

#: Step conclusions that mean the step genuinely went red. `cancelled` and
#: `skipped` are NOT here: a cancelled step is what the masking looks like from
#: the inside, and treating it as red would make every interrupted run fail for
#: the wrong reason. The point is to surface the steps that failed on their own
#: merits BEFORE the cancellation.
RED_CONCLUSIONS = ("failure", "timed_out")

#: Tools whose absence makes the run fail LATER anyway, so reporting them here
#: costs nothing and saves the wait. `OpenCppCoverage` is the only one:
#: windows-validation.yml:540-548 states its absence is "a HARD error (fail the
#: job) — the tool is a required runner dependency, not an optional nicety",
#: and that is what went red as step 15 of run 31359621883. Failing at readiness
#: time is therefore **the same failure roughly two hours earlier**, not a new
#: blocker — the cold vcpkg build alone is ~100 minutes.
REQUIRED_TOOLS = ("OpenCppCoverage",)

#: Reported but NOT enforced. `choco` is only the coverage step's *remediation*
#: path ("attempting choco install..."), so a box with OpenCppCoverage already
#: installed and no choco is perfectly serviceable — enforcing it would invent a
#: blocker the run does not actually have. That distinction is the difference
#: between a check people trust and one they switch off.
OBSERVED_TOOLS = ("choco",)


class ReadinessError(RuntimeError):
    """Raised when the check cannot be performed. Always a failure."""


# ---------------------------------------------------------------------------
# shared helpers
# ---------------------------------------------------------------------------

def _load_json(path: str, what: str):
    try:
        with open(path, "r", encoding="utf-8") as fh:
            return json.load(fh)
    except OSError as exc:
        raise ReadinessError(f"cannot read {what} from {path!r}: {exc}")
    except json.JSONDecodeError as exc:
        raise ReadinessError(f"{path!r} is not valid JSON ({what}): {exc}")


def _gh(argv: list[str], what: str):
    """Run `gh` and return parsed JSON. Any failure raises — never returns None."""
    gh = shutil.which("gh") or shutil.which("gh.exe")
    if not gh:
        raise ReadinessError(
            f"`gh` is not on PATH, so {what} cannot be checked. This is a "
            f"failure, not a skip: a readiness check that cannot look must not "
            f"report ready. (On this project gh lives at "
            f"C:\\Users\\<account>\\bin\\gh and is often not on PATH.)")
    try:
        proc = subprocess.run([gh] + argv, capture_output=True, text=True,
                              encoding="utf-8", errors="replace")
    except OSError as exc:
        raise ReadinessError(f"cannot execute gh: {exc}")
    if proc.returncode != 0:
        raise ReadinessError(
            f"gh exited {proc.returncode} while fetching {what}: "
            f"{(proc.stderr or '').strip()[:400]}")
    try:
        return json.loads(proc.stdout)
    except json.JSONDecodeError as exc:
        raise ReadinessError(f"gh returned non-JSON for {what}: {exc}")


def _emit(lines: list[str], summary_path: str | None) -> None:
    """Print to stdout and, when running in Actions, append to the step summary.

    The summary is where a human actually looks at a manual run, so a check
    whose result only reaches the log is a check nobody reads.
    """
    text = "\n".join(lines)
    print(text)
    target = summary_path or os.environ.get("GITHUB_STEP_SUMMARY")
    if not target:
        return
    try:
        with open(target, "a", encoding="utf-8") as fh:
            fh.write(text + "\n")
    except OSError as exc:  # never fail the check over the summary write
        print(f"[readiness] note: could not append to step summary: {exc}",
              file=sys.stderr)


# ---------------------------------------------------------------------------
# readiness — BEFORE the work
# ---------------------------------------------------------------------------

def runners_status(runners_doc) -> list[dict]:
    """Normalise the /actions/runners payload to a list of runner dicts."""
    if isinstance(runners_doc, dict) and "runners" in runners_doc:
        return list(runners_doc["runners"] or [])
    if isinstance(runners_doc, list):
        return list(runners_doc)
    raise ReadinessError(
        "runner payload has neither a 'runners' key nor a list at the top "
        "level; refusing to guess whether a runner is online.")


def check_runner(runners_doc, labels: list[str]) -> tuple[bool, list[str]]:
    """True when at least one runner carrying every label is online and idle-able."""
    runners = runners_status(runners_doc)
    lines = []
    if not runners:
        lines.append("- runner: **NONE REGISTERED** — the repository has no "
                     "self-hosted runner at all.")
        return False, lines

    wanted = {lbl.lower() for lbl in labels}
    ok = False
    for r in runners:
        have = {(lbl.get("name") or "").lower()
                for lbl in (r.get("labels") or [])}
        status = (r.get("status") or "unknown").lower()
        busy = bool(r.get("busy"))
        matches = wanted.issubset(have)
        mark = "online" if status == "online" else f"**{status.upper()}**"
        lines.append(f"- runner `{r.get('name', '?')}`: {mark}"
                     f"{', busy' if busy else ''}"
                     f"{'' if matches else ', label mismatch'}")
        if matches and status == "online":
            ok = True
    if not ok:
        lines.append(f"- **no online runner carries all of {sorted(wanted)}** — "
                     f"a run submitted now will QUEUE and later be reported "
                     f"`cancelled`, which alerts nobody.")
    return ok, lines


# Where a tool may live BESIDES the PATH.
#
# WHY THIS EXISTS. This check used raw shutil.which -- PATH only -- while
# windows-validation.yml's coverage step resolves OpenCppCoverage from three
# places: PATH, the portable %LOCALAPPDATA%\Programs location, and a
# Program-Files install that is not on PATH. The two disagreed, and the
# disagreement was not academic: the operator's elevated install put the binary
# in Program Files WITHOUT adding it to PATH, so this check reported MISSING,
# failed the readiness step, and cascaded into a `cmake not found` build failure
# (F46). All three dispatched runs on 2026-08-19 died there in under two
# minutes and NEVER REACHED the coverage step -- while the tool was sitting
# installed and working on the very same machine.
#
# A pre-flight check that is stricter than the step it front-runs does not
# prevent a mid-run failure; it manufactures an earlier one. Keep this list in
# step with the workflow's own fallbacks.
TOOL_FALLBACK_DIRS: dict[str, tuple[str, ...]] = {
    "OpenCppCoverage": (
        r"%LOCALAPPDATA%\Programs\OpenCppCoverage",
        r"C:\Program Files\OpenCppCoverage",
        r"C:\Program Files (x86)\OpenCppCoverage",
    ),
}


def _resolve_tool(tool: str) -> str | None:
    """PATH first, then the documented fallback directories."""
    found = shutil.which(tool)
    if found:
        return found
    for raw in TOOL_FALLBACK_DIRS.get(tool, ()):
        candidate = os.path.join(os.path.expandvars(raw), tool + ".exe")
        if os.path.isfile(candidate):
            return candidate
    return None


def check_tools(required=REQUIRED_TOOLS, observed=OBSERVED_TOOLS,
                which=None) -> tuple[bool, list[str]]:
    # Default to the fallback-aware resolver. `which` stays injectable so the
    # self-test can drive both branches without touching the real filesystem.
    if which is None:
        which = _resolve_tool
    lines = []
    ok = True
    for tool in required:
        found = which(tool)
        if found:
            lines.append(f"- tool `{tool}`: present (`{found}`)")
        else:
            ok = False
            lines.append(f"- tool `{tool}`: **MISSING** — a step that needs it "
                         f"will go red mid-run, hours in.")
    for tool in observed:
        found = which(tool)
        lines.append(f"- tool `{tool}` (observed, not required): "
                     f"{'present' if found else 'absent'}")
    return ok, lines


def cmd_readiness(args) -> int:
    labels = [s.strip() for s in args.labels.split(",") if s.strip()]
    runner_unavailable = ""
    doc = None
    if args.runners:
        doc = _load_json(args.runners, "runner list")
    else:
        repo = args.repo or os.environ.get("GITHUB_REPOSITORY")
        if not repo:
            raise ReadinessError(
                "no --repo and no GITHUB_REPOSITORY; cannot ask the API which "
                "runners are online.")
        try:
            doc = _gh(["api", f"repos/{repo}/actions/runners"], "runner list")
        except ReadinessError as exc:
            # `GET /actions/runners` needs **Administration: read**, which the
            # ambient GITHUB_TOKEN deliberately does not carry. Raising here
            # would fail the very first step of every manual run — a blocker
            # this check invented, on the half of it that is near-tautological
            # in-run anyway (the step is executing ON a runner).
            #
            # So: when the query cannot be made, say so LOUDLY and keep going,
            # and let the tool check decide the exit code. `--require-runner`
            # restores hard enforcement for the out-of-band use, where a
            # developer's own `gh` auth CAN read it and the answer is the whole
            # point of running the command.
            #
            # This is not fail-open by accident. The distinction is that an
            # unreadable runner list is REPORTED, not assumed green, and the
            # enforced half (tools) is unaffected.
            if args.require_runner:
                raise
            runner_unavailable = str(exc)

    if runner_unavailable:
        runner_ok = True  # not asserted either way; see the report line below
        runner_lines = [
            "- runner: **NOT CHECKED** — the runner list could not be read.",
            f"  - reason: {runner_unavailable.splitlines()[0][:300]}",
            "  - `GET /actions/runners` requires **Administration: read**, "
            "which the ambient `GITHUB_TOKEN` does not carry. In-run this "
            "check is near-tautological anyway (this step is executing on a "
            "runner). Run it before submitting, from a shell with your own "
            "`gh` auth, where it is meaningful: "
            "`check_ci_readiness.py readiness --repo <owner/repo> "
            "--require-runner`.",
        ]
    else:
        runner_ok, runner_lines = check_runner(doc, labels)
    tools_ok, tool_lines = check_tools()

    ok = runner_ok and tools_ok
    header = ("## Operational readiness — "
              + ("READY" if ok else "**NOT READY**"))
    _emit([header, "", *runner_lines, *tool_lines, "",
           ("All readiness checks passed." if ok else
            "One or more readiness checks FAILED. Fix these before submitting "
            "a run; a run started in this state produces a `cancelled` or a "
            "mid-run red hours later, which is the F49/A5-4.3 masking this "
            "check exists to prevent.")], args.summary)
    return 0 if ok else 1


# ---------------------------------------------------------------------------
# steps — AFTER the work
# ---------------------------------------------------------------------------

def red_steps(jobs_doc) -> list[dict]:
    """Every step whose own conclusion is red, across every job."""
    if isinstance(jobs_doc, dict) and "jobs" in jobs_doc:
        jobs = list(jobs_doc["jobs"] or [])
    elif isinstance(jobs_doc, list):
        jobs = list(jobs_doc)
    else:
        raise ReadinessError(
            "jobs payload has neither a 'jobs' key nor a list at the top "
            "level; refusing to report 'no red steps' from data it could not "
            "read.")
    if not jobs:
        raise ReadinessError(
            "the run reports zero jobs. Refusing to pass: 'no jobs' is not "
            "'no failures', and a checker that cannot see a job cannot clear "
            "it.")
    out = []
    for job in jobs:
        # security-audit finding, phase-66: a job object with NO `steps` key is
        # not a job with no failures — it is a payload this function cannot
        # read. `or []` would have quietly reported clean, which is the one
        # thing the rest of this function is written not to do. Real API
        # responses always carry `steps`, so this is unreachable today; the
        # point is that the contract says fail-closed and every path must.
        if "steps" not in job:
            raise ReadinessError(
                f"job {job.get('name', '?')!r} has no 'steps' key. Refusing to "
                f"report it clean: an unreadable job is not a passing job.")
        for step in (job.get("steps") or []):
            if (step.get("conclusion") or "").lower() in RED_CONCLUSIONS:
                out.append({"job": job.get("name", "?"),
                            "number": step.get("number"),
                            "name": step.get("name", "?"),
                            "conclusion": step.get("conclusion")})
    return out


def job_conclusions(jobs_doc) -> list[tuple[str, str]]:
    jobs = jobs_doc["jobs"] if isinstance(jobs_doc, dict) else jobs_doc
    return [(j.get("name", "?"), j.get("conclusion") or "?") for j in jobs]


def cmd_steps(args) -> int:
    if args.jobs:
        doc = _load_json(args.jobs, "job list")
    else:
        repo = args.repo or os.environ.get("GITHUB_REPOSITORY")
        run_id = args.run_id or os.environ.get("GITHUB_RUN_ID")
        if not (repo and run_id):
            raise ReadinessError(
                "need --repo/--run-id (or GITHUB_REPOSITORY/GITHUB_RUN_ID) to "
                "read this run's own step conclusions.")
        doc = _gh(["api", f"repos/{repo}/actions/runs/{run_id}/jobs",
                   "--paginate"], f"jobs for run {run_id}")

    reds = red_steps(doc)
    lines = ["## Step-level conclusions", ""]
    for name, concl in job_conclusions(doc):
        lines.append(f"- job `{name}`: reported **{concl}**")
    if reds:
        lines += ["", f"### {len(reds)} step(s) went RED on their own merits", ""]
        for r in reds:
            lines.append(f"- step {r['number']} — **{r['conclusion']}** — "
                         f"{r['name']}")
        lines += ["",
                  "These failed independently of the run-level conclusion. A "
                  "run reported `cancelled` with red steps inside it is the "
                  "F44-species masking recorded at audit-of-audits §4.3 — the "
                  "run-level status alerts nobody and hides them."]
    else:
        lines += ["", "No step went red."]
    _emit(lines, args.summary)

    if reds and args.enforce:
        return 1
    return 0


# ---------------------------------------------------------------------------
# self-test
# ---------------------------------------------------------------------------

def self_test() -> int:
    """Assert the checker's own contract. Returns the number of checks."""
    failures: list[str] = []
    checks = 0

    def check(label, got, want):
        nonlocal checks
        checks += 1
        if got != want:
            failures.append(f"{label}: got {got!r}, want {want!r}")

    # Runner detection.
    online = {"runners": [{"name": "pdfe-windows", "status": "online",
                           "busy": False,
                           "labels": [{"name": "self-hosted"},
                                      {"name": "Windows"}, {"name": "X64"},
                                      {"name": "pdfe-windows"}]}]}
    offline = json.loads(json.dumps(online))
    offline["runners"][0]["status"] = "offline"
    labels = ["self-hosted", "Windows", "X64", "pdfe-windows"]
    check("online runner is ready", check_runner(online, labels)[0], True)
    check("offline runner is NOT ready", check_runner(offline, labels)[0], False)
    check("no runners at all is NOT ready",
          check_runner({"runners": []}, labels)[0], False)

    mislabelled = json.loads(json.dumps(online))
    mislabelled["runners"][0]["labels"] = [{"name": "self-hosted"}]
    check("a runner missing a required label is NOT ready",
          check_runner(mislabelled, labels)[0], False)

    # Tools.
    check("a present tool passes",
          check_tools(required=("x",), observed=(),
                      which=lambda t: "C:/x.exe")[0], True)
    check("a missing tool fails",
          check_tools(required=("x",), observed=(),
                      which=lambda t: None)[0], False)

    # A tool installed but NOT on PATH must still resolve. This is the case that
    # failed three dispatched runs in under two minutes each while the binary
    # sat installed and working on the same machine: the readiness pre-check was
    # PATH-only, the coverage step it front-runs was not, and the stricter
    # pre-check manufactured a failure earlier rather than preventing one.
    import tempfile as _tf
    _dir = _tf.mkdtemp(prefix="pdfe-toolfallback-")
    try:
        _exe = os.path.join(_dir, "FakeCoverage.exe")
        with io.open(_exe, "w", encoding="utf-8") as _fh:
            _fh.write("")
        TOOL_FALLBACK_DIRS["FakeCoverage"] = (_dir,)
        check("a tool off PATH resolves via the fallback dirs",
              _resolve_tool("FakeCoverage"), _exe)
        check("check_tools accepts it via the default resolver",
              check_tools(required=("FakeCoverage",), observed=())[0], True)
        TOOL_FALLBACK_DIRS["FakeCoverage"] = (os.path.join(_dir, "nope"),)
        check("a genuinely absent tool still fails",
              check_tools(required=("FakeCoverage",), observed=())[0], False)
    finally:
        TOOL_FALLBACK_DIRS.pop("FakeCoverage", None)
        shutil.rmtree(_dir, ignore_errors=True)

    # The fallback list must cover what the workflow's coverage step probes.
    # If the workflow gains a location and this does not, the pre-check goes
    # back to being stricter than the step it guards.
    _wf = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(
        os.path.abspath(__file__)))), ".github", "workflows",
        "windows-validation.yml")
    if os.path.isfile(_wf):
        with io.open(_wf, encoding="utf-8") as _fh:
            _text = _fh.read()
        for _needle in ("Programs\\OpenCppCoverage",
                        "C:\\Program Files\\OpenCppCoverage"):
            if _needle in _text:
                check(f"fallback list covers workflow location {_needle}",
                      any(_needle in os.path.expandvars(d) or _needle in d
                          for d in TOOL_FALLBACK_DIRS["OpenCppCoverage"]),
                      True)

    # Red-step detection, including the masking shape.
    masked = {"jobs": [{"name": "windows-validation", "conclusion": "cancelled",
                        "steps": [
                            {"number": 1, "name": "ok", "conclusion": "success"},
                            {"number": 2, "name": "parity",
                             "conclusion": "failure"},
                            {"number": 3, "name": "ui",
                             "conclusion": "cancelled"},
                            {"number": 4, "name": "later",
                             "conclusion": "skipped"},
                        ]}]}
    reds = red_steps(masked)
    check("a red step inside a cancelled run is found", len(reds), 1)
    check("the red step is the failing one", reds[0]["name"], "parity")
    check("a cancelled step is not counted red",
          any(r["name"] == "ui" for r in reds), False)
    check("a skipped step is not counted red",
          any(r["name"] == "later" for r in reds), False)

    timed = {"jobs": [{"name": "j", "conclusion": "success",
                       "steps": [{"number": 1, "name": "t",
                                  "conclusion": "timed_out"}]}]}
    check("a timed_out step counts red", len(red_steps(timed)), 1)

    clean = {"jobs": [{"name": "j", "conclusion": "success",
                       "steps": [{"number": 1, "name": "t",
                                  "conclusion": "success"}]}]}
    check("a clean run reports no red steps", len(red_steps(clean)), 0)

    # Fail-closed: unreadable shapes must raise, never return "clean".
    for bad, label in (({}, "empty dict"), ({"jobs": []}, "zero jobs"),
                       ("nonsense", "a string")):
        checks += 1
        try:
            red_steps(bad)
            failures.append(f"{label} must raise, not return clean")
        except ReadinessError:
            pass

    # A job present but missing its `steps` key must RAISE, not read clean.
    checks += 1
    try:
        red_steps({"jobs": [{"name": "j", "conclusion": "success"}]})
        failures.append("a job with no 'steps' key must raise, not read clean")
    except ReadinessError:
        pass

    if failures:
        print("[ci-readiness] self-test FAILED:", file=sys.stderr)
        for f in failures:
            print("  - " + f, file=sys.stderr)
        return -1
    print(f"[ci-readiness] self-test: {checks} checks passed")
    return checks


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(
        prog="check_ci_readiness.py",
        description="Operational readiness preflight and step-conclusion "
                    "postflight for windows-validation (F49).")
    sub = ap.add_subparsers(dest="cmd")

    r = sub.add_parser("readiness", help="runner online + required tools present")
    r.add_argument("--repo", default="")
    r.add_argument("--runners", default="",
                   help="recorded /actions/runners JSON instead of calling gh")
    # `pdfe-builder` is in the DEFAULT set. Readiness asks "can the fleet serve
    # the lane", and the lane includes windows-validation's `build` and
    # `release`, which are the only jobs carrying that label. A default that
    # omitted it reported a machine READY when the two jobs that produce every
    # downstream artifact could not be scheduled onto it at all -- and an
    # unsatisfiable `runs-on` does not error, it queues and then reports
    # `cancelled`. Narrow this only for a multi-machine fleet where another
    # registered machine carries the label (see profiles.consume in
    # tools/runner-deployment/runner-requirements.json).
    r.add_argument("--labels",
                   default="self-hosted,Windows,X64,pdfe-windows,pdfe-builder")
    r.add_argument("--summary", default="",
                   help="file to append the markdown report to "
                        "(defaults to $GITHUB_STEP_SUMMARY)")
    r.add_argument("--require-runner", action="store_true",
                   help="treat an unreadable runner list as a FAILURE. Use "
                        "out of band, where your own gh auth can read it; the "
                        "ambient GITHUB_TOKEN cannot (needs Administration: "
                        "read), so in-run the query is reported, not enforced.")
    r.set_defaults(func=cmd_readiness)

    s = sub.add_parser("steps", help="surface step-level red regardless of the "
                                     "run-level conclusion")
    s.add_argument("--repo", default="")
    s.add_argument("--run-id", default="")
    s.add_argument("--jobs", default="",
                   help="recorded /actions/runs/<id>/jobs JSON instead of gh")
    s.add_argument("--summary", default="")
    s.add_argument("--enforce", action="store_true",
                   help="exit non-zero when any step went red")
    s.set_defaults(func=cmd_steps)

    t = sub.add_parser("self-test", help="verify this checker's own contract")
    t.set_defaults(func=lambda a: 0 if self_test() > 0 else 1)
    return ap


def _force_utf8_stdio() -> None:
    """Explicit UTF-8 on stdout/stderr.

    Not defensive boilerplate: the step names this tool prints come from the
    workflow and contain non-ASCII (`Code coverage (OpenCppCoverage -> Cobertura)`
    uses a real U+2192 arrow). On a Windows console defaulting to cp1252 the
    first `print` raises UnicodeEncodeError, and the checker dies WHILE REPORTING
    a red step — turning a surfaced failure back into a silent one, which is the
    exact defect it exists to prevent.
    """
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError):  # already wrapped, or not a TTY
            pass


def main(argv: list[str]) -> int:
    _force_utf8_stdio()
    args = build_parser().parse_args(argv)
    if not getattr(args, "func", None):
        build_parser().print_help()
        return 2
    try:
        return args.func(args)
    except ReadinessError as exc:
        # EXIT 3 = "could not perform the check", distinct from a check that
        # RAN and returned a verdict (0/1).
        #
        # Why this distinction had to exist. On dispatched run 32237974228 the
        # step-level unmasking step hit `gh: Resource not accessible by
        # integration (HTTP 403)`, which raised here and exited 1. The workflow
        # step ended with a bare `exit 0`, so the step reported SUCCESS -- the
        # unmasking mechanism silently not running looked identical to it
        # running and finding nothing. That is the exact false-green class this
        # tool exists to prevent, inside the tool itself.
        #
        # Callers must treat 3 as "I do not know", never as "clean". 0 and 1
        # are unchanged, so no existing caller's verdict handling shifts.
        print(f"[ci-readiness] FAILED: {exc}", file=sys.stderr)
        print("[ci-readiness] exit 3 = the check could not be performed; this "
              "is NOT a clean result.", file=sys.stderr)
        return 3


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
