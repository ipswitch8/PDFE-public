# -*- coding: utf-8 -*-
"""Guard the crash-dump artifact wiring in windows-validation.yml.

WHY THIS EXISTS, and what it is NOT for. Crash dumps were never uploaded
unconditionally -- `CrashHandler::writeMinidump` is reached only from the SEH
handler, the `std::terminate` handler and an explicit request, and a clean run
leaves `ui-out/crashdumps` empty. The 987 MB that exhausted the artifact quota
did not mean an over-eager upload; it meant CRASHES WERE HAPPENING ON MOST RUNS,
each dump was 25-50 MB, and every one of them was being kept for FOURTEEN DAYS
because it rode inside the general logs artifact.

So the five things this guards are the ones that actually bound the cost and
keep the dumps meaningful:

  1. NO GitHub artifact carries a crash dump. Artifacts of a PUBLIC repo are
     downloadable by anyone, and a minidump of this app can contain document
     passwords, decrypted copy-restricted content and OCSP/CRL tokens.
  2. Dumps ship to the PRIVATE R2 bucket -- and when R2 is unconfigured they
     are published NOWHERE and the step says so. An artifact fallback would
     restore the exposure, so its absence is what is asserted.
  3. `ui-out/crashdumps/**` is EXCLUDED from the logs artifact, so a dump is not
     stored twice at two different retentions -- fixing (1) without (3) doubles
     the storage instead of halving it, which is the failure mode a reviewer
     would most easily miss.
  4. The per-run dump count is BOUNDED, and truncation is announced. A silently
     truncated set reads as "only this many crashes happened", which is the
     opposite of the truth.
  5. The build-store pruning EXEMPTS crash-pinned stores. A dump whose PDB has
     been pruned is an address, not a diagnosis. That is not hypothetical: it
     is precisely what happened to the 2026-08-13 dumps and it cost the only
     available diagnosis of a live use-after-free.

Run:
    python tools/gate/check_crashdump_artifact.py [--self-test]
"""
from __future__ import annotations

import io
import os
import re
import sys

import yaml  # type: ignore

WORKFLOW = os.path.join(".github", "workflows", "windows-validation.yml")

LOGS_ARTIFACT = "windows-validation-logs"

# NO artifact may carry crash dumps. This name is the one that used to, and is
# kept so the guard can specifically catch its return -- a re-added
# `windows-validation-crashdumps` is the most likely way this regresses.
FORBIDDEN_DUMP_ARTIFACT = "windows-validation-crashdumps"

# Where dumps go instead: the private R2 bucket.
R2_DUMP_PREFIX = "pdfe-crashdumps/"
CRASHDUMP_GLOB = "ui-out/crashdumps/**"
EXCLUDE_GLOB = "!ui-out/crashdumps/**"
PIN_FILE = "crash-pin.txt"


# PowerShell cmdlets that make a line an actual pipeline stage.
PIPELINE_CMDLETS = ("Where-Object", "ForEach-Object", "Sort-Object",
                    "Select-Object", "Get-ChildItem", "Measure-Object")


def _is_pipeline_stage(line: str) -> bool:
    """True only for a line that is REALLY a stage feeding the next one.

    Ending in `|` is not enough, and that is not a hypothetical nicety: the
    phase-2 validator gate defeated the first version of this walk by deleting
    the real `Where-Object` pin filter and leaving a COMMENT that mentioned
    crash-pin.txt and ended in a pipe. The guard stayed green while pruning
    deleted every store regardless of pin -- this file's own failure mode,
    inside the check written to catch it.

    STATED LIMITATION, because the fix is narrower than it looks: this is a
    text scan, not a semantic one. A genuine
    `Where-Object { $true -or $_.Name -eq 'crash-pin.txt' }` is a real cmdlet
    containing the literal and would still pass while exempting nothing.
    Closing THAT needs a behavioural test that materialises pinned and unpinned
    store directories and runs the real pruning against them -- the pattern
    test_build_reuse_roundtrip.py already uses for the build-reuse guards. That
    is tracked as follow-up; this raises the bar without pretending to be the
    ceiling.
    """
    s = line.strip()
    if not s.endswith("|"):
        return False
    if s.startswith("#"):
        return False
    return any(c in s for c in PIPELINE_CMDLETS)


def _uploads(doc: dict) -> dict:
    """{artifact name: {'path': str, 'retention': int|None}} across all jobs."""
    out = {}
    for job in (doc.get("jobs") or {}).values():
        for s in job.get("steps") or []:
            if "actions/upload-artifact" not in str(s.get("uses", "")):
                continue
            w = s.get("with") or {}
            name = str(w.get("name", "")).strip()
            if not name:
                continue
            ret = w.get("retention-days")
            try:
                ret = int(ret) if ret is not None else None
            except (TypeError, ValueError):
                ret = None
            out[name] = {"path": str(w.get("path", "")), "retention": ret}
    return out


def check_doc(doc: dict, text: str) -> list[str]:
    problems: list[str] = []
    ups = _uploads(doc)

    logs = ups.get(LOGS_ARTIFACT)

    if logs is None:
        problems.append(
            f"{WORKFLOW}: no upload named {LOGS_ARTIFACT!r}. This guard's other "
            f"rules are all stated relative to it, so its absence makes them "
            f"vacuous rather than satisfied.")
        return problems

    lr, dr = logs["retention"], None

    # 1. NO GitHub artifact may carry crash dumps.
    #
    # Artifacts of a PUBLIC repository are downloadable by anyone with read
    # access -- which is everyone -- and a minidump of this application can
    # contain document passwords, decrypted content from copy-restricted PDFs,
    # OCSP/CRL tokens and absolute host paths. The destination is the private R2
    # bucket. This rule catches the specific regression of re-adding the old
    # artifact, and the general one of any upload naming the dump staging dir.
    if FORBIDDEN_DUMP_ARTIFACT in ups:
        problems.append(
            f"{WORKFLOW}: artifact {FORBIDDEN_DUMP_ARTIFACT!r} is back. Crash "
            f"dumps must NOT be uploaded as a GitHub artifact -- artifacts of a "
            f"public repo are downloadable by anyone, and a dump of this app can "
            f"carry document passwords. They go to the private R2 bucket.")
    for name, info in ups.items():
        for entry in info["path"].splitlines():
            e = entry.strip()
            if not e or e.startswith("!"):
                continue          # blank, or an EXCLUSION -- the correct shape
            if "crashdump-upload" in e or "crashdumps" in e:
                problems.append(
                    f"{WORKFLOW}: artifact {name!r} includes path entry {e!r}, "
                    f"which publishes crash dumps. Dumps are sensitive and must "
                    f"reach only the private R2 bucket.")

    # 2. Dumps must actually be shipped to R2, and absent R2 nothing is
    #    published. A fallback to an artifact would restore the exposure this
    #    rule exists to remove, so its ABSENCE is what is asserted.
    # THE PREFIX MUST APPEAR ON AN ACTUAL UPLOAD LINE, not merely somewhere in
    # the file. The first version tested `R2_DUMP_PREFIX not in text`, and a
    # live mutation walked straight through it: the provenance sidecar's PROSE
    # names the same prefix to tell a reader where dumps live, so deleting the
    # real `aws s3 cp` destination left the literal present and the guard green.
    # That is the literal-surviving-in-a-comment failure this file has already
    # caught twice elsewhere, occurring in its own newest rule.
    # Follow the VARIABLE, because the real code builds the key once and reuses
    # it -- which is the right shape, and a rule that demanded the literal
    # inline would have pushed it toward the worse one. Two things must both
    # hold: something ASSIGNS the prefix, and an `aws s3 cp` writes to an
    # `s3://` destination built from that same variable.
    assign = re.search(r"\$(\w+)\s*=\s*[\"'][^\"']*" + re.escape(R2_DUMP_PREFIX),
                       text)
    ships = False
    if assign:
        var = assign.group(1)
        ships = any(("aws" in ln and "s3" in ln and " cp" in ln
                     and "s3://" in ln and ("$" + var) in ln)
                    for ln in text.splitlines())
    if not ships:
        problems.append(
            f"{WORKFLOW}: nothing actually uploads to {R2_DUMP_PREFIX!r} -- there "
            f"is no `aws s3 cp` writing to an s3:// destination built from a "
            f"variable assigned that prefix. Dumps are staged and then go "
            f"nowhere, so a crash leaves no post-mortem material at all. "
            f"(Naming the prefix in a comment or a provenance sidecar does not "
            f"ship anything, and an earlier version of this rule was satisfied "
            f"by exactly that.)")
    if "not being published anywhere" not in text.lower():
        problems.append(
            f"{WORKFLOW}: the dump shipper does not state what happens when R2 "
            f"is unconfigured. It must say so loudly and publish NOTHING -- a "
            f"silent skip reads as 'no crash occurred', and an artifact fallback "
            f"would reintroduce the exposure.")

    # 3. crashdumps excluded from the logs artifact -- the half-fix check
    logs_path = logs["path"]
    if EXCLUDE_GLOB not in logs_path:
        problems.append(
            f"{WORKFLOW}: {LOGS_ARTIFACT!r} does not exclude {EXCLUDE_GLOB!r}, "
            f"so `ui-out/**` sweeps the dumps into a PUBLISHED artifact "
            f"({lr}d retention) and they leave the private R2 path entirely. "
            f"Now that dumps go to R2 this exclusion is the ONLY thing keeping "
            f"them out of an artifact anyone can download once this repository "
            f"is public -- it stopped being a storage-cost rule and became the "
            f"containment rule.")

    # 4. the per-run cap exists and is announced
    if not re.search(r"\$KEEP\s*=\s*\d+", text):
        problems.append(
            f"{WORKFLOW}: no per-run dump cap (`$KEEP = <n>`) found. One run "
            f"that crashes repeatedly can upload an unbounded number of "
            f"25-50 MB dumps.")
    if "DROPPING" not in text:
        problems.append(
            f"{WORKFLOW}: the dump cap does not announce truncation. A silently "
            f"truncated set reads as 'only this many crashes happened', which "
            f"is the opposite of what it means.")

    # 5. pruning exempts crash-pinned stores
    if PIN_FILE not in text:
        problems.append(
            f"{WORKFLOW}: no {PIN_FILE!r} anywhere. Nothing stops the build-store "
            f"pruning from deleting the PDB an uploaded dump needs, which "
            f"reduces the dump to an address with no symbols.")
    else:
        # STRUCTURAL, NOT A CHARACTER WINDOW. The first version of this rule
        # looked 1200 characters back from `Select-Object -Skip` -- which is the
        # same char-count-bounded shape that has already broken a guard in this
        # repo once, when a REQUIRED comment grew and pushed the thing it was
        # looking for out of range. Instead, walk the actual PowerShell pipeline:
        # from the `Select-Object -Skip` line, step backwards while the previous
        # line ends in `|`, which is exactly the set of stages feeding it.
        # Comments and blank lines can then grow without limit.
        lines = text.splitlines()
        for i, ln in enumerate(lines):
            if not re.search(r"Select-Object -Skip \d+", ln):
                continue
            stages = [ln]
            j = i - 1
            while j >= 0 and _is_pipeline_stage(lines[j]):
                stages.append(lines[j])
                j -= 1
            if not any(PIN_FILE in s for s in stages):
                problems.append(
                    f"{WORKFLOW}:{i + 1}: the pruning pipeline does not exempt "
                    f"stores holding {PIN_FILE!r}. The pin is written but never "
                    f"honoured, so the PDB is pruned anyway -- a control that "
                    f"exists and does nothing.")
            break

    # 6. THE STAGING DIR MUST BE CLEARED BEFORE THE EARLY EXIT.
    #
    # Found by the phase-2 security-audit gate, and it silently defeated the
    # whole retention property. `New-Item -Force` does not empty a directory,
    # every checkout here is `clean: false`, and the workspace persists. A run
    # with zero dumps exited early, leaving a previous run's .dmp files staged;
    # the `if: always()` upload then re-shipped them under a NEW run id with a
    # FRESH retention clock. Bounded per upload, unbounded in effect.
    if "crashdump-upload" in text:
        clear = text.find("Remove-Item $out -Recurse -Force")
        early = text.find("$all.Count -eq 0")
        if clear < 0:
            problems.append(
                f"{WORKFLOW}: the crash-dump staging directory is never cleared. "
                f"`New-Item -Force` does not empty it and the workspace persists "
                f"across runs (`clean: false`), so a previous run's dumps are "
                f"re-uploaded under a new run id with a fresh retention clock.")
        elif 0 <= early < clear:
            problems.append(
                f"{WORKFLOW}: the staging directory is cleared AFTER the "
                f"zero-dumps early exit, so the exact case that strands stale "
                f"dumps -- a run with no crashes -- skips the clear entirely. "
                f"It must happen before the count check.")

    # 7. CRASH PINS MUST EXPIRE. A pin exempts ~2.5 GB from pruning; with no
    #    expiry every crashy commit retains its store forever on a box that
    #    measured 41.8 GB free, and disk exhaustion on this project has
    #    previously presented as an unrelated failure.
    #
    # LOOK FOR THE ASSIGNMENT, NOT THE TOKEN. The first version tested
    # `"PIN_MAX_AGE_DAYS" not in text`, which a live mutation walked straight
    # through: deleting the assignment leaves the name behind in
    # `AddDays(-$PIN_MAX_AGE_DAYS)` and in a comment, so the token was still
    # present while the bound was gone. Content guards that key on a NAME rather
    # than the thing that gives it a value are decoration -- the same shape as a
    # guard whose literal survives in a comment.
    if PIN_FILE in text and not re.search(r"\$PIN_MAX_AGE_DAYS\s*=\s*\d+", text):
        problems.append(
            f"{WORKFLOW}: crash pins are never expired. A pinned store is exempt "
            f"from pruning permanently, so repeated crashes fill the disk. Give "
            f"pins a bounded lifetime (`$PIN_MAX_AGE_DAYS`).")
    return problems


def self_test() -> int:
    checks = 0
    failures: list[str] = []

    def ck(label, got, want):
        nonlocal checks
        checks += 1
        if got != want:
            failures.append(f"{label}: got {got!r}, want {want!r}")

    good_text = (
        "          $out = \"$env:GITHUB_WORKSPACE\\crashdump-upload\"\n"
        "          $KEEP = 4\n"
        "          Remove-Item $out -Recurse -Force -ErrorAction SilentlyContinue\n"
        "          if ($all.Count -eq 0) { exit 0 }\n"
        "          Write-Host \"::warning::... DROPPING $dropped ...\"\n"
        "          $prefix = \"pdfe-crashdumps/$sha/run-$id\"\n"
        "          aws s3 cp $f \"s3://$r2b/$prefix/$n\" --endpoint-url $e\n"
        "          Write-Host \"::warning::... they are NOT being published anywhere.\"\n"
        "          ... crash-pin.txt ...\n"
        "            $PIN_MAX_AGE_DAYS = 14\n"
        "              Where-Object { -not (Test-Path (Join-Path $_.FullName 'crash-pin.txt')) } |\n"
        "              Sort-Object LastWriteTime -Descending | Select-Object -Skip 6 |\n"
    )
    good = {"jobs": {"report": {"steps": [
        {"uses": "actions/upload-artifact@v4", "with": {
            "name": LOGS_ARTIFACT,
            "path": "ui-suite.log\nui-out/**\n!ui-out/crashdumps/**\n",
            "retention-days": 14}},
    ]}}}
    ck("sound wiring passes", check_doc(good, good_text), [])

    import copy





    # THE CONTAINMENT CASE. When dumps went to their own artifact, dropping this
    # exclusion merely stored them twice -- a cost problem. Now that dumps go to
    # private R2, this exclusion is the ONLY thing keeping them out of a
    # PUBLISHED artifact, so the same edit changed from wasteful to a data leak.
    # The rule did not change; what it protects did.
    bad3 = copy.deepcopy(good)
    # index 0 now: the fixture holds ONLY the logs artifact, since the dump
    # artifact was removed rather than reconfigured.
    bad3["jobs"]["report"]["steps"][0]["with"]["path"] = "ui-suite.log\nui-out/**\n"
    ck("dropping the exclusion (dumps into a published artifact) is caught",
       any("does not exclude" in p for p in check_doc(bad3, good_text)), True)

    ck("a missing cap is caught",
       any("per-run dump cap" in p
           for p in check_doc(good, good_text.replace("$KEEP = 4", "# none"))),
       True)
    ck("silent truncation is caught",
       any("announce truncation" in p
           for p in check_doc(good, good_text.replace("DROPPING", "quietly"))),
       True)
    ck("no crash pin at all is caught",
       any("Nothing stops the build-store" in p
           for p in check_doc(good, good_text.replace("crash-pin.txt", "x.txt"))),
       True)

    # THE SUBTLE ONE: the pin is written but the pruning ignores it. Everything
    # still mentions crash-pin.txt, so a whole-file containment check would pass.
    unhonoured = (
        "          $KEEP = 4\n"
        "          Write-Host \"DROPPING $dropped\"\n"
        "          ... writes crash-pin.txt ...\n"
        + ("\n" * 80) +
        "              Sort-Object LastWriteTime -Descending | Select-Object -Skip 6 |\n"
    )
    ck("a pin that pruning does not honour is caught",
       any("never honoured" in p for p in check_doc(good, unhonoured)), True)

    # --- the destination rules --------------------------------------------
    # Re-adding the old artifact is the most likely regression, and it would
    # publish document passwords the moment the repo is public.
    bad_art = copy.deepcopy(good)
    bad_art["jobs"]["report"]["steps"].insert(0, {
        "uses": "actions/upload-artifact@v4",
        "with": {"name": FORBIDDEN_DUMP_ARTIFACT,
                 "path": "crashdump-upload/**", "retention-days": 3}})
    ck("re-adding the crash-dump artifact is caught",
       any("is back" in p for p in check_doc(bad_art, good_text)), True)

    # Any OTHER artifact that sweeps the staging dir is equally fatal.
    bad_sweep = copy.deepcopy(good)
    bad_sweep["jobs"]["report"]["steps"].append({
        "uses": "actions/upload-artifact@v4",
        "with": {"name": "something-else", "path": "crashdump-upload/**"}})
    ck("any artifact publishing the dump staging dir is caught",
       any("publishes crash dumps" in p for p in check_doc(bad_sweep, good_text)),
       True)

    # ...but the EXCLUSION entry must not be mistaken for publication. This is
    # the defect the first version of the rule had: it judged the whole
    # multi-line path block, so `!ui-out/crashdumps/**` never looked like an
    # exclusion and the artifact doing the right thing was flagged.
    ck("an exclusion entry is not mistaken for publication",
       check_doc(good, good_text), [])

    ck("dumps going nowhere (no R2 ship) is caught",
       any("nothing actually uploads to" in p for p in check_doc(
           good, good_text.replace("pdfe-crashdumps/", "somewhere-else/"))), True)

    # PROSE IS NOT AN UPLOAD. The sidecar names the same prefix to tell a
    # reader where dumps live; deleting the real upload while leaving that
    # text must still be caught, or the rule is containment-only.
    prose_only = "\n".join(
        ("          # they live under pdfe-crashdumps/<commit>/run-<id>/"
         if ("aws" in ln and "s3" in ln and "cp" in ln) else ln)
        for ln in good_text.splitlines()) + "\n"
    ck("naming the prefix only in prose does not count as shipping",
       any("nothing actually uploads to" in p for p in check_doc(good, prose_only)),
       True)

    ck("silence when R2 is unconfigured is caught",
       any("must say so loudly" in p for p in check_doc(
           good, good_text.replace("NOT being published anywhere", "skipped"))),
       True)

    # --- rule 5 hardening: a COMMENT cannot impersonate a pipeline stage ---
    # This is the exact bypass the phase-2 validator gate demonstrated against
    # the live workflow: delete the real Where-Object, leave a comment that
    # mentions crash-pin.txt and ends in a pipe. Every literal is still present.
    comment_impersonation = (
        "          $out = crashdump-upload\n"
        "          $KEEP = 4\n"
        "          s3://$r2b/pdfe-crashdumps/$env:GITHUB_SHA/run-x\n"
        "          ::warning::... are NOT being published anywhere.\n"
        "          Remove-Item $out -Recurse -Force -ErrorAction SilentlyContinue\n"
        "          if ($all.Count -eq 0) { exit 0 }\n"
        "          Write-Host \"DROPPING $dropped\"\n"
        "          ... writes crash-pin.txt ...\n"
        "            $PIN_MAX_AGE_DAYS = 14\n"
        "              # crash-pin.txt exemption already applied above |\n"
        "              Sort-Object LastWriteTime -Descending | Select-Object -Skip 6 |\n"
    )
    ck("a COMMENT ending in | cannot impersonate the pin filter",
       any("never honoured" in p for p in check_doc(good, comment_impersonation)),
       True)

    ck("a real cmdlet stage IS accepted (the fix is not just 'reject everything')",
       check_doc(good, good_text), [])

    # --- rule 6: staging dir clearing ---
    ck("a staging dir that is never cleared is caught",
       any("never cleared" in p for p in check_doc(
           good, good_text.replace(
               "          Remove-Item $out -Recurse -Force -ErrorAction SilentlyContinue\n", ""))),
       True)

    # THE ORDERING CASE, which is the whole defect: clearing exists, but after
    # the early exit, so the zero-dump run -- the one that strands stale dumps
    # -- skips it. A containment check passes this.
    after_exit = (
        "          $out = crashdump-upload\n"
        "          $KEEP = 4\n"
        "          s3://$r2b/pdfe-crashdumps/$env:GITHUB_SHA/run-x\n"
        "          ::warning::... are NOT being published anywhere.\n"
        "          if ($all.Count -eq 0) { exit 0 }\n"
        "          Remove-Item $out -Recurse -Force -ErrorAction SilentlyContinue\n"
        "          Write-Host \"DROPPING $dropped\"\n"
        "          ... crash-pin.txt ...\n"
        "            $PIN_MAX_AGE_DAYS = 14\n"
        "              Where-Object { -not (Test-Path (Join-Path $_.FullName 'crash-pin.txt')) } |\n"
        "              Sort-Object LastWriteTime -Descending | Select-Object -Skip 6 |\n"
    )
    ck("clearing placed AFTER the early exit is caught",
       any("early exit" in p for p in check_doc(good, after_exit)), True)

    # --- rule 7: pins must expire ---
    ck("pins with no expiry are caught",
       any("never expired" in p for p in check_doc(
           good, good_text.replace("$PIN_MAX_AGE_DAYS = 14", "# no expiry"))),
       True)

    if failures:
        print("[crashdump-artifact] self-test FAILED:", file=sys.stderr)
        for f in failures:
            print("  - " + f, file=sys.stderr)
        return 1
    print(f"[crashdump-artifact] self-test: {checks} checks passed")
    return 0


def main(argv: list[str]) -> int:
    if "--self-test" in argv:
        return self_test()
    if not os.path.isfile(WORKFLOW):
        print(f"[crashdump-artifact] {WORKFLOW} not found", file=sys.stderr)
        return 1
    with io.open(WORKFLOW, encoding="utf-8") as fh:
        text = fh.read()
    doc = yaml.safe_load(text)
    problems = check_doc(doc, text)
    if problems:
        print("[crashdump-artifact] FAILED — crash dumps cost more than they tell you:")
        for p in problems:
            print("  * " + p)
        return 1
    ups = _uploads(doc)
    print(f"[crashdump-artifact] crash dumps reach NO GitHub artifact; they ship "
          f"to the private R2 bucket under {R2_DUMP_PREFIX!r}, are excluded from "
          f"{LOGS_ARTIFACT!r} (retention {ups[LOGS_ARTIFACT]['retention']}d), are "
          f"capped per run with truncation announced, publish nothing when R2 is "
          f"absent, and the PDB their symbols come from is exempt from "
          f"build-store pruning.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
