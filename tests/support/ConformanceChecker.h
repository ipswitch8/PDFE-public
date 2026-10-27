#pragma once
//
// Independent PDF/A conformance checker integration — meta-audit follow-up F14
// (PDF-10 / PDF-17).
//
// THE DEFECT THIS REPLACES. Before F14, the only independent PDF/A checker in
// the tree was reached through `std::getenv("VERAPDF_JAR")`
// (tests/test_fidelity_r3_19_pdfa_independent.cpp:200) and the test
// `GTEST_SKIP`ped when the variable was unset (:407). `VERAPDF_JAR` was set
// nowhere in the repository or in CI, so the checker had NEVER run: PDF/A
// conformance was graded solely by PDFE's own validator against PDFE's own
// converter — a closed loop that cannot detect a false conformance claim.
//
// THE RULE THIS FILE FOLLOWS. Resolution is **deterministic and total**, and
// depends on NO human-supplied variable:
//
//   1. the provisioned location `<repo>/tools/conformance/verapdf/`, which
//      `tools/conformance/fetch-verapdf.py` populates (documented default),
//   2. a fixed, ordered list of conventional system install locations,
//   3. a fixed, ordered list of Java locations (PATH, then JAVA_HOME, then the
//      conventional Windows JDK directories).
//
// Every call returns either an AVAILABLE checker with a path that exists, or an
// UNAVAILABLE result carrying a concrete reason and the named prerequisite that
// would fix it. There is no third, indeterminate state — which is what lets a
// test assert the outcome instead of skipping on it.

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace pdfe::test {

/// Replace host-specific paths in `s` with placeholders, and the operator's
/// account name with `<user>` wherever it appears.
///
/// THE SECOND HALF IS NOT A RESTATEMENT OF THE FIRST, and this doc used to say
/// only "every host-specific path", which claim-check round 6 showed was both
/// too strong and too weak. Too strong: a UNC root was not recognised as a path
/// at all. Too weak: the account name can survive every path-shaped pass, since
/// each depends on recognising the path AROUND it -- a drive-rooted install path
/// outside the three named roots, containing a space before the account-name
/// segment, stranded that segment verbatim into a git-tracked file.
///
/// Guarantees, in the order they are enforced:
///   1. `<temp>` / `<repo>` / `<home>` prefixes, longest first;
///   2. any remaining drive-rooted OR UNC-rooted path token -> `<path>`;
///   3. the account name itself -> `<user>`, independent of path shape.
/// (2) still stops at a space, which an unquoted path cannot be delimited past;
/// (3) is what makes the account-name guarantee hold anyway.
///
/// EXPOSED SO THE SCRUBBER CAN BE TESTED DIRECTLY (claim-check, phase-1,
/// 2026-10-06). `UiSuitePathHygiene.TheTrackedConformanceRecordIsPortable`
/// asserts that the COMMITTED record carries no absolute path -- but it passes
/// whenever that file happens to be portable, including when it is stale and
/// predates the scrubber change entirely. So the test's green said nothing about
/// whether the scrubber actually handles a root it does not know about, which is
/// precisely the case that failed: veraPDF installed at `C:\verapdf`, outside
/// `<repo>`, `<home>` and `<temp>`.
///
/// Declaring it here lets a unit test feed it a synthetic non-repo absolute path
/// and assert on the result, so the claim is verified at the level it is made
/// rather than inferred from an artifact that may not have been regenerated.
std::string portablePath(std::string s);

/// Outcome of locating the independent checker. Total: `available` is true with
/// a usable `jarPath` + `javaCommand`, or false with a non-empty `reason` and
/// `prerequisite`.
struct CheckerResolution {
    bool available = false;
    std::string tool = "veraPDF";
    std::string jarPath;          ///< absolute path to the CLI jar
    std::string javaCommand;      ///< absolute path to (or bare name of) java
    std::string version;          ///< tool version banner, when obtainable
    std::string howResolved;      ///< which resolution rule matched
    std::string reason;           ///< why unavailable (empty when available)
    std::string prerequisite;     ///< what would make it available
    std::vector<std::string> searched;  ///< every location tried, in order
};

/// Resolve the checker. Deterministic; the result is computed once and cached.
/// Reads no VERAPDF_JAR (or any other checker-specific) variable.
const CheckerResolution& resolveConformanceChecker();

/// Result of one conformance check.
struct CheckerVerdict {
    bool ran = false;        ///< the checker process actually executed
    bool valid = false;      ///< the file conforms to `flavour` (only if ran)
    int exitCode = -1;
    std::string flavour;
    std::string file;
    std::string output;      ///< captured stdout+stderr (trimmed)
    std::string reason;      ///< why it did not run (only if !ran)
};

/// Run the checker over @p pdf at PDF/A @p flavour ("1b", "2b", "2a", "3b", …).
/// Never throws; a checker that cannot run comes back as `ran == false` with a
/// reason, so the caller asserts rather than skips.
CheckerVerdict runConformanceCheck(const std::filesystem::path& pdf,
                                   const std::string& flavour);

/// The ISO clause ids the checker reported as FAILED, sorted and de-duplicated.
///
/// Parsed from the verbose CLI output, whose failure lines look like
/// `  FAIL 6.1.7-2`. Returns empty for a verdict that passed or did not run.
std::vector<std::string> failedClauses(const CheckerVerdict& verdict);

/// Append @p verdict to the phase-14 result-or-reason record and rewrite it.
///
/// The record is written to BOTH
///   `.claude/pipelines/meta-audit-followup/gate-artifacts/phase-14/conformance-checker-record.json`
/// (the gate artifact) and
///   `docs/audit/meta-audit-2026-07/followup/phase-14/conformance-checker-record.json`
/// (the durable copy — `.claude/` is gitignored).
void recordConformanceResult(const CheckerVerdict& verdict);

/// Rewrite both copies of the record from whatever has been recorded so far.
/// Called automatically by recordConformanceResult(); exposed so a test can
/// force a record to exist even when nothing ran.
///
/// IDEMPOTENT (GitHub #155). A copy is rewritten only when its SUBSTANTIVE
/// content -- verdicts, ISO clause ids, per-file results, checker resolution --
/// differs from what is already on disk. Regenerated timestamps and the random
/// per-run temp-directory name do NOT count as a change, because the durable
/// copy is tracked in git and rewriting it on every run left the working tree
/// permanently dirty after any suite run.
///
/// @returns how many of the two copies were actually written (0, 1 or 2). A
/// second call with nothing changed in between returns 0 -- which is what a
/// test asserts on, since "the file did not change" is otherwise only
/// observable through a timestamp.
int flushConformanceRecord();

/// Write @p text to @p path, but only when its SUBSTANTIVE content differs from
/// what is already on disk. Returns true iff a write actually happened. An
/// absent or unreadable destination counts as "differs", so a missing record is
/// always created.
///
/// This is the same function `flushConformanceRecord()` calls for each of its
/// two copies; it is exported so a test can drive the writer against a scratch
/// path in BOTH directions (GitHub #190). Before #190 every case in
/// tests/test_conformance_record_idempotent.cpp exercised the does-NOT-write
/// direction or the digest in isolation, so a writer that had stopped writing
/// altogether -- `return false;` as the first statement -- would have passed
/// the whole file. That mutant is now killed by
/// `ConformanceRecordWrites.*`.
bool writeConformanceRecordIfChanged(const std::filesystem::path& path,
                                     const std::string& text);

/// Does a flush holding @p ownVerdicts checks of its own and @p mergedChecks
/// merged from other shards' sidecars know ANYTHING about `checks`?
///
/// When both are zero the answer is no, and `flushConformanceRecord()` returns
/// without writing. Writing in that state emits `"checks": []` over a TRACKED
/// evidence file holding a real run's verdicts -- an erasure, not a
/// measurement. Measured 2026-10-18: the sidecar directory lives under the
/// checkout, so moving the checkout left it behind; the first shard to flush
/// blanked the committed record from six checks to none, and GitHub #190's
/// repair case then failed the premise guard that looks for a "verdict" field.
///
/// This is the same predicate `flushConformanceRecord()` calls, exported so
/// the rule is pinned directly rather than through a second copy that could
/// stay correct while the shipped one rotted.
bool conformanceFlushHasInformation(std::size_t ownVerdicts,
                                    std::size_t mergedChecks);

/// The normalised form `flushConformanceRecord()` compares on -- the record
/// with its volatile fields neutralised. Exposed for tests only; this string is
/// never written anywhere.
std::string conformanceRecordDigest(const std::string& json);

} // namespace pdfe::test
