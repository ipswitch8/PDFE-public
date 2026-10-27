// -*- coding: utf-8 -*-
//
// Conformance-record writer is idempotent — GitHub #155.
//
// THE DEFECT. tests/support/ConformanceChecker.cpp writes its result-or-reason
// record into docs/audit/meta-audit-2026-07/followup/phase-14/
// conformance-checker-record.json, which is TRACKED IN GIT, and it rewrote that
// file unconditionally on every call. The substantive content — the veraPDF
// verdicts, the ISO clause ids, the per-file results — was byte-identical
// between runs; only three volatile things moved:
//
//   1. the top-level `generated` timestamp,
//   2. each check's `recorded` timestamp,
//   3. the random per-run temp-directory NAME inside `output` (the %TEMP% root
//      is already scrubbed to `<temp>`; the random leaf survived).
//
// So `git status` was never clean after a suite run, and the file was restored
// with `git checkout --` in every phase of eight consecutive pipelines. That
// matters because this project forbids `git add -A` and relies on the operator
// reading `git status` before staging named files — a permanently-dirty entry
// trains the reader to skim past exactly the output they must not skim.
//
// THE FIX UNDER TEST: the writer compares SUBSTANTIVE content and skips the
// write when it has not changed, rather than deleting the volatile fields
// (which are genuine provenance) or hiding the file.
//
// WHAT THESE CASES PIN, and why each is needed:
//   - the digest IGNORES each of the three volatile classes (cases 1a/1b/1c) —
//     otherwise the skip never triggers and the tree stays dirty;
//   - the digest still DISTINGUISHES a real verdict change (cases 2a/2b/2c) —
//     otherwise a digest that returned a constant would pass every case above
//     while silently freezing the evidence file forever. That is the failure
//     mode with teeth here, so it gets three cases against the fields #155
//     names as the ones that must not be lost;
//   - the writer itself actually skips (case 3), end to end.

#include <gtest/gtest.h>

#include "support/ConformanceChecker.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>

namespace {

using pdfe::test::conformanceRecordDigest;
using pdfe::test::flushConformanceRecord;

// A record shaped like the real one, with the three volatile classes present.
// `%GEN%`, `%REC%`, `%TMPDIR%` and `%VERDICT%` are substituted per case.
std::string sampleRecord(const std::string& generated,
                         const std::string& recorded,
                         const std::string& tempDir,
                         const std::string& verdict,
                         const std::string& clause = "6.7.3-1") {
    std::ostringstream o;
    o << "{\n";
    o << "  \"schema\": \"pdfe.conformance-checker-record/2\",\n";
    o << "  \"phase\": \"phase-14\",\n";
    o << "  \"generated\": \"" << generated << "\",\n";
    o << "  \"checks\": [\n";
    o << "    {\"file\": \"claim_check_1b.pdf\", \"flavour\": \"1b\", "
      << "\"ran\": true, \"verdict\": \"" << verdict << "\", "
      << "\"exit_code\": 1, \"recorded\": \"" << recorded << "\", "
      << "\"reason\": \"\", "
      << "\"output\": \"FAIL <temp>\\\\" << tempDir
      << "\\\\claim_check_1b.pdf 1b\\n  FAIL " << clause << "\"}\n";
    o << "  ]\n";
    o << "}\n";
    return o.str();
}

const char* kGenA = "2026-08-26T19:20:48Z";
const char* kGenB = "2026-09-13T04:11:02Z";
const char* kRecA = "2026-08-26T19:07:04Z";
const char* kRecB = "2026-09-13T04:10:57Z";

// ---------------------------------------------------------------------------
// 1. The digest ignores each volatile class — one case per class, so a fix that
//    handles only some of them cannot pass.
// ---------------------------------------------------------------------------

TEST(ConformanceRecordIdempotent, DigestIgnoresTheGeneratedTimestamp) {
    const std::string a = sampleRecord(kGenA, kRecA, "PDFE-EylaBd", "FAIL");
    const std::string b = sampleRecord(kGenB, kRecA, "PDFE-EylaBd", "FAIL");
    ASSERT_NE(a, b) << "the two records must differ literally, or this case "
                       "compares a string with itself and proves nothing";
    EXPECT_EQ(conformanceRecordDigest(a), conformanceRecordDigest(b))
        << "a new `generated` timestamp alone is not a substantive change and "
           "must not cause the tracked record to be rewritten";
}

TEST(ConformanceRecordIdempotent, DigestIgnoresPerCheckRecordedTimestamps) {
    const std::string a = sampleRecord(kGenA, kRecA, "PDFE-EylaBd", "FAIL");
    const std::string b = sampleRecord(kGenA, kRecB, "PDFE-EylaBd", "FAIL");
    ASSERT_NE(a, b);
    EXPECT_EQ(conformanceRecordDigest(a), conformanceRecordDigest(b))
        << "a check's `recorded` timestamp is regenerated every run and must "
           "not count as a substantive change";
}

TEST(ConformanceRecordIdempotent, DigestIgnoresTheRandomTempDirectoryName) {
    const std::string a = sampleRecord(kGenA, kRecA, "PDFE-EylaBd", "FAIL");
    const std::string b = sampleRecord(kGenA, kRecA, "PDFE-Zq7t1K", "FAIL");
    ASSERT_NE(a, b);
    EXPECT_EQ(conformanceRecordDigest(a), conformanceRecordDigest(b))
        << "the random per-run temp-directory leaf inside `output` must be "
           "normalised away; the %TEMP% ROOT is already scrubbed to <temp>, "
           "and it is the leaf that was still churning the tracked file";
}

TEST(ConformanceRecordIdempotent, DigestIgnoresAllThreeVolatileClassesAtOnce) {
    const std::string a = sampleRecord(kGenA, kRecA, "PDFE-EylaBd", "FAIL");
    const std::string b = sampleRecord(kGenB, kRecB, "PDFE-Zq7t1K", "FAIL");
    ASSERT_NE(a, b);
    EXPECT_EQ(conformanceRecordDigest(a), conformanceRecordDigest(b))
        << "a run that changed nothing substantive must digest identically "
           "even though all three volatile classes moved together -- this is "
           "the actual shape of the diff #155 reports";
}

// A fourth volatile class, found by RUNNING the suite rather than by reading
// the issue: #155 lists only the two timestamp classes and the temp path.
// `checks_this_process` counts what one process contributed, so it moves with
// the shard count and with any --gtest_filter in effect while every verdict
// stays identical. Both a 16-shard run and a filtered run were observed
// dirtying the tracked file through this field alone.
TEST(ConformanceRecordIdempotent, DigestIgnoresChecksThisProcess) {
    const std::string one =
        "{\n  \"checks_this_process\": 1,\n  \"checks\": []\n}\n";
    const std::string zero =
        "{\n  \"checks_this_process\": 0,\n  \"checks\": []\n}\n";
    ASSERT_NE(one, zero);
    EXPECT_EQ(conformanceRecordDigest(one), conformanceRecordDigest(zero))
        << "how many checks ONE process contributed is a property of how the "
           "suite was invoked (shards, --gtest_filter), not of what the "
           "checker found, and must not by itself rewrite a tracked file";
}

// core.autocrlf=true means git writes CRLF into the working tree while this
// writer emits LF. A byte comparison would therefore see every freshly
// checked-out record as changed -- green in the session that wrote the file,
// red after any checkout.
TEST(ConformanceRecordIdempotent, DigestIsInsensitiveToLineEndings) {
    const std::string lf = sampleRecord(kGenA, kRecA, "PDFE-EylaBd", "FAIL");
    std::string crlf;
    for (char c : lf) {
        if (c == '\n') crlf += '\r';
        crlf += c;
    }
    ASSERT_NE(lf, crlf) << "the CRLF conversion must actually have changed the "
                           "bytes, or this case is comparing a string to itself";
    EXPECT_EQ(conformanceRecordDigest(lf), conformanceRecordDigest(crlf))
        << "a CRLF working-tree copy and an LF generated copy of the SAME "
           "record must digest identically; otherwise the writer rewrites the "
           "tracked file on every freshly checked-out tree";
}

// ---------------------------------------------------------------------------
// 2. The digest still distinguishes real change.
// ---------------------------------------------------------------------------
//
// #155 is explicit that the verdicts are a separate, real PDF/A finding and
// must not be changed or lost by this fix. A digest that over-normalised would
// freeze them in the tracked file forever while every case above still passed.

TEST(ConformanceRecordIdempotent, DigestDistinguishesAChangedVerdict) {
    const std::string fail = sampleRecord(kGenA, kRecA, "PDFE-EylaBd", "FAIL");
    const std::string pass = sampleRecord(kGenA, kRecA, "PDFE-EylaBd", "PASS");
    EXPECT_NE(conformanceRecordDigest(fail), conformanceRecordDigest(pass))
        << "FAIL becoming PASS is the single most important change this record "
           "can carry; it MUST be written";
}

TEST(ConformanceRecordIdempotent, DigestDistinguishesAChangedIsoClauseId) {
    const std::string a =
        sampleRecord(kGenA, kRecA, "PDFE-EylaBd", "FAIL", "6.7.3-1");
    const std::string b =
        sampleRecord(kGenA, kRecA, "PDFE-EylaBd", "FAIL", "6.1.7-2");
    EXPECT_NE(conformanceRecordDigest(a), conformanceRecordDigest(b))
        << "the ISO clause a file fails on is substantive evidence; a record "
           "that failed on a different clause is a different record";
}

TEST(ConformanceRecordIdempotent, DigestDistinguishesADifferentCheckedFile) {
    std::string a = sampleRecord(kGenA, kRecA, "PDFE-EylaBd", "FAIL");
    std::string b = a;
    const std::string from = "claim_check_1b.pdf";
    const std::string to = "claim_check_2b.pdf";
    for (std::size_t at = b.find(from); at != std::string::npos;
         at = b.find(from, at + to.size())) {
        b.replace(at, from.size(), to);
    }
    ASSERT_NE(a, b);
    EXPECT_NE(conformanceRecordDigest(a), conformanceRecordDigest(b))
        << "the file a verdict is about must survive normalisation -- it "
           "appears inside `output` right after the <temp> placeholder, which "
           "is precisely the region being normalised, so an over-broad "
           "normaliser would eat it";
}

namespace {

// Establish, IN THIS PROCESS, the premise the two end-to-end cases below rest
// on: that the live record holds at least one check.
//
// WHY THIS EXISTS. Both cases used to inherit that premise from whichever
// OTHER shard happened to run a conformance test first, because `checks` is
// merged from sidecar files shared across shards. That made them
// order-dependent in a way nothing declared. Measured 2026-10-18 after the
// checkout moved (the sidecar directory lives under the checkout, so no
// sidecar came with it): both ran in shard 1 with the directory absent, the
// repair case failed its premise guard, and the rerun "healed" purely because
// a later shard had written sidecars in the meantime. A heal explained by
// state the first run could not have had is not a flake.
//
// Returns false only when the checker is genuinely unavailable. Callers then
// take the honest 'unavailable here' branch this file's siblings already use
// (see ConformanceIndependentTest) — asserted concretely, never skipped.
bool recordOneLiveCheck() {
    const pdfe::test::CheckerResolution& r =
        pdfe::test::resolveConformanceChecker();
    if (!r.available) return false;

    // A COMMITTED fixture, so this cannot depend on gitignored corpus input.
    // Its verdict is FAIL (it is not PDF/A) and that is fine: these cases need
    // a check RECORDED, not a conforming one.
    const std::filesystem::path pdf =
        std::filesystem::path(PDFE_REPO_ROOT) / "tests" / "fixtures" /
        "test_single.pdf";
    if (!std::filesystem::exists(pdf)) {
        ADD_FAILURE() << "committed fixture is missing: " << pdf.string();
        return false;
    }

    pdfe::test::CheckerVerdict v = pdfe::test::runConformanceCheck(pdf, "1b");
    if (!v.ran) {
        ADD_FAILURE() << "the checker resolved as available but did not run: "
                      << v.reason;
        return false;
    }
    // Writes the sidecar AND flushes, so the live record now holds this check.
    pdfe::test::recordConformanceResult(v);
    return true;
}

// The honest 'unavailable here' branch, shared by both callers. An absent
// checker is a pass, but only a CONCRETE one: it must say what is missing.
void expectHonestlyUnavailable() {
    const pdfe::test::CheckerResolution& r =
        pdfe::test::resolveConformanceChecker();
    EXPECT_FALSE(r.available);
    EXPECT_FALSE(r.reason.empty())
        << "an unavailable checker must say why";
    EXPECT_FALSE(r.prerequisite.empty())
        << "an unavailable checker must name its prerequisite";
}

}  // namespace

// ---------------------------------------------------------------------------
// 2b. A flush that knows nothing must not erase. GitHub #155's other half.
// ---------------------------------------------------------------------------
//
// `checks` is merged from sidecar files. A process that ran no check of its
// own and finds no sidecar knows NOTHING about `checks` -- it does not know
// that there are none. Writing anyway emits "checks": [] over a TRACKED file
// holding a real run's six verdicts.
//
// This is pinned on the predicate the shipped flush calls, not a copy of it.
// Both directions are asserted: an "always has information" mutant (return
// true) is killed by the first case, and a "never has information" mutant
// (return false), which would stop the record ever being written, is killed by
// the other three -- and by ConformanceRecordWrites.FlushRewrites..., which
// requires a real write.
//
// WHAT THIS DOES NOT PIN, stated rather than implied: the one-line WIRING of
// the predicate into flushConformanceRecord(). Measured 2026-10-18 by mutating
// the predicate to `return true`: this case failed (good), but both end-to-end
// cases still PASSED, because each now records a live check of its own before
// flushing and so never reaches the no-information state. Deleting the call
// from flush while leaving the predicate correct would therefore go unnoticed
// here. Reaching that state in-process is not possible once any case in the
// shard has recorded a check, and recording nothing is not something a test
// can impose on its process -- so the honest position is that the rule is
// pinned and the wiring is not.
TEST(ConformanceRecordIdempotent, AFlushWithNoInformationHasNothingToSay) {
    using pdfe::test::conformanceFlushHasInformation;

    EXPECT_FALSE(conformanceFlushHasInformation(0, 0))
        << "no own checks and no sidecars is ABSENCE OF INFORMATION, not a "
           "finding of zero checks; writing here blanks the tracked record";

    EXPECT_TRUE(conformanceFlushHasInformation(1, 0))
        << "this process ran a check, so it has something to record even "
           "before any sidecar exists";
    EXPECT_TRUE(conformanceFlushHasInformation(0, 1))
        << "another shard's sidecar is information too -- this is the ONLY "
           "thing that carries a check across shard processes";
    EXPECT_TRUE(conformanceFlushHasInformation(6, 6));
}

// ---------------------------------------------------------------------------
// 3. End to end: the writer actually skips.
// ---------------------------------------------------------------------------
//
// flushConformanceRecord() returns how many of its two copies it wrote. The
// first call may legitimately write (the on-disk record can be stale, or
// absent). The SECOND, with nothing changed in between, must write neither —
// which is exactly the "git status is clean after a suite run" property, made
// observable without having to inspect a timestamp.
TEST(ConformanceRecordIdempotent, ASecondFlushWithNoChangeWritesNothing) {
    // The premise, established here rather than inherited from another shard.
    // Without it this case can pass for the wrong reason: a process holding no
    // checks at all now legitimately writes NOTHING on either flush (see
    // flushConformanceRecord's no-information guard), so the second flush
    // returns 0 without the digest having been consulted — the skip would be
    // measuring the guard, not the normalisation this case exists to pin.
    if (!recordOneLiveCheck()) {
        expectHonestlyUnavailable();
        return;
    }

    flushConformanceRecord();  // settle whatever was on disk

    // CROSS A WHOLE-SECOND BOUNDARY between the two flushes. The record's
    // timestamps are ISO-8601 at SECOND resolution, so two back-to-back
    // flushes normally land in the same second and produce byte-identical
    // text -- at which point this case passes whether or not any
    // normalisation exists, and is measuring nothing. (Observed: it survived
    // the "digest does no normalisation" mutant for exactly this reason.)
    // Sleeping past the boundary forces `generated` to genuinely differ, so
    // the only thing that can still make the write be skipped is the digest.
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));

    EXPECT_EQ(flushConformanceRecord(), 0)
        << "a flush that follows another flush with no intervening check must "
           "rewrite NEITHER copy of the record; a non-zero count here is the "
           "tracked file being dirtied by a suite run, which is GitHub #155";
}

// ---------------------------------------------------------------------------
// 3b. The DOES-WRITE direction, end to end — GitHub #190.
// ---------------------------------------------------------------------------
//
// THE GAP #190 REPORTS. Everything above this point tests either the digest in
// isolation or the writer's decision to STAY SILENT. Nothing anywhere in the
// suite ran the writer and then read the file back to confirm it had actually
// been written. So the single cheapest way for this component to fail
// catastrophically — a writer that never writes — was covered by nothing:
//
//   * `ASecondFlushWithNoChangeWritesNothing` expects 0 and would get 0.
//   * `TheTrackedRecordIsWhereTheDocumentsCiteIt` expects the file to EXIST,
//     and it exists already from an earlier run — that case cannot tell a live
//     writer from a dead one on any tree where the record is committed, which
//     is every tree, because the record IS committed.
//   * every Digest* case calls a pure function and touches no file at all.
//
// A `return false;` inserted as the first statement of `writeRecordIfChanged`
// passed the entire file. The cases below kill that mutant, and they assert on
// CONTENT read back from disk rather than on the return value alone, because a
// writer could also return true while writing nothing.
//
// These run against a scratch directory, never the tracked record: a test that
// dirtied the tracked file to prove the writer works would re-create #155 in
// the act of testing its fix.

namespace {

std::filesystem::path scratchDir(const char* leaf) {
    const auto dir = std::filesystem::temp_directory_path() /
                     (std::string("pdfe-conformance-write-") + leaf);
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

std::string slurp(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

}  // namespace

using pdfe::test::writeConformanceRecordIfChanged;

TEST(ConformanceRecordWrites, AnAbsentRecordIsCreatedAndReadsBackByteForByte) {
    const auto dir = scratchDir("absent");
    const auto path = dir / "nested" / "conformance-checker-record.json";
    ASSERT_FALSE(std::filesystem::exists(path))
        << "the scratch path must start empty, or this case cannot tell "
           "'created' from 'was already there'";

    const std::string text = sampleRecord(kGenA, kRecA, "PDFE-EylaBd", "FAIL");
    EXPECT_TRUE(writeConformanceRecordIfChanged(path, text))
        << "an absent record must be CREATED; 'nothing substantive changed' "
           "cannot be true of a file that does not exist";

    ASSERT_TRUE(std::filesystem::exists(path))
        << "the writer reported a write but there is no file at "
        << path.string() << " -- the return value alone is not evidence";
    EXPECT_EQ(slurp(path), text)
        << "the bytes on disk must be the bytes handed to the writer, "
           "including the volatile fields: they are genuine provenance and "
           "#155 explicitly chose not to strip them";

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST(ConformanceRecordWrites, ASubstantiveChangeIsWrittenAndTheOldContentIsGone) {
    const auto dir = scratchDir("substantive");
    const auto path = dir / "conformance-checker-record.json";

    const std::string fail = sampleRecord(kGenA, kRecA, "PDFE-EylaBd", "FAIL");
    ASSERT_TRUE(writeConformanceRecordIfChanged(path, fail));
    ASSERT_EQ(slurp(path), fail);

    // FAIL -> PASS is the change #155 names as the one that must never be
    // suppressed. Drive it through the real writer and read the result back.
    const std::string pass = sampleRecord(kGenA, kRecA, "PDFE-EylaBd", "PASS");
    ASSERT_NE(fail, pass);
    EXPECT_TRUE(writeConformanceRecordIfChanged(path, pass))
        << "a verdict flipping from FAIL to PASS MUST be written";

    const std::string onDisk = slurp(path);
    EXPECT_EQ(onDisk, pass);
    EXPECT_EQ(onDisk.find("\"verdict\": \"FAIL\""), std::string::npos)
        << "the superseded FAIL verdict is still in the file, so the write "
           "appended or partially overwrote rather than replacing";

    // Same again for the ISO clause id, the other substantive field.
    const std::string otherClause =
        sampleRecord(kGenA, kRecA, "PDFE-EylaBd", "PASS", "6.1.7-2");
    EXPECT_TRUE(writeConformanceRecordIfChanged(path, otherClause))
        << "a different failing ISO clause is a different record";
    EXPECT_NE(slurp(path).find("6.1.7-2"), std::string::npos);

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// The pair that makes the two directions one measurement. Without the skip
// half, "always write" passes the cases above; without the write half, "never
// write" passes everything before this section. Asserted against the same
// path, in the same test, so neither can be satisfied alone.
TEST(ConformanceRecordWrites, WritesOnSubstantiveChangeAndSkipsOnVolatileChange) {
    const auto dir = scratchDir("both-directions");
    const auto path = dir / "conformance-checker-record.json";

    const std::string first = sampleRecord(kGenA, kRecA, "PDFE-EylaBd", "FAIL");
    ASSERT_TRUE(writeConformanceRecordIfChanged(path, first));

    // (a) volatile-only movement: all three classes at once, no write.
    const std::string volatileOnly =
        sampleRecord(kGenB, kRecB, "PDFE-Zq7t1K", "FAIL");
    ASSERT_NE(first, volatileOnly);
    EXPECT_FALSE(writeConformanceRecordIfChanged(path, volatileOnly))
        << "timestamps and the temp-dir leaf moving is not a substantive "
           "change (GitHub #155)";
    EXPECT_EQ(slurp(path), first)
        << "the writer reported no write, so the file must still hold the "
           "ORIGINAL bytes -- not the new ones";

    // (b) substantive movement on top of the same volatile movement: written.
    const std::string substantive =
        sampleRecord(kGenB, kRecB, "PDFE-Zq7t1K", "PASS");
    EXPECT_TRUE(writeConformanceRecordIfChanged(path, substantive))
        << "a verdict change must be written even when it arrives alongside "
           "volatile movement -- that is the realistic shape of a real run";
    EXPECT_EQ(slurp(path), substantive);

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

// End to end through the SHIPPED entry point rather than the seam: damage the
// tracked record, flush, and confirm flushConformanceRecord() repaired it.
//
// This leaves the working tree CLEANER than it found it, not dirtier: the flush
// rewrites the file from the live record, which is by construction the content
// that belongs there. The original bytes are restored regardless, so a failure
// part-way through cannot leave the tree damaged.
TEST(ConformanceRecordWrites, FlushRewritesATrackedRecordThatNoLongerMatches) {
    const std::filesystem::path tracked =
        std::filesystem::path(PDFE_REPO_ROOT) / "docs" / "audit" /
        "meta-audit-2026-07" / "followup" / "phase-14" /
        "conformance-checker-record.json";

    // Establish the premise IN THIS PROCESS. This case asserts on a "verdict"
    // field, which only exists if the record holds at least one check; before
    // this call it depended on another shard having run one first.
    if (!recordOneLiveCheck()) {
        expectHonestlyUnavailable();
        return;
    }

    flushConformanceRecord();                 // settle
    ASSERT_TRUE(std::filesystem::exists(tracked)) << tracked.string();
    const std::string original = slurp(tracked);
    ASSERT_FALSE(original.empty());

    // A SUBSTANTIVE corruption: a verdict that is not what the record says.
    // Volatile-field noise would legitimately be ignored and would make this
    // case assert the opposite of what it means to.
    std::string damaged = original;
    const std::string from = "\"verdict\": \"";
    const std::size_t at = damaged.find(from);
    ASSERT_NE(at, std::string::npos)
        << "the record carries no \"verdict\" field, so this case cannot "
           "introduce a substantive difference and would be measuring nothing";
    damaged.insert(at + from.size(), "MUTANT-190-");
    ASSERT_NE(pdfe::test::conformanceRecordDigest(damaged),
              pdfe::test::conformanceRecordDigest(original))
        << "the corruption must be SUBSTANTIVE, or the writer is entitled to "
           "ignore it and this case proves nothing";

    {
        std::ofstream out(tracked, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out.good());
        out << damaged;
    }
    ASSERT_EQ(slurp(tracked), damaged) << "the damage did not land on disk";

    const int written = flushConformanceRecord();
    const std::string after = slurp(tracked);

    // Restore before asserting, so a failure cannot leave the tree damaged.
    if (after != original) {
        std::ofstream out(tracked, std::ios::binary | std::ios::trunc);
        out << original;
    }

    EXPECT_GE(written, 1)
        << "flushConformanceRecord() must report at least one write when a "
           "copy on disk no longer matches the record";
    EXPECT_EQ(pdfe::test::conformanceRecordDigest(after),
              pdfe::test::conformanceRecordDigest(original))
        << "the tracked record was NOT repaired: it still differs "
           "substantively from the record the checker holds. This is the "
           "does-write direction GitHub #190 reports as untested.";
    EXPECT_EQ(after.find("MUTANT-190-"), std::string::npos)
        << "the injected corruption survived the flush";
    EXPECT_EQ(slurp(tracked), original)
        << "the tracked file must be byte-identical to what it was before this "
           "case ran; a test that dirties the tracked record re-creates #155";
}

// The record lives where the documents cite it. If this moves, every citation
// in phase-14 and the #155 fix's own reasoning silently stops applying.
TEST(ConformanceRecordIdempotent, TheTrackedRecordIsWhereTheDocumentsCiteIt) {
    const std::filesystem::path tracked =
        std::filesystem::path(PDFE_REPO_ROOT) / "docs" / "audit" /
        "meta-audit-2026-07" / "followup" / "phase-14" /
        "conformance-checker-record.json";
    flushConformanceRecord();
    EXPECT_TRUE(std::filesystem::exists(tracked))
        << "expected the durable record at " << tracked.string();
}

}  // namespace
