// Direct tests for ConformanceChecker's path scrubber.
//
// WHY THESE EXIST (claim-check, phase-1, 2026-10-06)
// ==================================================
// `UiSuitePathHygiene.TheTrackedConformanceRecordIsPortable` asserts that the
// COMMITTED record `docs/audit/.../phase-14/conformance-checker-record.json`
// contains no absolute path. That is the right thing to assert, but it passes
// whenever the committed file happens to be portable -- including when it is
// simply STALE and was written before the scrubber changed. The test's green is
// therefore decoupled from the behaviour it appears to certify.
//
// That decoupling was not hypothetical. The scrubber knew `<repo>`, `<home>` and
// `<temp>`; veraPDF is installed at `C:\verapdf` on the CI host, which is none
// of those, so `jar_path` was written VERBATIM into the tracked file and the
// test failed for real. The fix (a backstop collapsing any still drive-qualified
// path to `<path>`) was then verified only on a gitignored sidecar, because the
// tracked file was restored rather than regenerated.
//
// So these call `portablePath()` directly. They do not depend on which machine
// ran them, on whether veraPDF is installed, or on whether any artifact was
// regenerated -- which is what makes them evidence for the claim rather than
// evidence about a file.

#include "support/ConformanceChecker.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <string>

namespace {

// PIN USERPROFILE FOR THE DURATION OF A TEST.
//
// GitHub #274, found by the now-skip-aware magnitude guard on 2026-10-07. The
// account-name tests below used to read the REAL account name and
// `GTEST_SKIP()` when it was shorter than the scrubber's 3-character bound.
// That is the exact shape #274 is about -- `if (user.size() < 3) GTEST_SKIP()`
// -- and I introduced three fresh instances of it in the same commit that added
// the scrubber. A test that stops running on some hosts is not a control on
// those hosts, and it is counted as a non-failure everywhere.
//
// `portablePath()` reads USERPROFILE itself, so the dependency is removable
// rather than merely tolerable: pin the variable and the tests become
// deterministic on every machine, with no skip and no reliance on who is logged
// in. The short-name boundary then becomes an ASSERTION (see
// TheAccountNamePassIsBoundedBelowAtThreeCharacters) instead of the reason the
// other tests do not run.
class ScopedUserProfile {
public:
    explicit ScopedUserProfile(const std::string& value) {
        const char* prev = std::getenv("USERPROFILE");
        had_ = prev != nullptr;
        if (had_) previous_ = prev;
        set("USERPROFILE", value.c_str());
    }
    ~ScopedUserProfile() {
        // Restore exactly, including the absent case: leaving a pinned value
        // behind would silently change every later test in this process.
        set("USERPROFILE", had_ ? previous_.c_str() : "");
    }
    ScopedUserProfile(const ScopedUserProfile&) = delete;
    ScopedUserProfile& operator=(const ScopedUserProfile&) = delete;

private:
    static void set(const char* name, const char* value) {
#if defined(_WIN32)
        _putenv_s(name, value);
#else
        if (value == nullptr || *value == '\0') ::unsetenv(name);
        else ::setenv(name, value, 1);
#endif
    }
    bool had_ = false;
    std::string previous_;
};

// A deterministic stand-in, long enough to clear the >= 3 bound and distinctive
// enough that a stray match would be obvious rather than coincidental.
const char* const kPinnedProfile = "D:\\pinned\\zqaccount";
const char* const kPinnedAccount = "zqaccount";

bool hasDriveQualifiedPath(const std::string& s) {
    for (std::size_t i = 0; i + 2 < s.size(); ++i) {
        const bool alpha = (s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= 'a' && s[i] <= 'z');
        if (alpha && s[i + 1] == ':' && (s[i + 2] == '\\' || s[i + 2] == '/'))
            return true;
    }
    return false;
}

}  // namespace

// THE CASE THAT ACTUALLY FAILED IN CI.
TEST(ConformancePathScrub, AnInstallRootOutsideEveryKnownRootIsStillScrubbed) {
    const std::string raw = R"(C:\verapdf\bin\cli-1.30.2.jar)";
    const std::string out = pdfe::test::portablePath(raw);

    EXPECT_FALSE(hasDriveQualifiedPath(out))
        << "a path under an install root that is neither <repo>, <home> nor "
           "<temp> survived the scrubber verbatim, which is how the account "
           "name reached a git-tracked file. Got: " << out;
    EXPECT_EQ(out.find("verapdf"), std::string::npos)
        << "the install directory name survived: " << out;
}

TEST(ConformancePathScrub, ForwardSlashFormIsScrubbedToo) {
    const std::string out = pdfe::test::portablePath("C:/verapdf/bin/cli.jar");
    EXPECT_FALSE(hasDriveQualifiedPath(out)) << out;
}

// NEGATIVE CONTROL. A scrubber that eats everything is not a scrubber; the
// non-path text around a path has to survive, or a diagnostic becomes unreadable
// and someone turns the scrubbing off.
TEST(ConformancePathScrub, SurroundingProseSurvives) {
    const std::string out =
        pdfe::test::portablePath(R"(resolved jar at C:\verapdf\bin\cli.jar ok)");
    EXPECT_NE(out.find("resolved jar at "), std::string::npos) << out;
    EXPECT_NE(out.find(" ok"), std::string::npos) << out;
    EXPECT_FALSE(hasDriveQualifiedPath(out)) << out;
}

// REGRESSION. `SurroundingProseSurvives` checks words either side of a path and
// passed while the scrubber was eating the punctuation touching one. Measured on
// the tracked record itself:
//
//   before  "install root <repo>\tools\conformance\verapdf; java resolved to java"
//   after   "install root <path> java resolved to java"
//
// The `;` went into the path token (the loop stopped at the following space, so
// the space survived and the semicolon did not). A scrubber that silently
// rewrites prose is the "eats everything" failure in miniature, and it lands in
// a GIT-TRACKED diagnostic where nobody would reread it.
TEST(ConformancePathScrub, PunctuationTouchingAPathSurvives) {
    const std::string out = pdfe::test::portablePath(
        R"(install root C:\verapdf; java resolved to java)");

    EXPECT_NE(out.find(";"), std::string::npos)
        << "the semicolon adjacent to the path was consumed into the path "
           "token. `;` is the Windows PATH delimiter and is never part of a "
           "single path. Got: " << out;
    EXPECT_NE(out.find("install root "), std::string::npos) << out;
    EXPECT_NE(out.find(" java resolved to java"), std::string::npos) << out;
    EXPECT_FALSE(hasDriveQualifiedPath(out)) << out;
}

// Stopping early must not become a leak: the text after the stop character is
// re-examined, so a SECOND drive-qualified path still gets scrubbed. This is the
// property that makes `;` safe to stop at.
TEST(ConformancePathScrub, ASecondPathAfterASemicolonIsAlsoScrubbed) {
    const std::string out =
        pdfe::test::portablePath(R"(C:\verapdf\bin;C:\other\tool\x.jar)");

    EXPECT_FALSE(hasDriveQualifiedPath(out))
        << "a path following a semicolon escaped the scrubber, so stopping at "
           "`;` traded a prose bug for a leak. Got: " << out;
    EXPECT_EQ(out.find("verapdf"), std::string::npos) << out;
    EXPECT_EQ(out.find("other"), std::string::npos) << out;
    EXPECT_NE(out.find(";"), std::string::npos) << out;
}

// `)` must NOT be a stop character -- this is an ordinary Windows path, and
// stopping there would emit `<path>` followed by a literal remainder.
TEST(ConformancePathScrub, ParenthesesInsideAPathDoNotTerminateTheToken) {
    const std::string out =
        pdfe::test::portablePath(R"(C:\Program Files (x86)\verapdf\cli.jar)");

    // The space inside "Program Files" legitimately ends the token, so assert
    // the part that matters: no drive-qualified prefix survives, and the
    // scrubber did not emit a literal "(x86)\verapdf" tail with a drive letter.
    EXPECT_FALSE(hasDriveQualifiedPath(out)) << out;
}

// CLAIM-CHECK ROUND 4 (C9). The round-3 fix was justified as "punctuation
// adjacent to a path survives", but only `;` had been added and tested. The gate
// probed the general claim with a trailing COLON and it failed on unmodified
// code:
//
//   portablePath(R"(Error: C:\verapdf\bin\cli.jar: No such file)")
//       -> "Error: <path> No such file"
//
// Worse than the semicolon case, because `checkObjectJson()` scrubs raw captured
// subprocess output and "path: message" is the ordinary tool-diagnostic shape.
// The stop set is now derived from Win32's forbidden path-component characters
// rather than extended one incident at a time.
TEST(ConformancePathScrub, ATrailingColonTouchingAPathSurvives) {
    const std::string out =
        pdfe::test::portablePath(R"(Error: C:\verapdf\bin\cli.jar: No such file)");

    EXPECT_EQ(out, "Error: <path>: No such file")
        << "the colon terminating the path was consumed into the path token, so "
           "a tool diagnostic reads as one run-on sentence in a git-tracked "
           "file. Got: " << out;
    EXPECT_FALSE(hasDriveQualifiedPath(out)) << out;
}

// THE REGRESSION THE `i + 2` SCAN START EXISTS TO PREVENT. Making `:` a stop
// character while scanning from `i` would halt on the DRIVE's own colon, emit
// `<path>`, and leave `:\verapdf\...` as literal text -- converting a prose bug
// into an actual path leak. This is the test that fails if that start is moved.
TEST(ConformancePathScrub, TheDriveColonIsNotTreatedAsATerminator) {
    const std::string out = pdfe::test::portablePath(R"(C:\verapdf\bin\cli.jar)");

    EXPECT_EQ(out, "<path>")
        << "the whole path should collapse to a single token. A result like "
           "\"<path>:\\verapdf\\...\" means the scan started before the drive "
           "colon and the remainder leaked. Got: " << out;
    EXPECT_EQ(out.find("verapdf"), std::string::npos) << out;
}

TEST(ConformancePathScrub, ACommaTouchingAPathSurvives) {
    const std::string out =
        pdfe::test::portablePath(R"(searched C:\verapdf\bin, then gave up)");

    EXPECT_NE(out.find(","), std::string::npos)
        << "the comma was eaten, same class as the `;` and `:` cases: " << out;
    EXPECT_NE(out.find(" then gave up"), std::string::npos) << out;
    EXPECT_FALSE(hasDriveQualifiedPath(out)) << out;
}

// Win32 forbids these in a path COMPONENT, so they can never be interior to one
// and always end the token. Table-driven so adding one to the set without a test,
// or silently dropping one, shows up here rather than in a tracked diagnostic
// months later.
TEST(ConformancePathScrub, EveryWin32ForbiddenCharacterTerminatesThePathToken) {
    for (const char c : {'<', '>', ':', '"', '|', '?', '*'}) {
        const std::string raw =
            std::string(R"(C:\verapdf\bin)") + c + std::string("tail");
        const std::string out = pdfe::test::portablePath(raw);

        EXPECT_NE(out.find(c), std::string::npos)
            << "character '" << c << "' was consumed into the path token; it "
               "cannot be interior to a Windows path component. Got: " << out;
        EXPECT_NE(out.find("tail"), std::string::npos)
            << "text after '" << c << "' was lost. Got: " << out;
        EXPECT_FALSE(hasDriveQualifiedPath(out)) << out;
    }
}

// CLAIM-CHECK ROUND 5 (C10). Round 4's comment claimed `'` was deliberately NOT a
// stop character; its code made it one, and the table test above omitted it on
// the strength of the comment. The result, on the unmodified tree:
//
//   portablePath(R"(C:\v\O'Neill\cli.jar)") -> "<path>'Neill\cli.jar"
//
// The token halted MID-COMPONENT and stranded the rest of the path. `O'Neill` is
// an ordinary legal Windows directory and account name. These characters are all
// legal in a path and must never end a token.
//
// THE EXPECTED FRAGMENT MUST BE A SUBSTRING OF THE INPUT, or the case asserts
// nothing. karen caught exactly that here: the original case used `O'Brien`, a
// later rename made the input `O'Neill` to satisfy
// check_tracked_path_hygiene.py (a literal `C:\Users\<name>` in a tracked file),
// and the expected fragment was left as "Brien" -- a string that cannot appear in
// the output whatever the scrubber does. The apostrophe mutation then killed
// NOTHING and the suite still read 15/15. A renamed fixture that silences its own
// oracle is the vacuous-oracle shape, and it was invisible from the green.
TEST(ConformancePathScrub, CharactersLegalInAPathDoNotTerminateTheToken) {
    struct Case { const char* raw; const char* leaked; };
    for (const Case& c : {
             Case{R"(C:\v\O'Neill\cli.jar)", "Neill"},
             Case{R"(C:\a&b\cli.jar)",       "cli.jar"},
             Case{R"(C:\v\(x86)\cli.jar)",   "x86"},
         }) {
        // GUARD THE GUARD: a fragment that is not in the input makes the
        // EXPECT_EQ below pass for free. This is the assertion that would have
        // failed the rename instead of letting it through.
        ASSERT_NE(std::string(c.raw).find(c.leaked), std::string::npos)
            << "the expected leaked fragment \"" << c.leaked << "\" does not "
               "occur in the input \"" << c.raw << "\", so this case cannot "
               "detect stranding and asserts nothing";
        const std::string out = pdfe::test::portablePath(c.raw);

        EXPECT_EQ(out.find(c.leaked), std::string::npos)
            << "the token stopped inside the path and stranded \"" << c.leaked
            << "\" verbatim -- the fragment-stranding leak C10 found. Input: "
            << c.raw << "  Got: " << out;
        EXPECT_FALSE(hasDriveQualifiedPath(out)) << out;
    }
}

// A SPACE INSIDE A PATH *DOES* STRAND THE REMAINDER, AND THAT IS A DOCUMENTED
// LIMIT RATHER THAN A FIXABLE BUG. Found by the test above while it still had a
// "C:\Program Files (x86)\..." case in it:
//
//   portablePath(R"(C:\Program Files (x86)\v\cli.jar)")
//       -> "<path> Files (x86)\v\cli.jar"
//
// An unquoted Windows path containing spaces cannot be delimited from the prose
// around it: "root C:\a b" is either one path with a space or a path followed by
// a word, and nothing in the string distinguishes them. Consuming to end-of-line
// would eat whole sentences; stopping at the space strands a tail. This chooses
// to stop.
//
// The limit is BOUNDED, and this test pins the bound so it cannot quietly widen:
//   1. a stranded tail is never drive-qualified, so the tracked-record
//      portability assertion still holds; and
//   2. `<home>`/`<repo>`/`<temp>` are substituted BEFORE this backstop runs, so
//      a path under the operator's profile is replaced wholesale and the ACCOUNT
//      NAME is gone before any space is reached -- which is the string that must
//      never reach a tracked file.
// GitHub #283. A SPACE INSIDE A PATH NO LONGER STRANDS THE TAIL.
//
// This test previously asserted the opposite, and asserted it deliberately:
// `C:\Program Files (x86)\v\cli.jar` came out as
// `<path> Files (x86)\v\cli.jar`, and the bound was pinned here so it could not
// quietly widen. The reasoning for accepting it was that
// `C:\Program Files\x` and `C:\tmp\a and then some prose` are
// indistinguishable without quoting, so consuming past a space would eat
// sentences.
//
// That was a false dichotomy. "Consume past every space" is not the only
// alternative to "stop at the first one": a space is INTERIOR when a later part
// of the same run still carries a path SEPARATOR, which is exactly what
// separates `Program Files (x86)\v\cli.jar` from `and then some prose`. The
// bound moved; it did not disappear, and the cases below pin where it now sits.
TEST(ConformancePathScrub, ASpaceInsideAPathIsConsumedAndProseStillIsNot) {
    // THE PREVIOUSLY-ACCEPTED INPUT. Nothing is stranded now.
    {
        const std::string out =
            pdfe::test::portablePath(R"(C:\Program Files (x86)\v\cli.jar)");
        EXPECT_EQ(out, "<path>") << out;
        EXPECT_EQ(out.find("Files"), std::string::npos)
            << "the tail is still stranded: " << out;
        EXPECT_EQ(out.find("cli.jar"), std::string::npos) << out;
        EXPECT_FALSE(hasDriveQualifiedPath(out)) << out;
    }

    // ...and the case the old bound existed to protect. Prose must survive.
    {
        const std::string out =
            pdfe::test::portablePath(R"(root C:\tmp\a and then some prose)");
        EXPECT_NE(out.find("and then some prose"), std::string::npos)
            << "prose was eaten: " << out;
        EXPECT_FALSE(hasDriveQualifiedPath(out)) << out;
    }

    // GUARD 1 -- a forbidden character stops the lookahead, so a diagnostic of
    // the ordinary "path: message" shape keeps its message.
    {
        const std::string out =
            pdfe::test::portablePath(R"(C:\v\bin cli.jar: No such file)");
        EXPECT_NE(out.find("No such file"), std::string::npos)
            << "the diagnostic was eaten: " << out;
    }

    // GUARD 2 -- a new drive-qualified path ends the lookahead, so two entries
    // in a PATH-style list are not merged into one:
    {
        const std::string out =
            pdfe::test::portablePath(R"(C:\a b and C:\c\d)");
        EXPECT_NE(out.find("and"), std::string::npos)
            << "the separator word was eaten, merging two paths: " << out;
        EXPECT_FALSE(hasDriveQualifiedPath(out)) << out;
    }

    // GUARD 3 -- the span is bounded. A separator eight words later is prose,
    // and a bound that admits it is not a bound.
    {
        const std::string out = pdfe::test::portablePath(
            R"(C:\tmp\a one two three four five six b\c)");
        EXPECT_NE(out.find("three"), std::string::npos)
            << "the span is unbounded: " << out;
    }

    // THE STANDING LEAK BOUND, unchanged and the reason the residual is
    // tolerable: the named-root pass runs BEFORE this loop, so a path under the
    // operator's profile loses the account name wholesale before any space is
    // reached.
    const char* home = std::getenv("USERPROFILE");
    if (home != nullptr && *home != '\0') {
        const std::string raw = std::string(home) + R"(\My Documents\x.pdf)";
        const std::string scrubbed = pdfe::test::portablePath(raw);
        EXPECT_NE(scrubbed.find("<home>"), std::string::npos) << scrubbed;
        EXPECT_FALSE(hasDriveQualifiedPath(scrubbed)) << scrubbed;
    }
}

// GitHub #283, GUARD 0 -- A NEWLINE MUST END THE TOKEN.
//
// THE REGRESSION THIS PINS, found in the git-tracked conformance record
// rather than by any test. `isSpace` covers newlines, so the first version of
// the interior-space lookahead crossed a LINE BREAK hunting for a separator
// and ate the flavour token with it:
//
//   before  FAIL <temp><path> 1b   [newline]   FAIL 6.7.3-1
//   after   FAIL <temp><path>                 FAIL 6.7.3-1
//
// i.e. both the " 1b" and the line break vanished into the placeholder.
//
// Only the multi-line FAIL rows were affected; the single-line PASS rows kept
// their " 1b", which is why this hid. A path never spans lines, so crossing
// one cannot be continuing a path -- it can only be eating prose.
// GitHub #283, THE CASE THAT DAMAGED A TRACKED FILE.
//
// `mergedCheckObjects()` reads sidecars holding SERIALISED JSON, so a line
// break in them is the two characters backslash-n, and a real path separator
// is DOUBLED. The interior-space lookahead went hunting for a separator,
// found the backslash of the escaped newline, and concluded the space before
// " 1b" was interior to the path:
//
//   sidecar  ...claim_check_1b.pdf 1b   [escaped newline]   FAIL 6.7.3-1
//   record   ...<path>                                      FAIL 6.7.3-1
//
// The flavour token and the line break both vanished into the placeholder, in
// a GIT-TRACKED diagnostic. Neither this file's own tests nor a gate's
// newline probe caught it -- the committed artifact did, which is the whole
// argument for having one.
TEST(ConformancePathScrub, AnEscapedNewlineIsNotAPathSeparator) {
    const std::string out = pdfe::test::portablePath(
        R"(FAIL C:\\PDFE-kFZYMc\\claim_check_1b.pdf 1b\n  FAIL 6.7.3-1)");

    EXPECT_NE(out.find("1b"), std::string::npos)
        << "the flavour token was eaten across an ESCAPED newline, which is "
           "how the tracked conformance record lost a line: " << out;
    EXPECT_NE(out.find("6.7.3-1"), std::string::npos) << out;
    EXPECT_FALSE(hasDriveQualifiedPath(out)) << out;
}

TEST(ConformancePathScrub, ANewlineEndsTheTokenAndNeverContinuesAPath) {
    // Raw string for the path so the backslashes are literal, concatenated
    // with a real newline — writing it as one ordinary literal would turn
    // `\t` into a TAB and `\o` into an invalid escape, which is a mistake this
    // file has already made once.
    const std::string out = pdfe::test::portablePath(
        R"(FAIL C:\tmp\out.pdf 1b)"
        "\n"
        R"(  FAIL 6.7.3-1)");

    EXPECT_NE(out.find("1b"), std::string::npos)
        << "the flavour token after the path was eaten across a newline, "
           "which is how a tracked diagnostic lost a line: " << out;
    EXPECT_NE(out.find("6.7.3-1"), std::string::npos) << out;
    EXPECT_NE(out.find('\n'), std::string::npos)
        << "the line break itself was consumed: " << out;
    EXPECT_FALSE(hasDriveQualifiedPath(out)) << out;
}

// GitHub #283, THE ACTUAL RESIDUAL -- pinned because the first write-up
// named an example that does not reproduce.
//
// The phase-5 claim-check gate caught this. The record claimed the new
// residual was
//
//     C:\tmp\a and the file b\c
//
// and then RAN it: that string has THREE interior spaces before the
// separator, which EXCEEDS kMaxInteriorSpaces = 2, so the lookahead correctly
// declines and the prose survives. The stated justification for moving the
// bound was being judged against a case the code already handles -- worse
// than no example, because it reads as evidence.
//
// The real residual is the same shape one word shorter, inside the cap. It is
// a known, accepted limitation, asserted here so that it is DOCUMENTED rather
// than discovered, and so that narrowing it later is a deliberate change to a
// test rather than a silent improvement nobody notices.
//
// Why it is tolerable: it cannot leak. <home>/<repo>/<temp> are substituted
// BEFORE this loop runs, so a path under the operator's profile has already
// lost the ACCOUNT NAME wholesale. What is lost here is prose in a
// diagnostic, and hasDriveQualifiedPath still holds.
TEST(ConformancePathScrub, TwoInteriorSpacesBeforeASeparatorStillConsumeProse) {
    // Within the cap: "and" is swallowed because the next token carries a
    // separator.
    {
        const std::string out = pdfe::test::portablePath(R"(C:\tmp\a and b\c)");
        EXPECT_EQ(out, "<path>")
            << "the accepted residual changed. If it is now narrower that is "
               "an improvement -- update this test deliberately rather than "
               "letting the limitation go unrecorded. Got: " << out;
        EXPECT_FALSE(hasDriveQualifiedPath(out)) << out;
    }
    // In a sentence, which is how it would actually be met.
    {
        const std::string out = pdfe::test::portablePath(
            R"(The report is at C:\tmp\a and b\c has more details)");
        EXPECT_EQ(out.find("and b"), std::string::npos)
            << "expected the two-space run to be consumed: " << out;
        EXPECT_NE(out.find("The report is at "), std::string::npos) << out;
        EXPECT_NE(out.find(" has more details"), std::string::npos) << out;
        EXPECT_FALSE(hasDriveQualifiedPath(out)) << out;
    }
    // ONE WORD MORE and the cap declines, so the bound is real rather than
    // merely described. This pair is what makes the case above a BOUND and
    // not an anecdote.
    {
        const std::string out =
            pdfe::test::portablePath(R"(C:\tmp\a and the file b\c)");
        EXPECT_NE(out.find("and the file"), std::string::npos)
            << "three interior spaces exceed kMaxInteriorSpaces = 2 and must "
               "NOT be consumed; if this fails the cap has widened: " << out;
        EXPECT_FALSE(hasDriveQualifiedPath(out)) << out;
    }
}

// `;` and `,` are LEGAL in a Windows filename, so they only end a token when they
// are ACTING as punctuation. Both halves matter: round 3 and round 4 exist
// because the punctuation case was being eaten, and C10 exists because treating
// such a character as an unconditional stop strands the rest of a name.
TEST(ConformancePathScrub, DelimitersEndTheTokenOnlyWhenActingAsPunctuation) {
    // Acting as punctuation -> must survive as prose.
    for (const char* raw : {R"(root C:\verapdf; java next)",
                            R"(root C:\verapdf, java next)"}) {
        const std::string out = pdfe::test::portablePath(raw);
        EXPECT_NE(out.find("java next"), std::string::npos) << out;
        EXPECT_TRUE(out.find(';') != std::string::npos
                    || out.find(',') != std::string::npos)
            << "the punctuation was eaten: " << out;
    }
    // A PATH-style list: the delimiter separates two drive-qualified paths, so it
    // is punctuation too, and BOTH sides must be scrubbed.
    {
        const std::string out =
            pdfe::test::portablePath(R"(C:\verapdf\bin;C:\other\tool\x.jar)");
        EXPECT_NE(out.find(';'), std::string::npos) << out;
        EXPECT_EQ(out.find("verapdf"), std::string::npos) << out;
        EXPECT_EQ(out.find("other"), std::string::npos) << out;
        EXPECT_FALSE(hasDriveQualifiedPath(out)) << out;
    }
    // Interior to a NAME -> must not split it, or the tail is stranded.
    {
        const std::string out = pdfe::test::portablePath(R"(C:\v\a,b;c.jar)");
        EXPECT_EQ(out, "<path>")
            << "a comma/semicolon inside a filename split the token and left a "
               "fragment behind. Got: " << out;
    }
}

// CLAIM-CHECK ROUND 6 (C11), PART 1. A UNC root was invisible to the backstop:
// its drive test is `isalpha(s[i]) && s[i+1] == ':'`, which can never match
// `\\server\share\...`. A UNC-rooted JAVA_HOME or veraPDF install root -- both
// free-form strings this code reads from the environment and filesystem -- passed
// through COMPLETELY unscrubbed, account name included.
TEST(ConformancePathScrub, AUncRootIsScrubbedLikeADriveRoot) {
    for (const char* raw : {R"(\\fileserver\profiles$\acct\verapdf\cli.jar)",
                            "//fileserver/profiles$/acct/verapdf/cli.jar"}) {
        const std::string out = pdfe::test::portablePath(raw);

        EXPECT_EQ(out, "<path>")
            << "a UNC path was not recognised as a path, so it reached the "
               "tracked record verbatim. Input: " << raw << "  Got: " << out;
        EXPECT_EQ(out.find("fileserver"), std::string::npos) << out;
        EXPECT_EQ(out.find("acct"), std::string::npos) << out;
    }
}

// A doubled separator that is NOT a UNC root must not be mistaken for one -- the
// JSON-escaped form `C:\\Users\\x` arrives with doubled backslashes, and three or
// more separators is not a server name.
TEST(ConformancePathScrub, ADoubledSeparatorAloneIsNotTreatedAsAUncRoot) {
    const std::string out = pdfe::test::portablePath(R"(see \\\ for details)");
    EXPECT_NE(out.find("for details"), std::string::npos) << out;
}

// CLAIM-CHECK ROUND 6 (C11), PART 2 -- THE ONE THAT CARRIES THE SECURITY WEIGHT.
// The previous round's Bound 2 claimed the account name "is gone before any space
// is reached". That held only for paths literally prefixed by %USERPROFILE%.
// Measured on the unmodified tree:
//
//   D:\My Documents\<account>\jdk\bin\java.exe
//       -> "<path> Documents\<account>\jdk\bin\java.exe"
//
// Not under <home>/<repo>/<temp>, so no named root matched; the space inside
// "My Documents" ended the token early and stranded the account name verbatim --
// into a GIT-TRACKED file. The fix is a final pass that removes the account NAME
// regardless of the path shape around it, because a shape can always be missed
// again.
TEST(ConformancePathScrub, TheAccountNameNeverSurvivesWhateverSurroundsIt) {
    const ScopedUserProfile pinned(kPinnedProfile);
    const std::string account(kPinnedAccount);

    // Each of these defeats a DIFFERENT one of the path-shaped passes.
    for (const std::string raw : {
             // outside every named root, space before the account segment
             "D:\\My Documents\\" + account + "\\jdk\\bin\\java.exe",
             // UNC, which the drive check could not see at all
             "\\\\fileserver\\profiles$\\" + account + "\\jdk\\java.exe",
             // bare, with no path around it to recognise
             "resolved for user " + account + " at startup",
         }) {
        const std::string out = pdfe::test::portablePath(raw);
        EXPECT_EQ(out.find(account), std::string::npos)
            << "the account name survived into output that goes into a "
               "git-tracked file. Input: " << raw << "  Got: " << out;
    }
}

// NEGATIVE CONTROL, THE ONE THAT WAS MISSING. karen's seventh-round finding: the
// account-name pass was a bare case-insensitive SUBSTRING replace, so it rewrote
// ordinary words that merely contain the name --
//
//   account "ask"    : "the task basket" -> "the t<user> b<user>et"
//   account "acct" : "schacctl"      -> "sch<user>l"
//
// and the `>= 3` length bound did not save it. The control that existed only ever
// tested the name flanked by SPACES, so it passed the whole time. A scrubber that
// corrupts the text it is cleaning gets switched off, which costs the protection
// entirely.
TEST(ConformancePathScrub, TheAccountNamePassMatchesWholeTokensOnly) {
    const ScopedUserProfile pinned(kPinnedProfile);
    const std::string account(kPinnedAccount);

    // The name EMBEDDED in a longer alphanumeric token is not the account name.
    for (const std::string raw : {"sch" + account + "l reported it",
                                  "prefix" + account,
                                  account + "suffix"}) {
        const std::string out = pdfe::test::portablePath(raw);
        EXPECT_EQ(out, raw)
            << "a longer word merely CONTAINING the account name was rewritten, "
               "so this pass corrupts ordinary text. Input: " << raw
            << "  Got: " << out;
    }

    // ...but flanked by path separators or whitespace it still must go.
    for (const std::string raw : {"D:\\x\\" + account + "\\y",
                                  "user " + account + " resolved",
                                  "(" + account + ")"}) {
        const std::string out = pdfe::test::portablePath(raw);
        EXPECT_EQ(out.find(account), std::string::npos)
            << "the account name survived where it IS a whole token. Input: "
            << raw << "  Got: " << out;
    }
}

// NEGATIVE CONTROL for that final pass: it must not eat the prose around the
// name, or a diagnostic becomes unreadable and someone disables the scrubbing.
TEST(ConformancePathScrub, TheAccountNamePassLeavesSurroundingProseIntact) {
    const ScopedUserProfile pinned(kPinnedProfile);
    const std::string account(kPinnedAccount);

    const std::string out =
        pdfe::test::portablePath("resolved for user " + account + " at startup");
    EXPECT_NE(out.find("resolved for user "), std::string::npos) << out;
    EXPECT_NE(out.find(" at startup"), std::string::npos) << out;
    EXPECT_NE(out.find("<user>"), std::string::npos) << out;
}

// THE BOUND THAT USED TO BE A SKIP. `portablePath` deliberately does nothing when
// the account name is shorter than 3 characters, so that a pathological or empty
// value cannot turn a whole-token replacement into a mass rewrite. Three tests
// above used to `GTEST_SKIP()` on exactly that condition, which meant the bound
// was never asserted anywhere AND those tests silently stopped running on a host
// with a short account name (#274).
//
// Now the condition is the subject of its own test rather than the reason others
// do not run, and the environment is pinned so it holds on every machine.
TEST(ConformancePathScrub, TheAccountNamePassIsBoundedBelowAtThreeCharacters) {
    {   // Two characters: below the bound, so the name is NOT scrubbed.
        const ScopedUserProfile pinned("D:\\pinned\\ab");
        const std::string out = pdfe::test::portablePath("user ab here");
        EXPECT_EQ(out, "user ab here")
            << "a 2-character account name was scrubbed; the >= 3 bound exists so "
               "a pathological value cannot rewrite ordinary text. Got: " << out;
    }
    {   // Three characters: at the bound, so it IS scrubbed.
        const ScopedUserProfile pinned("D:\\pinned\\abc");
        const std::string out = pdfe::test::portablePath("user abc here");
        EXPECT_EQ(out, "user <user> here")
            << "a 3-character account name was not scrubbed, so the bound is off "
               "by one and the shortest real account names leak. Got: " << out;
    }
}

// The pin must restore what it found, or every later test in this process runs
// against a fabricated USERPROFILE -- which would be a far worse bug than the one
// it was introduced to fix.
TEST(ConformancePathScrub, TheUserProfilePinRestoresThePreviousValue) {
    const char* before = std::getenv("USERPROFILE");
    const std::string saved = before ? before : "";
    {
        const ScopedUserProfile pinned(kPinnedProfile);
        const char* during = std::getenv("USERPROFILE");
        ASSERT_NE(during, nullptr);
        EXPECT_STREQ(during, kPinnedProfile);
    }
    const char* after = std::getenv("USERPROFILE");
    EXPECT_EQ(std::string(after ? after : ""), saved)
        << "ScopedUserProfile did not restore USERPROFILE";
}

TEST(ConformancePathScrub, TextWithNoPathIsReturnedUnchanged) {
    const std::string raw = "veraPDF 1.30.2";
    EXPECT_EQ(pdfe::test::portablePath(raw), raw);
}

// The named roots must still win, because `<repo>/tools/...` is far more useful
// to a reader than `<path>`. If the backstop ran first this would regress to the
// generic token and nobody would notice from the tracked file alone.
TEST(ConformancePathScrub, TheRepoRootStillYieldsTheRepoPlaceholder) {
    const std::string raw = std::string(PDFE_REPO_ROOT) + "\\tools\\conformance\\x.jar";
    const std::string out = pdfe::test::portablePath(raw);
    EXPECT_NE(out.find("<repo>"), std::string::npos)
        << "the <repo> placeholder was lost -- the generic backstop is running "
           "ahead of the named roots. Got: " << out;
    EXPECT_FALSE(hasDriveQualifiedPath(out)) << out;
}
