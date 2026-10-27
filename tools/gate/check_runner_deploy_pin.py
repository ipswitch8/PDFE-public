# -*- coding: utf-8 -*-
"""Keep the runner-deployment bootstrap honest about what it will fetch.

WHY THIS EXISTS. `Bootstrap-PdfeRunner.ps1` is the one file copied by hand to a
new machine; it fetches the rest of `tools/runner-deployment/` from GitHub at a
PINNED COMMIT, from a hand-maintained file list. Both of those have now gone
stale in practice, and neither failure is visible on the machine that made it:

  * THE PIN. It sat at c136529 while six later commits changed the very scripts
    it fetches. A machine bootstrapped from that pin would have received
    Install-PdfeRunner.ps1 with three shape checks that never fired, a URL probe
    that calls a good installer dead on one TCP timeout, and no dry-run harness
    at all -- while reporting a completely successful bootstrap. A stale pin is
    not a safe default; it is a silently OLD deployment.
  * THE FILE LIST. `$ExpectedFiles` is hand-maintained and missed
    Test-InstallerDryRun.py. The script has a runtime drift check, but it is a
    deploy-time WARNING on the new machine -- by which point the operator is
    already provisioning with an incomplete package.

Both become errors here, at commit time, on the machine that can still fix them.

Run:
    python tools/gate/check_runner_deploy_pin.py [--self-test]
"""
from __future__ import annotations

import io
import os
import re
import sys
import subprocess

DEPLOY_DIR = os.path.join("tools", "runner-deployment")
BOOTSTRAP = os.path.join(DEPLOY_DIR, "Bootstrap-PdfeRunner.ps1")
# The bootstrap is copied by hand, never fetched, and cannot contain its own
# SHA -- so it is excluded from the pin-currency comparison by construction.
SELF = "Bootstrap-PdfeRunner.ps1"


def _git(*args: str, cwd: str | None = None) -> tuple[int, str]:
    """Run git, optionally in `cwd`.

    The `cwd` parameter exists because its absence was a real defect, found by
    the `validator` gate of phase-6 while verifying this very checker.
    `check()` takes a `repo_root` and used it for the bootstrap's FILE path
    while every git query here ran in the PROCESS cwd -- so a caller passing a
    root got answers about the bootstrap in one repository and the history of
    whichever repository the shell happened to be in.

    It was masked because the only two callers ran with cwd == repo_root: the
    workflow invokes this from the repo root, and the pytest suite passes
    `str(ROOT)` while pytest's cwd is also the root. The parameter was honoured
    in one half and silently ignored in the other -- the same "wired into one
    of two call paths" shape this project has recorded before.
    """
    p = subprocess.run(["git", *args], capture_output=True, text=True,
                       encoding="utf-8", errors="replace", cwd=cwd)
    return p.returncode, (p.stdout or "") + (p.stderr or "")


def parse_bootstrap(text: str) -> tuple[str, list[str]]:
    """Return (pinned_ref, expected_files) as the script declares them."""
    m = re.search(r"\[string\]\$Ref\s*=\s*'([0-9a-fA-F]+)'", text)
    ref = m.group(1) if m else ""
    block = re.search(r"\$ExpectedFiles\s*=\s*@\((.*?)\)", text, re.S)
    files = re.findall(r"'([^']+)'", block.group(1)) if block else []
    return ref, files


def _previous_pin(current: str, repo_root: str = ".") -> str:
    """The pin's value at the most recent commit where it DIFFERED from `current`.

    Walks the bootstrap's own history rather than comparing against HEAD~: the
    pin usually stays put across many commits that touch the file for other
    reasons, so "the previous commit's value" is almost always the same value
    and would make Rule B inert.

    Returns "" when no earlier distinct value exists, or when the earlier value
    is not a commit on this history (already covered by Rule A, and not
    comparable with merge-base).
    """
    rc, out = _git("log", "--format=%H", "--", BOOTSTRAP, cwd=repo_root)
    if rc != 0:
        return ""
    for sha in out.split():
        # FORWARD SLASHES ARE MANDATORY HERE. `BOOTSTRAP` comes from
        # os.path.join, so on Windows it carries backslashes -- and git's
        # `<rev>:<path>` revision syntax does not accept them: `git show
        # <sha>:tools\runner-deployment\...` exits 128 with "does not exist in
        # <sha>".
        #
        # That made every blob read in this loop fail, so the function always
        # returned "" and RULE B WAS INERT ON WINDOWS -- the pin could move
        # backwards and nothing would say so. It looked like a working guard:
        # the checker still exited 0, because an inert rule adds no problems.
        # `test_rule_b_moving_the_pin_backwards_is_caught` is what exposed it,
        # by asserting the complaint rather than the exit code.
        rc2, blob = _git("show", f"{sha}:{BOOTSTRAP.replace(os.sep, '/')}", cwd=repo_root)
        if rc2 != 0:
            continue
        m = re.search(r"\[string\]\$Ref\s*=\s*'([0-9a-fA-F]+)'", blob)
        if not m:
            continue
        value = m.group(1)
        if value == current:
            continue
        rc3, _ = _git("cat-file", "-e", f"{value}^{{commit}}", cwd=repo_root)
        if rc3 != 0:
            return ""          # dead SHA: Rule A's business, not comparable here
        rc4, _ = _git("merge-base", "--is-ancestor", value, "HEAD", cwd=repo_root)
        if rc4 != 0:
            return ""          # abandoned history: likewise
        return value
    return ""


def check(repo_root: str = ".") -> list[str]:
    problems: list[str] = []
    path = os.path.join(repo_root, BOOTSTRAP)
    if not os.path.isfile(path):
        return [f"{BOOTSTRAP} not found"]
    with io.open(path, encoding="utf-8") as fh:
        ref, expected = parse_bootstrap(fh.read())

    if not ref:
        problems.append(f"{BOOTSTRAP}: could not find the pinned $Ref.")
    if not expected:
        problems.append(f"{BOOTSTRAP}: could not find $ExpectedFiles.")
    if problems:
        return problems

    # --- the file list must match what is actually tracked ------------------
    rc, out = _git("ls-files", DEPLOY_DIR, cwd=repo_root)
    if rc != 0:
        return [f"git ls-files failed: {out.strip()}"]
    tracked = {os.path.basename(p) for p in out.split() if p.strip()}
    tracked.discard(SELF)
    missing = sorted(tracked - set(expected))
    extra = sorted(set(expected) - tracked)
    if missing:
        problems.append(
            f"{BOOTSTRAP}: $ExpectedFiles does not list {missing}. A machine "
            f"bootstrapped now would be provisioned WITHOUT those files and "
            f"would report a successful bootstrap anyway.")
    if extra:
        problems.append(
            f"{BOOTSTRAP}: $ExpectedFiles lists {extra}, which are not tracked "
            f"under {DEPLOY_DIR}. The fetch will 404 on them.")

    # --- the pin must be current for everything it fetches ------------------
    rc, _ = _git("cat-file", "-e", f"{ref}^{{commit}}", cwd=repo_root)
    if rc != 0:
        problems.append(
            f"{BOOTSTRAP}: pinned ref {ref!r} is not a commit in this "
            f"repository. The bootstrap would fail to fetch anything.")
        return problems

    # --- RULE A: the pin must be an ANCESTOR of HEAD (GitHub #129) ----------
    #
    # `cat-file -e` above only proves the object EXISTS. That is a weaker
    # statement than it looks in this repository, which has had TWO history
    # rewrites (2026-08-22 and 2026-08-27) that re-hashed every commit. After a
    # rewrite the abandoned objects linger in the local object store -- and
    # GitHub kept serving them by SHA for some time afterwards -- so a pin at a
    # dead SHA passes an existence check while pointing at a history nobody is
    # on any more.
    #
    # This is not hypothetical here. Tracing the pin's own history:
    #
    #     fed52797  2026-08-26  pin=fc0af9f    NOT an ancestor of HEAD
    #     5356cd78  2026-08-26  pin=029cf0e    NOT an ancestor of HEAD
    #
    # Both were live in the tree at the time and both are dead now. A machine
    # bootstrapped from either would fetch nothing, or fetch from an abandoned
    # history, and report success.
    rc, _ = _git("merge-base", "--is-ancestor", ref, "HEAD", cwd=repo_root)
    if rc != 0:
        problems.append(
            f"{BOOTSTRAP}: pinned ref {ref!r} EXISTS but is NOT an ancestor of "
            f"HEAD. It points at an abandoned, rewritten or fabricated commit "
            f"-- this repository has had two history rewrites, and dead objects "
            f"survive both locally and on the remote for a while, so an "
            f"existence check is not enough.\n"
            f"    FIX: repin to a commit on the current history.")
        return problems

    # --- RULE B: the pin must never move BACKWARDS (GitHub #129) ------------
    #
    # THE MECHANISM THIS EXISTS FOR, and it is not the one the file's own
    # warning anticipated. Commit 8cc495be ("remap the SHA citations the
    # account-name history rewrite invalidated") rewrote this pin from fc0af9f
    # to 5f7680a -- BACKWARDS -- because a bare 7-hex string in a .ps1 is
    # indistinguishable from a documentation citation to a regex.
    #
    # It survived a month unnoticed, and neither existing rule could see it:
    # the new SHA exists, it is an ancestor of HEAD, and the package files
    # happened not to change in between, so the staleness diff below was empty.
    # Nothing was wrong with the RESULT; what was wrong was the DIRECTION.
    #
    # A pin only ever advances. Moving it to an ancestor of its own previous
    # value is always either a mistake or a deliberate rollback that should be
    # explicit -- so it is reported rather than silently accepted.
    prev = _previous_pin(ref, repo_root)
    if prev:
        rc_anc, _ = _git("merge-base", "--is-ancestor", ref, prev, cwd=repo_root)
        rc_same = (ref == prev)
        if rc_anc == 0 and not rc_same:
            problems.append(
                f"{BOOTSTRAP}: the pin MOVED BACKWARDS -- {ref!r} is an "
                f"ancestor of the previous pin {prev!r}. A pin only advances; "
                f"going backwards silently reverts every package change made in "
                f"between. This is how commit 8cc495be broke it: an automated "
                f"SHA remap could not tell a live pin from a documentation "
                f"citation.\n"
                f"    FIX: repin forwards. A DELIBERATE rollback is legitimate "
                f"(reverting a broken package pin is a real need) but this "
                f"check cannot tell one from an accident, so it has no "
                f"override: do the rollback as a forward pin to a commit that "
                f"restores the older package contents.\n"
                f"    This message previously promised that explaining the "
                f"rollback in the commit message would satisfy the check. It "
                f"never did -- nothing here reads commit messages. The phase-10 "
                f"security-audit gate caught the discrepancy; a guard that "
                f"advertises an escape hatch it does not implement is one "
                f"people learn to bypass wholesale.")

    rc, out = _git("diff", "--name-only", ref, "HEAD", "--",
                   DEPLOY_DIR, f":!{BOOTSTRAP}", cwd=repo_root)
    if rc != 0:
        problems.append(f"git diff against {ref} failed: {out.strip()}")
    else:
        drifted = sorted(os.path.basename(p) for p in out.split() if p.strip())
        if drifted:
            problems.append(
                f"{BOOTSTRAP}: the pin {ref!r} is STALE -- {drifted} changed "
                f"after it. A machine bootstrapped from this pin gets the OLD "
                f"scripts and says nothing about it.\n"
                f"    FIX: commit the script change first, then a SECOND commit "
                f"setting $Ref to the first one's SHA. A commit cannot contain "
                f"its own SHA, so the two-step is inherent, not an oversight -- "
                f"and this check being red in between is CORRECT: during that "
                f"window the bootstrap really would deploy stale scripts.")
    return problems


def self_test() -> int:
    checks = 0
    failures: list[str] = []

    def ck(label, got, want):
        nonlocal checks
        checks += 1
        if got != want:
            failures.append(f"{label}: got {got!r}, want {want!r}")

    good = """
    [string]$Ref = 'abc1234',
    $ExpectedFiles = @(
        'a.ps1',
        'b.json'
    )
    """
    ref, files = parse_bootstrap(good)
    ck("parses the pinned ref", ref, "abc1234")
    ck("parses the file list", files, ["a.ps1", "b.json"])

    # A bootstrap with no pin at all must not parse as "fine".
    ref2, _ = parse_bootstrap(good.replace("'abc1234'", "$env:SOMETHING"))
    ck("an unparseable pin is reported as empty, not guessed", ref2, "")

    block = parse_bootstrap("$ExpectedFiles = @(\n)\n")[1]
    ck("an empty file list parses as empty", block, [])

    if failures:
        print("[runner-deploy-pin] self-test FAILED:", file=sys.stderr)
        for f in failures:
            print("  - " + f, file=sys.stderr)
        return 1
    print(f"[runner-deploy-pin] self-test: {checks} checks passed")
    return 0


def main(argv: list[str]) -> int:
    if "--self-test" in argv:
        return self_test()
    problems = check()
    if problems:
        print("[runner-deploy-pin] FAILED — a new runner would be provisioned "
              "from a stale or incomplete package:")
        for p in problems:
            print("  * " + p)
        return 1
    print("[runner-deploy-pin] the bootstrap's file list matches the tracked "
          "package and its pin is current for every file it fetches.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
