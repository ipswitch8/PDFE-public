# -*- coding: utf-8 -*-
"""Guard the build-reuse wiring in windows-validation.yml.

WHY THIS EXISTS. The lane can now skip its build steps and restore a previously
published build instead. That introduces one specific way to silently lose
coverage, and it is the F44/F46 masking class this phase exists to remove:

    a step guarded on `steps.build_debug.outcome == 'success'` SKIPS when the
    build was skipped -- which is exactly what happens on a successful restore.

So every producer guard naming a build step must ALSO accept a successful
restore. If it does not, a shard reusing a build would quietly run none of the
gtest / parity / coverage / security / render controls and still report green.

Also checked:
  * the build steps are gated on restore FAILING (`!=`), never on it succeeding
  * the publish step does not run when this run was itself a restore
  * the restore refuses a commit mismatch (the one failure that would make every
    downstream test green against code that is not under test)

Run:
    python tools/gate/check_build_reuse_guards.py [--self-test]
"""
from __future__ import annotations

import io
import os
import re
import sys

WORKFLOW = os.path.join(".github", "workflows", "windows-validation.yml")

# Two spellings, because windows-validation.yml is now a chained DAG. WITHIN a
# job a producer is `steps.<id>.outcome`; ACROSS jobs it must be a job OUTPUT.
#
# It has to be an output and not `needs.build.result`: a consumer needs to know
# WHICH of build-vs-restore succeeded, because the skipped one reports `skipped`.
# Collapsing both into the job result would lose precisely the distinction that
# stops a restored run from silently skipping every downstream control -- the
# defect this whole file exists to prevent, re-entering through the refactor.
#
# `steps.<id>.outcome` referenced from ANOTHER job does not error. It evaluates
# to empty, which reads as "not success", and the step silently SKIPS. That is
# why these are checked rather than trusted to fail loudly.
RESTORE_OK = ("steps.restore_build.outcome == 'success'",
              "needs.build.outputs.restored == 'success'")
RESTORE_NOT_OK = ("steps.restore_build.outcome != 'success'",
                  "needs.build.outputs.restored != 'success'")
BUILD_REFS = ("steps.build_debug.outcome", "steps.build_release.outcome",
              "needs.build.outputs.built", "needs.release.outputs.release_built")

# The two literals the cross-machine round trip turns on. Named so the rule and
# its self-test cannot drift apart, and raw strings because a non-raw Windows
# path here is one backslash-t away from testing a different string than the
# workflow actually contains.
ARTIFACT_KEY = r"pdfe-build-${{ github.sha }}"

# Step NAMES the cross-machine rule anchors on. Names, because the `uses:` value
# also occurs in explanatory comments and _step_segment takes the first match.
DOWNLOAD_STEP_NAME = "Fetch the published build from ARTIFACTS"
UPLOAD_STEP_NAME = "Upload the published build as an ARTIFACT"
STORE_PATH = r"C:\pdfe-build-store\${{ github.sha }}"

# Indented forms the self-test mutates, derived from the same constants so the
# fixture and its mutations cannot disagree.
_NAME_OK = "          name: " + ARTIFACT_KEY
_NAME_BAD = "          name: pdfe-build-latest"
_PATH_OK = "          path: " + STORE_PATH
_PATH_BAD = r"          path: C:\somewhere-else"


def _any_in(needles, hay: str) -> bool:
    return any(n in hay for n in needles)


def _step_segment(text: str, marker: str) -> str:
    """The body of the step carrying `marker`, to the start of the next step.

    This replaced a fixed character window (`[:1200]` / `[:4000]`). A window is
    a guess about how long a step is, and the guess broke the moment a step
    acquired the F46 PRODUCER-GUARD DECISION comment it is REQUIRED to carry:
    the commit-mismatch refusal was pushed past 4000 characters and the checker
    reported the refusal missing when it was right there. A guard that fails
    because a comment grew is a guard that trains people to delete comments.
    """
    i = text.find(marker)
    if i < 0:
        return ""
    # Walk back to the `- name:` that opens this step, then forward to the next.
    start = text.rfind("\n      - name:", 0, i)
    start = 0 if start < 0 else start + 1
    nxt = text.find("\n      - name:", i)
    return text[start:] if nxt < 0 else text[start:nxt]


def check_text(text: str) -> list[str]:
    """Return a list of problems. Empty means the wiring is sound."""
    problems: list[str] = []

    # 1. Every `if:` naming a build outcome must also accept a restore.
    for m in re.finditer(r"^\s*if:\s*(.+)$", text, re.MULTILINE):
        cond = m.group(1)
        line = text[: m.start()].count("\n") + 1
        names_build = any(ref in cond for ref in BUILD_REFS)
        if not names_build:
            continue
        # A step gated on the restore NOT having succeeded is deliberately
        # "only when we actually built" — the build steps themselves, and the
        # publish step, which must not republish a restored copy. Those are not
        # consumers of the binaries and are correctly skipped on a restored run.
        #
        # (The first version of this rule additionally required the condition
        # not to name a build outcome, which wrongly flagged the publish step —
        # it needs BOTH "we built" and "the build succeeded". The control case
        # in self_test caught that.)
        if _any_in(RESTORE_NOT_OK, cond):
            continue
        if not _any_in(RESTORE_OK, cond):
            problems.append(
                f"{WORKFLOW}:{line}: a producer guard names a build step but "
                f"does NOT accept a restore, so a reused build would SKIP this "
                f"step and report green having run nothing:\n    {cond.strip()}"
            )

    # 2. The build steps must be skipped only when a restore SUCCEEDED.
    if not _any_in(RESTORE_NOT_OK, text):
        problems.append(
            f"{WORKFLOW}: no build step is gated on any of {RESTORE_NOT_OK!r}; "
            f"the restore cannot actually save a build.")

    # 3. Publishing must not happen on a run that was itself a restore, or the
    #    store would be rewritten from a copy of itself.
    if "id: publish_build" in text:
        seg = _step_segment(text, "id: publish_build")
        if not _any_in(RESTORE_NOT_OK, seg):
            problems.append(
                f"{WORKFLOW}: the publish step does not exclude restored runs "
                f"(none of {RESTORE_NOT_OK}), so it would republish a restored "
                f"copy.")

    # 3b. CROSS-MACHINE REUSE. With 2-4 runners the machine that builds is
    #     usually not the machine that tests, so the local per-machine store
    #     cannot serve a cold machine on its own. If an artifact download exists
    #     it must land in the SAME directory the local publish writes, because
    #     the verification step is shared -- that is what makes the four refusal
    #     cases apply identically to an artifact-sourced store instead of
    #     needing to be re-proved against a second code path.
    if "actions/download-artifact" in text:
        # SCOPED TO THE DOWNLOAD STEP, not the whole file. The first version of
        # this rule used whole-file containment -- and both literals it looks
        # for appear TWICE in the live workflow, once in the download and once
        # in the upload. So repointing the download's artifact name while the
        # upload kept the correct literal passed CLEAN: the guard was decoration
        # for precisely the defect its own comment names. A karen gate found it
        # by mutating the live file; the self-test could not, because its
        # fixture held only one of the two steps.
        # Anchored on STEP NAMES, not on `actions/download-artifact`. That
        # bare string also appears in prose: the lookup step's comment explains
        # v4's per-run scoping, and _step_segment returns the FIRST match -- so
        # the guard silently inspected the lookup step and reported the download
        # missing fields it plainly had. A check that matches a comment is a
        # check measuring the wrong thing while looking healthy.
        dl = _step_segment(text, DOWNLOAD_STEP_NAME)
        up = _step_segment(text, UPLOAD_STEP_NAME)

        # An anchor that no longer matches yields an EMPTY segment, and every
        # `x not in ""` containment below would then fire at once -- noisy, but
        # the opposite arrangement (silently passing) is the dangerous one. Say
        # it plainly instead of leaving the reader to infer it from a wall of
        # failures.
        if not dl:
            problems.append(
                f"{WORKFLOW}: no step named {DOWNLOAD_STEP_NAME!r}. It was "
                f"renamed or removed; update DOWNLOAD_STEP_NAME deliberately "
                f"rather than letting this rule quietly stop applying.")
        if not up:
            problems.append(
                f"{WORKFLOW}: no step named {UPLOAD_STEP_NAME!r}. Same as above "
                f"-- a rename must not drop the cross-machine rule.")
        if not dl or not up:
            return problems

        if ARTIFACT_KEY not in dl:
            problems.append(
                f"{WORKFLOW}: the download-artifact step is not keyed on "
                f"`{ARTIFACT_KEY}`. An unkeyed or differently keyed artifact "
                f"could hand one commit's binaries to another commit's source "
                f"-- the failure the commit-mismatch refusal exists to stop, "
                f"arriving by a different route.")
        if STORE_PATH not in dl:
            problems.append(
                f"{WORKFLOW}: the download-artifact step does not extract into "
                f"{STORE_PATH}. It must populate the same directory the local "
                f"publish writes, or the shared verification step never sees it "
                f"and every refusal case would need re-proving against a second "
                f"path.")
        # v4 SCOPES ARTIFACTS PER RUN. Without `run-id` the download searches
        # only the CURRENT run -- and the publish happens in a LATER job of the
        # same run, so a same-run lookup is not merely empty, it is
        # chronologically impossible. A download-artifact@v4 with no run-id is
        # therefore INERT: it can never find anything, on any run.
        #
        # Nothing about the wiring shows this. Name and path agreed, the
        # refusal cases held, the self-tests passed, and two gates accepted it.
        # It took a security-audit gate reading the ACTION'S semantics. So the
        # rule lives here now rather than in anyone's memory.
        if "actions/download-artifact@v4" in dl:
            if "run-id:" not in dl:
                problems.append(
                    f"{WORKFLOW}: the download-artifact@v4 step has no "
                    f"`run-id:`. v4 scopes artifacts PER RUN, so without it the "
                    f"download searches only the current run -- and the publish "
                    f"happens in a later job of that same run. The cross-machine "
                    f"restore would be INERT: never finding anything, on any "
                    f"run, while every other check on it passes.")
            if "github-token:" not in dl:
                problems.append(
                    f"{WORKFLOW}: the download-artifact@v4 step has no "
                    f"`github-token:`. Cross-run artifact download requires one; "
                    f"without it the run-id lookup cannot be authorised.")
            if "steps.find_build_artifact.outputs.run_id != ''" not in dl:
                problems.append(
                    f"{WORKFLOW}: the download step is not conditioned on the "
                    f"artifact lookup having FOUND a run. Downloading with an "
                    f"empty run-id fails the step, turning 'no reusable build "
                    f"exists' -- an ordinary, expected state -- into a red run.")

        if "steps.restore_probe.outputs.local_hit != 'true'" not in dl:
            problems.append(
                f"{WORKFLOW}: the download-artifact step is not conditioned on "
                f"the local-store probe missing. Downloading 2.3 GB onto a "
                f"machine that already holds the build is waste, and on an "
                f"exhausted artifact quota it is a failure as well.")

        # The two ends must AGREE. A build published under one name and fetched
        # under another is a reuse that silently never happens -- and it fails
        # only at run time, on the machine that needed it, hours into a run.
        if up:
            if (ARTIFACT_KEY in dl) != (ARTIFACT_KEY in up):
                problems.append(
                    f"{WORKFLOW}: upload and download disagree about the "
                    f"artifact name `{ARTIFACT_KEY}`.")
            if (STORE_PATH in dl) != (STORE_PATH in up):
                problems.append(
                    f"{WORKFLOW}: upload and download disagree about "
                    f"{STORE_PATH}; the round trip does not close.")

    # 4. The restore must refuse a commit mismatch.
    if "id: restore_build" in text:
        seg = _step_segment(text, "id: restore_build")
        if "GITHUB_SHA" not in seg or "Refusing" not in seg:
            problems.append(
                f"{WORKFLOW}: the restore step does not visibly refuse a commit "
                f"mismatch; reusing another commit's binaries would make every "
                f"downstream test green against code that is not under test.")

    return problems


def self_test() -> int:
    checks = 0
    failures: list[str] = []

    def check(label, got, want):
        nonlocal checks
        checks += 1
        if got != want:
            failures.append(f"{label}: got {got!r}, want {want!r}")

    good = """
      - name: Restore
        id: restore_build
        if: ${{ !cancelled() && inputs.reuse_published_build != '' }}
        run: |
          if ($x -ne "$env:GITHUB_SHA") { Write-Error "Refusing to test"; exit 1 }
      - name: Build
        id: build_debug
        if: ${{ !cancelled() && steps.restore_build.outcome != 'success' }}
      - name: Consumer
        if: ${{ !cancelled() && (steps.build_debug.outcome == 'success' || steps.restore_build.outcome == 'success') }}
      - name: Publish
        id: publish_build
        if: ${{ !cancelled() && steps.restore_build.outcome != 'success' && steps.build_debug.outcome == 'success' }}
    """
    check("a sound wiring passes", check_text(good), [])

    # The defect this exists for: a consumer that forgot the restore term.
    bad = good.replace(
        "(steps.build_debug.outcome == 'success' || steps.restore_build.outcome == 'success')",
        "steps.build_debug.outcome == 'success'")
    check("a consumer missing the restore term is caught",
          len(check_text(bad)) >= 1, True)

    # A restore that does not refuse a commit mismatch.
    bad2 = good.replace('if ($x -ne "$env:GITHUB_SHA") { Write-Error "Refusing to test"; exit 1 }',
                        'Write-Host "restored"')
    check("a restore with no commit check is caught",
          any("commit mismatch" in p for p in check_text(bad2)), True)

    # A publish that would republish a restored copy.
    bad3 = good.replace(
        "if: ${{ !cancelled() && steps.restore_build.outcome != 'success' && steps.build_debug.outcome == 'success' }}",
        "if: ${{ !cancelled() && steps.build_debug.outcome == 'success' }}")
    check("a publish that does not exclude restored runs is caught",
          any("republish" in p for p in check_text(bad3)), True)

    # CROSS-MACHINE. The fixture carries BOTH the upload and the download,
    # because the defect karen found is only representable with both present.
    # A self-test fixture that cannot express the defect is a self-test that
    # cannot fail -- that is the lesson here, not the specific mutation.
    cross = good + (
        "\n      - name: Fetch the published build from ARTIFACTS"
        "\n        if: ${{ !cancelled() && steps.restore_probe.outputs.local_hit != 'true' && steps.find_build_artifact.outputs.run_id != '' }}"
        "\n        uses: actions/download-artifact@v4"
        "\n        with:"
        "\n" + _NAME_OK +
        "\n" + _PATH_OK +
        "\n          run-id: ${{ steps.find_build_artifact.outputs.run_id }}"
        "\n          github-token: ${{ github.token }}"
        "\n      - name: Upload the published build as an ARTIFACT"
        "\n        if: ${{ !cancelled() && inputs.publish_build_artifact != '' }}"
        "\n        uses: actions/upload-artifact@v4"
        "\n        with:"
        "\n" + _NAME_OK +
        "\n" + _PATH_OK + "\n")
    check("a sound cross-machine round trip passes", check_text(cross), [])

    # Mutate ONLY the download; leave the upload correct. Whole-file
    # containment passed exactly this.
    one_name = cross.replace(_NAME_OK, _NAME_BAD, 1)
    assert one_name != cross, "one-sided name mutation did not apply"
    check("a download repointed off the sha-keyed artifact is caught while the "
          "upload stays correct",
          any("not keyed on" in p or "disagree about the artifact name" in p
              for p in check_text(one_name)), True)

    one_path = cross.replace(_PATH_OK, _PATH_BAD, 1)
    assert one_path != cross, "one-sided path mutation did not apply"
    check("a download extracting outside the shared store is caught while the "
          "upload stays correct",
          any("does not extract into" in p or "disagree about" in p
              for p in check_text(one_path)), True)

    # THE INERT-WIRING DEFECT security-audit found. Every other check on this
    # fixture passes; only a rule that knows v4's PER-RUN artifact scoping
    # catches it. Two gates had already passed the wiring as self-consistent.
    check("a download-artifact@v4 with no run-id is caught as INERT",
          any("INERT" in p for p in
              check_text(cross.replace(
                  "\n          run-id: ${{ steps.find_build_artifact.outputs.run_id }}",
                  ""))), True)
    check("a download-artifact@v4 with no github-token is caught",
          any("github-token" in p for p in
              check_text(cross.replace(
                  "\n          github-token: ${{ github.token }}", ""))), True)
    check("a download not conditioned on the lookup FINDING a run is caught",
          any("having FOUND a run" in p for p in
              check_text(cross.replace(
                  " && steps.find_build_artifact.outputs.run_id != ''", ""))), True)

    check("downloading without checking the local store first is caught",
          any("local-store probe" in p for p in
              check_text(cross.replace(
                  "steps.restore_probe.outputs.local_hit != 'true'",
                  "always()"))), True)

    # A long comment must not break the checker. This is the regression, not a
    # hypothetical: the restore step is REQUIRED by F46 to carry an inline
    # PRODUCER-GUARD DECISION comment, adding that comment pushed the
    # commit-mismatch refusal past the old fixed 4000-character window, and the
    # checker reported the refusal missing while it sat plainly in the file. A
    # guard that fails because a comment grew teaches people to delete comments.
    padded = good.replace(
        "        id: restore_build\n",
        "        id: restore_build\n"
        + "".join("        # %s\n" % ("F46 PRODUCER-GUARD DECISION padding " * 3)
                  for _ in range(120)))
    check("a step with a very long comment block still passes",
          check_text(padded), [])
    # ...and the padded form must still be able to FAIL, or the check above
    # would be satisfied by a segment that simply matched nothing.
    padded_bad = padded.replace(
        'if ($x -ne "$env:GITHUB_SHA") { Write-Error "Refusing to test"; exit 1 }',
        'Write-Host "restored"')
    check("the long-comment form can still go red",
          any("commit mismatch" in p for p in check_text(padded_bad)), True)

    # The CHAINED-JOB shape. Same semantics, different spelling -- and a checker
    # that only knew the step-level spelling would have passed the restructured
    # workflow while every downstream guard silently skipped.
    chained = """
      - name: Consumer in another job
        if: ${{ !cancelled() && (needs.build.outputs.built == 'success' || needs.build.outputs.restored == 'success') }}
      - name: Build
        id: build_debug
        if: ${{ !cancelled() && needs.build.outputs.restored != 'success' }}
      - name: Restore
        id: restore_build
        run: |
          if ($x -ne "$env:GITHUB_SHA") { Write-Error "Refusing to test"; exit 1 }
      - name: Publish
        id: publish_build
        if: ${{ !cancelled() && needs.build.outputs.restored != 'success' && needs.build.outputs.built == 'success' }}
    """
    check("the chained-job spelling passes", check_text(chained), [])

    chained_bad = chained.replace(
        "(needs.build.outputs.built == 'success' || needs.build.outputs.restored == 'success')",
        "needs.build.outputs.built == 'success'")
    check("a chained consumer missing the restore term is caught",
          len(check_text(chained_bad)) >= 1, True)

    if failures:
        print("[build-reuse] self-test FAILED:", file=sys.stderr)
        for f in failures:
            print("  - " + f, file=sys.stderr)
        return 1
    print(f"[build-reuse] self-test: {checks} checks passed")
    return 0


def main(argv: list[str]) -> int:
    if "--self-test" in argv:
        return self_test()
    if not os.path.isfile(WORKFLOW):
        print(f"[build-reuse] {WORKFLOW} not found", file=sys.stderr)
        return 1
    with io.open(WORKFLOW, encoding="utf-8") as fh:
        problems = check_text(fh.read())
    if problems:
        print("[build-reuse] FAILED — the build-reuse wiring can lose coverage:")
        for p in problems:
            print("  * " + p)
        return 1
    print("[build-reuse] every producer guard accepts a restored build; "
          "build steps gate on restore failing; commit mismatch refused.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
