# -*- coding: utf-8 -*-
"""Discrimination controls for the tracked-tree path-hygiene guard.

The guard exists because F73 closed the PRODUCER and could not close the TREE: a
raw `--gtest_output=xml` capture copied into docs/ by hand never passes through
`run_ui_suite.py`'s scrubber, and 50 such files landed carrying the account name
on a repository that had been history-scrubbed to remove exactly that.

These cases pin BOTH directions, because this repository has shipped guards that
could never go red (decoration) and one that could only go red (noise). A guard
is only worth having when its output distinguishes the two states.
"""
from __future__ import annotations

import importlib.util
import sys
from pathlib import Path

import pytest

HERE = Path(__file__).resolve().parent
_SPEC = importlib.util.spec_from_file_location(
    "path_hygiene_under_test", HERE / "check_tracked_path_hygiene.py")
assert _SPEC and _SPEC.loader
ph = importlib.util.module_from_spec(_SPEC)
sys.modules["path_hygiene_under_test"] = ph
_SPEC.loader.exec_module(ph)

ACCOUNTS = {"alice"}


# ── RED: the states the guard must catch ────────────────────────────────

def test_the_real_leak_shape_is_caught():
    """THE DEFECT, as it actually appeared: a JUnit capture naming the account."""
    text = ('<testcase name="X" file="C:\\Users\\alice\\source\\PDFE\\tests\\'
            'test_x.cpp" />')
    hits, owners = ph.scan_text(text, ACCOUNTS)
    assert hits == ["alice"]
    assert owners == ["alice"]


def test_a_foreign_account_is_caught_even_though_it_is_not_this_machine():
    """A leak from another host is still a leak. Keying only on THIS machine's
    account would make the guard pass on any artifact imported from elsewhere."""
    hits, owners = ph.scan_text("/home/bob/build/out.xml", ACCOUNTS)
    assert hits == []
    assert owners == ["bob"]


@pytest.mark.parametrize("text,owner", [
    (r"C:\Users\carol\x", "carol"),        # backslash
    ("C:/Users/carol/x", "carol"),         # forward slash, as gtest emits
    (r"C:\\Users\\carol\\x", "carol"),     # doubled: the JSON-escaped form
    (r"c:\users\CAROL\x", "carol"),        # case-insensitive
])
def test_every_spelling_of_a_home_path_is_caught(text, owner):
    """DCA 0042 records that a detector which missed the escaped form let a leak
    read as clean. Each spelling is a separate way to be wrong."""
    _, owners = ph.scan_text(text, ACCOUNTS)
    assert owners == [owner]


def test_the_allowlist_is_by_name_and_does_not_widen():
    """`somedev` is allowed; `somedev2` is not. An allowlist matched by prefix or
    by resemblance would quietly admit real names that happen to look synthetic."""
    _, owners = ph.scan_text(r"C:\Users\somedev2\x", ACCOUNTS)
    assert owners == ["somedev2"]


# ── GREEN: the states the guard must NOT redden on ──────────────────────

def test_the_history_rewrite_placeholder_does_not_red():
    """141 tracked files legitimately contain `C:\\Users\\dev\\...`, including the
    scrub's own write-up. A guard that reds on correct content is worse than no
    guard, because it teaches people to ignore it."""
    hits, owners = ph.scan_text(r"C:\Users\dev\source\PDFE", ACCOUNTS)
    assert hits == []
    assert owners == []


def test_elisions_and_traversal_segments_are_not_names():
    """`C:\\Users\\...\\actions-runner` is prose; `C:\\Users\\..\\Windows` is a
    path-traversal fixture in test_crash_handler.cpp. Neither identifies anyone."""
    _, owners = ph.scan_text(r"C:\Users\...\actions-runner C:\Users\..\Windows",
                             ACCOUNTS)
    assert owners == []


def test_the_scrubbers_own_fixtures_do_not_red():
    """run_ui_suite.py and scrub_artifacts.py must contain the UNSCRUBBED shape --
    that is what their self-tests assert against."""
    _, owners = ph.scan_text(r"C:\Users\somedev\source\PDFE and c:/Users/me/d.pdf",
                             ACCOUNTS)
    assert owners == []


def test_keyboard_shortcut_prose_is_not_an_account():
    """`PgUp/Home/End` is a key list. Making the POSIX branch case-insensitive
    to match the Windows branch reported "end" and "pgup" as account names --
    a guard reddening on correct content, caught here."""
    _, owners = ph.scan_text("Ctrl+PgUp/Home/End navigates pages", ACCOUNTS)
    assert owners == []


def test_a_github_api_url_is_not_a_filesystem_path():
    """`https://api.github.com/users/ipswitch8` appears in a committed CI
    fixture. It is a public URL, not a home directory, and the owner name in it
    is the repository owner -- already public by construction."""
    _, owners = ph.scan_text(
        '"url":"https://api.github.com/users/ipswitch8","x":1', ACCOUNTS)
    assert owners == []


def test_a_real_posix_home_path_still_reds_after_that_narrowing():
    """The narrowing above must not have disarmed the POSIX branch. Preceded by
    a quote, a space, and at line start -- the shapes a real path appears in."""
    for text in ('"/home/bob/x"', " /home/bob/x", "/home/bob/x"):
        _, owners = ph.scan_text(text, ACCOUNTS)
        assert owners == ["bob"], (text, owners)


def test_a_short_account_name_is_not_searched_for():
    """A one- or two-character account would match inside ordinary words and
    drown the report. current_account_names() requires >= 3 chars."""
    hits, _ = ph.scan_text("the quick brown fox", {"ab"})
    assert hits == []


# ── The guard's own self-test must be wired and must pass ───────────────

def test_the_self_exemption_is_partial_and_narrow():
    """This detector and its tests must contain the shapes they detect, so the
    synthetic-owner half is waived FOR THEM ONLY -- and the account-name half
    stays armed, so the detector cannot become the one place a real leak hides.

    Pinned because a blanket skip is the obvious shortcut and is the wrong one.
    """
    assert ph.SELF_EXEMPT_OWNER_CHECK == {
        "tools/gate/check_tracked_path_hygiene.py",
        "tools/gate/test_check_tracked_path_hygiene.py",
    }, "the self-exemption must not widen beyond the detector and its tests"

    source = (HERE / "check_tracked_path_hygiene.py").read_text(encoding="utf-8")
    marker = "if _norm(path) in SELF_EXEMPT_OWNER_CHECK:"
    assert marker in source, "the exemption is not applied where it is claimed"
    tail = source[source.index(marker):source.index(marker) + 400]
    assert "owner_hits = []" in tail
    assert "account_hits = []" not in tail, (
        "the account-name half must remain armed for the exempt files")


def test_the_synthetic_names_used_here_still_red_in_any_other_file():
    """The exemption is by FILE, not by name: `/home/bob` committed anywhere
    else is still a leak."""
    _, owners = ph.scan_text("/home/bob/x and C:\\Users\\carol\\y", ACCOUNTS)
    assert owners == ["bob", "carol"]


def test_the_bundled_self_test_passes():
    assert ph._self_test() == 0


def test_the_repo_is_currently_clean():
    """Not a tautology: this is the assertion that would have FAILED at 3778b3b,
    and it is the one that fails again the next time a raw capture is committed."""
    assert ph.main([]) == 0


# ---------------------------------------------------------------------------
# The CI false-positive class.
#
# On the GitHub ubuntu runner USER=runner, and this repo has tools/runner-
# deployment, a self-hosted runner, runner_watchdog.ps1 and a runner-
# requirements manifest -- so "runner" as the needle matched 365 tracked files
# including the workflow running the check. fast-checks was red across five
# consecutive commits with every other failure in the job buried underneath it.
# ---------------------------------------------------------------------------

def test_a_service_account_is_never_used_as_the_needle():
    for name in ("runner", "root", "ubuntu", "Administrator", "jenkins", "GITHUB"):
        assert name.lower() in ph.GENERIC_ACCOUNTS, name


def test_service_account_env_yields_no_needle(monkeypatch):
    for var in ("USERNAME", "USER", "LOGNAME"):
        monkeypatch.delenv(var, raising=False)
    monkeypatch.setenv("USER", "runner")
    assert ph.current_account_names() == set()


def test_a_real_account_is_still_a_needle(monkeypatch):
    """The denylist must not swallow real names -- otherwise the fix for the
    false positives is a fix that disarms the check."""
    for var in ("USERNAME", "USER", "LOGNAME"):
        monkeypatch.delenv(var, raising=False)
    monkeypatch.setenv("USER", "alicedev")
    assert ph.current_account_names() == {"alicedev"}


def test_ci_with_no_personal_account_runs_the_home_path_half_not_a_vacuous_pass(monkeypatch):
    """Returning 0 here is only acceptable because the OTHER half still ran and
    the run says so. Verified separately that a planted /home/<owner> path in a
    tracked file still exits 1 under exactly this environment."""
    for var in ("USERNAME", "USER", "LOGNAME"):
        monkeypatch.delenv(var, raising=False)
    monkeypatch.setenv("USER", "runner")
    monkeypatch.setenv("GITHUB_ACTIONS", "true")
    # redirect_stdout rather than capsys: this repo's pytest config disables
    # capture, and capsys then errors out rather than simply capturing nothing.
    import contextlib, io as _io
    buf = _io.StringIO()
    with contextlib.redirect_stdout(buf):
        rc = ph.main([])
    out = buf.getvalue()
    assert rc == 0
    assert "CANNOT run here" in out, "a half that did not run must announce itself"
    assert "home-path half only" in out


def test_no_account_off_ci_still_fails_closed(monkeypatch):
    """The original fail-closed behaviour must survive on a developer machine.
    'Could not determine the account' passing vacuously is this repository's
    single most repeated defect shape."""
    for var in ("USERNAME", "USER", "LOGNAME", "CI", "GITHUB_ACTIONS",
                "TF_BUILD", "JENKINS_URL"):
        monkeypatch.delenv(var, raising=False)
    assert ph.main([]) == 1


# ---------------------------------------------------------------------------
# HOST-NAME RATCHET (GitHub #146)
#
# The account-name half is absolute; this half is a ratchet. Its two ways of
# being useless are opposite, so both are pinned:
#   * it never fires  -> the population grows unnoticed, which is the ONLY
#     failure mode that matters here (the existing 54 occurrences are accepted
#     on purpose);
#   * it fires on the accepted population -> it reddens on a correct tree, gets
#     muted, and is then not there for the real case (DCA 0047).
# ---------------------------------------------------------------------------


def _synthetic_needle():
    """A hashed needle for a made-up two-part machine name.

    REPLACES the real host name, which these tests used to contain -- ten
    occurrences across five lines, and the only reason this file needed an
    exemption from the very ratchet it tests. Hashed matching makes a real name
    unnecessary: the matcher cannot tell a real needle from a synthetic one, so
    a synthetic one proves it fires just as well, and the tree stops naming the
    machine.
    """
    import hashlib
    return [{"label": "synthetic-host",
             "sha256": hashlib.sha256(b"zzsynth host").hexdigest()}]


def test_a_new_occurrence_is_caught():
    """A file with more occurrences than the baseline allows must be reported."""
    n = _synthetic_needle()
    assert ph.scan_hostnames("nothing here", n) == 0
    assert ph.scan_hostnames("runs on zzsynth-host today", n) == 1
    # The needle must also match a LONGER runner name that embeds it. The old
    # implementation got that from substring matching; the hashed one gets it
    # from adjacent-PAIR hashing.
    assert ph.scan_hostnames("zzsynth-host and runner-zzsynth-host", n) == 2


def test_matching_is_case_insensitive():
    """The same machine reaches files in several casings, and inside a longer
    runner name that embeds the bare one."""
    n = _synthetic_needle()
    assert ph.scan_hostnames("ZZSYNTH-HOST", n) == 1
    assert ph.scan_hostnames("zzsynth-host", n) == 1
    assert ph.scan_hostnames("ZzSynth_Host", n) == 1


def test_separator_forms_all_reduce_to_the_same_needle():
    """The property hashing ADDED over the old substring match.

    Four spellings of one machine name were four different strings to a
    substring matcher, and the baseline would have had to enumerate them.
    Tokenised hashing collapses them -- and must still not fire on a different
    machine that merely shares one part.
    """
    n = _synthetic_needle()
    for form in ("zzsynth-host", "zzsynth_host", "zzsynth.host", "zzsynth host"):
        assert ph.scan_hostnames(form, n) == 1, form
    assert ph.scan_hostnames("zzsynth-other and someone-host", n) == 0

def test_the_real_baseline_loads_and_the_needle_is_present():
    """A baseline that silently fails to load would disarm the whole half.

    THE PROXY FOR 'IT LOADED' CHANGED ON 2026-10-19, and the old one would now
    assert the opposite of what it meant. This used to require `accepted_files`
    to be non-empty (>20 entries, against a recorded population of 36), on the
    reasoning that a baseline which failed to load would come back empty.

    The host name has since been scrubbed from every tracked file, so the
    accepted population is legitimately EMPTY -- which is the *strictest*
    possible state, not a load failure. Keeping the old assertion would have
    forced someone to re-accept occurrences to make a test pass.

    `hostnames` is the better proxy and always was: it is the needle the scan
    matches, so an empty list genuinely does disarm the check, whereas an empty
    accepted map merely means nothing is forgiven. Absence of the FILE is a
    separate case and is covered by the test below, which is what stops this
    one passing vacuously.
    """
    names, accepted = ph.load_hostname_baseline()
    assert names, "no host names in the baseline -- the ratchet would be inert"
    assert accepted is not None, (
        "accepted_files is None, which means the baseline did not load at all; "
        "an empty dict is fine and is the post-scrub expectation")
    assert isinstance(accepted, dict)


def test_a_missing_baseline_is_reported_not_silently_skipped():
    """load_hostname_baseline must signal absence, not return an empty list.

    Returning ([], {}) would read as 'no host names to check' and pass
    vacuously — the exact shape this checker's own header warns about.
    """
    names, accepted = ph.load_hostname_baseline("definitely/not/here.json")
    assert names is None and accepted is None


def test_nothing_exempts_itself_from_the_host_name_ratchet_any_more():
    """Replaces test_the_checker_exempts_itself_and_its_baseline.

    That test asserted the opposite: that the baseline and the checker were
    exempt, "because both must NAME the host name in order to police it". True
    of a PLAINTEXT needle, and it made those files the only tracked ones still
    carrying the machine name -- fine privately, a disclosure once the
    repository is public (GitHub #200).

    The needle is hashed as of 2026-10-24, so nothing needs exempting and the
    scanned population is strictly LARGER than before. Asserting the absence of
    the mechanism is the point: if someone reintroduces a plaintext needle and
    an exemption to hide it, this fails.
    """
    assert not hasattr(ph, "HOSTNAME_SELF_EXEMPT"), (
        "HOSTNAME_SELF_EXEMPT is back; a plaintext host-name needle in a "
        "tracked file is the exposure its removal closed")


def test_the_host_name_needle_is_hashed_not_plaintext():
    """The baseline must carry hashes, and must not carry the old key.

    A sha256 is 64 hex characters; a machine name is not. This is checkable
    without knowing the host name, which is the property that makes hashing
    work here at all.
    """
    import io as _io
    import json as _json
    import re as _re
    raw = _io.open(ph.HOSTNAME_BASELINE, encoding="utf-8").read()
    data = _json.loads(raw)
    assert "hostnames" not in data or not data["hostnames"], (
        "the plaintext 'hostnames' key is back in the baseline")
    needles = data.get("hostname_needles")
    assert needles, "the baseline carries no hashed needle; the ratchet is inert"
    for n in needles:
        assert _re.fullmatch(r"[0-9a-f]{64}", n["sha256"]), (
            "needle %r is not a sha256 -- a plaintext needle would be a leak"
            % n.get("label"))


def test_a_plaintext_baseline_fails_closed_rather_than_scanning_nothing():
    """A reverted baseline must FAIL, not report a clean tree.

    This is the shape the checker's own header warns about: a guard that cannot
    find its needle and says OK is worse than no guard.
    """
    import io as _io
    import json as _json
    import tempfile
    import os as _os
    data = _json.loads(_io.open(ph.HOSTNAME_BASELINE, encoding="utf-8").read())
    data.pop("hostname_needles", None)
    data["hostnames"] = ["SOME-HOST"]
    fd, tmp = tempfile.mkstemp(suffix=".json")
    _os.close(fd)
    try:
        _io.open(tmp, "w", encoding="utf-8").write(_json.dumps(data))
        names, accepted = ph.load_hostname_baseline(tmp)
        assert names is None and accepted is None, (
            "a plaintext baseline was accepted; it must fail closed")
    finally:
        _os.unlink(tmp)


def test_the_two_halves_tokenise_identically():
    """The host-name matcher must agree with the account-name matcher.

    Both guards tokenise into maximal alphanumeric runs and hash each token
    plus each space-joined adjacent pair. They are separate implementations in
    sibling files, so without this they can drift -- and a drift would mean one
    half silently stops catching a form the other still does.
    """
    import hashlib as _h
    import importlib.util as _u
    import os as _os
    spec = _u.spec_from_file_location(
        "_scrub", _os.path.join(_os.path.dirname(_os.path.abspath(ph.__file__)),
                                "check_scrub_regressions.py"))
    scrub = _u.module_from_spec(spec)
    spec.loader.exec_module(scrub)

    sample = "alpha-beta gamma_delta EPSILON"
    want = sorted({t.lower() for t in scrub._TOKEN_RE.findall(sample)})
    got = sorted({t.lower() for t in ph._HOST_TOKEN_RE.findall(sample)})
    assert want == got, "the two tokenisers disagree: %r vs %r" % (want, got)

    # And the pair rule: a synthetic two-part name must be found by the host
    # matcher when its PAIR hash is the needle -- proving the matcher fires.
    pair = _h.sha256(b"zzsynth host").hexdigest()
    needles = [{"label": "synthetic", "sha256": pair}]
    assert ph.scan_hostnames("runner zzsynth-host-01 online", needles) == 1, (
        "the pair needle did not match inside a longer runner-style name")
    assert ph.scan_hostnames("nothing to see here", needles) == 0, (
        "the matcher fired on a clean line")


def test_the_accepted_population_does_not_redden_the_guard():
    """Every baselined file must be at or under its recorded count on this tree.

    This is the DCA 0047 half: a ratchet that fires on the population it just
    accepted is a guard that gets muted.
    """
    import io as _io
    names, accepted = ph.load_hostname_baseline()
    over = []
    for path, allowed in accepted.items():
        try:
            text = _io.open(path, encoding="utf-8", errors="replace").read()
        except OSError:
            continue
        found = ph.scan_hostnames(text, names)
        if found > allowed:
            over.append((path, found, allowed))
    assert not over, f"baselined files now exceed their recorded counts: {over}"


# --- p101 phase-6: a needle-matching FOREIGN owner must be redacted ----------
#
# The diagnostic printed the leaked home-path owner verbatim when it was
# foreign to the ambient account -- which on CI (where the leaked owner is the
# build account, foreign to the runner's own account) wrote the account name
# straight into the Actions log, a scanned publication surface. shown_owners()
# now withholds any owner whose sha256 is a scrub needle. These pin that so a
# revert reddens instead of silently re-leaking.

def _account_needle_hashes():
    """The real scrub-needle hash set, via the guard's own loader."""
    return ph._load_scrub_needle_hashes()


def test_a_foreign_owner_that_is_a_scrub_needle_is_redacted():
    import hashlib
    owner = "zzsynthacct"
    nh = {hashlib.sha256(owner.encode("utf-8")).hexdigest()}
    shown, redacted = ph.shown_owners([owner], accounts=set(), needle_hashes=nh)
    assert shown == [], "a needle-matching owner must never be printed"
    assert redacted == 1


def test_a_foreign_owner_that_is_NOT_a_needle_is_still_named():
    # The diagnostic must still identify a genuinely foreign owner that is not
    # a forbidden identifier -- that is the legitimate signal.
    shown, redacted = ph.shown_owners(["strangerxyz"], accounts=set(),
                                      needle_hashes=set())
    assert shown == ["strangerxyz"]
    assert redacted == 0


def test_ambient_account_owner_is_redacted_even_without_a_needle_match():
    shown, redacted = ph.shown_owners(["alice"], accounts={"alice"},
                                      needle_hashes=set())
    assert shown == [] and redacted == 1


def test_the_real_scrub_needle_set_loads_and_drives_redaction():
    # The real scrub-needle hash set must be non-empty (an empty set would make
    # the whole redaction a no-op), and membership in it must withhold a
    # foreign owner regardless of the ambient account set -- the exact CI shape
    # (leaked owner is the build account, foreign to the runner). We cannot
    # forge the account's plaintext (by design), so exercise the property with
    # a synthetic owner whose hash we place in the set.
    import hashlib
    assert _account_needle_hashes(), \
        "scrub_needles.json yielded no hashes -- redaction would be a no-op"
    owner = "whatever"
    nh = {hashlib.sha256(owner.encode("utf-8")).hexdigest()}
    shown, redacted = ph.shown_owners([owner], accounts=set(), needle_hashes=nh)
    assert shown == [] and redacted == 1
