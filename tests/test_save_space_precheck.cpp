// -*- coding: utf-8 -*-
//
// GitHub #100 / meta-audit WIN-78 — no free-space pre-check existed anywhere in
// src/ (`grep -rl GetDiskFreeSpace src` returned nothing). A save that could not
// fit was discovered only after the user had waited through the whole write.
//
// WHAT IS ASSERTED, AND WHY IN THIS SHAPE
//
//   * The estimate is REAL. An estimate that silently resolved to 0 would make
//     "free space >= estimate" trivially true forever, and the pre-check would
//     be a decoration that can never fire. Asserted against the actual file
//     size on disk.
//   * The comparison fires when free < estimate, and does NOT fire when it does
//     not. One without the other is a guard that can only ever say one thing.
//   * The check is WIRED INTO Document::save. On a healthy machine the result
//     of the check is invisible in the save's behaviour, so a test of the
//     function alone would keep passing after the call site was deleted. The
//     process-wide precheck record is what makes the call site observable.
//   * The pre-check is ADVISORY. Document::save must still reach and complete
//     its normal write path; #100's non-regression requirement is that
//     TempWriteBlockModes/TempWriteFailureTest.
//     EnospcDuringTempWriteLeavesOriginalByteIdentical keeps passing, which it
//     can only do if the pre-check never short-circuits the temp write.
#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/SaveSpacePrecheck.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <string>

using pdfe::core::SaveSpaceEstimate;
using pdfe::core::checkFreeSpaceForSave;
using pdfe::core::estimateSaveOutputBytes;
using pdfe::core::lastSaveSpacePrecheck;
using pdfe::core::resetSaveSpacePrecheckRecord;
using pdfe::core::setSaveSpaceWarningHandler;
using pdfe::core::warnIfInsufficientSpaceForSave;

namespace {

class SaveSpacePrecheckTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        dir_ = std::filesystem::temp_directory_path()
             / ("pdfe_issue100_" + std::to_string(counter_++));
        std::filesystem::create_directories(dir_);
        resetSaveSpacePrecheckRecord();
    }

    void TearDown() override
    {
        // Never leave a handler installed: it is process-wide and the next
        // test's save would fire this test's lambda into a dangling capture.
        setSaveSpaceWarningHandler({});
        // Same reasoning for the probe: it is process-wide, and one left
        // installed would make every later test in the binary read a
        // simulated volume.
        pdfe::core::setFreeSpaceProbeForTesting({});
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }

    std::filesystem::path writeFile(const std::string& name, std::size_t bytes)
    {
        const auto p = dir_ / name;
        std::ofstream out(p, std::ios::binary | std::ios::trunc);
        out << std::string(bytes, 'A');
        out.close();
        return p;
    }

    std::filesystem::path dir_;
    static int counter_;
};

int SaveSpacePrecheckTest::counter_ = 0;

TEST_F(SaveSpacePrecheckTest, EstimateIsTheRealOnDiskSize)
{
    const auto f = writeFile("estimate.bin", 4096);
    EXPECT_EQ(estimateSaveOutputBytes(f), 4096u)
        << "the output estimate is not derived from the document on disk; a "
           "wrong-by-any-amount estimate makes the whole check advisory noise";

    // "No estimate" must be reported as 0 and must NOT be reported as a
    // satisfied check further down.
    EXPECT_EQ(estimateSaveOutputBytes(dir_ / "does_not_exist.pdf"), 0u);
}

TEST_F(SaveSpacePrecheckTest, ReportsInsufficientOnlyWhenFreeSpaceIsBelowEstimate)
{
    const auto target = writeFile("target.pdf", 64);

    // An estimate no volume can satisfy. Chosen rather than a mocked filesystem
    // so the comparison runs against a REAL std::filesystem::space() reading.
    const std::uintmax_t impossible = std::numeric_limits<std::uintmax_t>::max() / 4;
    const SaveSpaceEstimate low = checkFreeSpaceForSave(target, impossible);
    ASSERT_TRUE(low.determined)
        << "free space could not be read for " << target.string()
        << " — the check has no opinion and the test proves nothing";
    EXPECT_TRUE(low.insufficient)
        << "an estimate of " << impossible << " bytes against " << low.freeBytes
        << " free was NOT reported as insufficient — the pre-check can never "
           "fire (GitHub #100)";

    // The other side. Without this the assertion above would be satisfied by a
    // check hard-coded to say "insufficient".
    const SaveSpaceEstimate ok = checkFreeSpaceForSave(target, 1);
    ASSERT_TRUE(ok.determined);
    EXPECT_FALSE(ok.insufficient)
        << "a 1-byte estimate against " << ok.freeBytes
        << " free was reported as insufficient — the check always warns";

    // A target that does not exist yet (the Save As case) still resolves,
    // via its parent directory.
    const SaveSpaceEstimate newTarget =
        checkFreeSpaceForSave(dir_ / "not_yet.pdf", 1);
    EXPECT_TRUE(newTarget.determined)
        << "free space could not be determined for a not-yet-created target, "
           "so Save As would never be pre-checked";
}

TEST_F(SaveSpacePrecheckTest, WarningHandlerFiresOnlyWhenSpaceIsInsufficient)
{
    const auto target = dir_ / "warned.pdf";

    int warnings = 0;
    SaveSpaceEstimate seen;
    std::filesystem::path seenTarget;
    setSaveSpaceWarningHandler(
        [&](const SaveSpaceEstimate& est, const std::filesystem::path& t) {
            ++warnings;
            seen = est;
            seenTarget = t;
        });

    (void)warnIfInsufficientSpaceForSave(target, static_cast<std::uintmax_t>(1));
    EXPECT_EQ(warnings, 0)
        << "the user was warned about disk space for a 1-byte save";

    const std::uintmax_t impossible = std::numeric_limits<std::uintmax_t>::max() / 4;
    (void)warnIfInsufficientSpaceForSave(target, impossible);
    ASSERT_EQ(warnings, 1)
        << "no warning was raised for a save that cannot possibly fit "
           "(GitHub #100)";
    EXPECT_EQ(seenTarget, target);
    EXPECT_EQ(seen.estimatedBytes, impossible);
    EXPECT_TRUE(seen.insufficient);
    EXPECT_LT(seen.freeBytes, seen.estimatedBytes)
        << "the warning carried figures that do not justify it";
}

TEST_F(SaveSpacePrecheckTest, DocumentSaveRunsThePrecheckAgainstItsTarget)
{
    const auto fixture = std::filesystem::path(TEST_FIXTURES_DIR) / "test_3page.pdf";
    ASSERT_TRUE(std::filesystem::exists(fixture))
        << "tracked fixture missing: " << fixture.string();

    const auto working = dir_ / "save_target.pdf";
    std::filesystem::copy_file(fixture, working,
                               std::filesystem::copy_options::overwrite_existing);

    auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
    auto doc = pdfe::core::Document::open(ctx, working);
    ASSERT_NE(doc, nullptr);

    resetSaveSpacePrecheckRecord();
    ASSERT_EQ(lastSaveSpacePrecheck().sequence, 0u);

    ASSERT_NO_THROW(doc->save(working));

    const auto record = lastSaveSpacePrecheck();
    EXPECT_GT(record.sequence, 0u)
        << "Document::save performed NO free-space pre-check — the call site "
           "is missing (GitHub #100)";
    EXPECT_EQ(record.target, working)
        << "the pre-check ran against a different path than the save target, "
           "so it measured the wrong volume";
    EXPECT_GT(record.estimate.estimatedBytes, 0u)
        << "the pre-check ran with a zero estimate, which can never be "
           "insufficient — it is wired up but inert";
    EXPECT_TRUE(record.estimate.determined);

    // ADVISORY, NOT A GATE: the save completed and produced a readable file.
    EXPECT_TRUE(std::filesystem::exists(working));
    EXPECT_GT(std::filesystem::file_size(working), 0u);
}

// ===========================================================================
// GitHub #100, second half: the seam at the volume boundary.
//
// The three tests above all run against the REAL volume, which is why none of
// them can reach the warning path on a healthy machine without an absurd
// estimate. These pin the seam that lets a test simulate a full volume with an
// ORDINARY estimate — the state the GUI dialog assertion needs, and the half
// that was missing when #100 was closed the first time.
// ===========================================================================

TEST_F(SaveSpacePrecheckTest, InstalledProbeReplacesTheVolumeReading)
{
    const auto target = writeFile("probed.pdf", 4096);

    EXPECT_FALSE(pdfe::core::freeSpaceProbeInstalled())
        << "a probe was already installed before this test installed one; a "
           "previous test leaked process state and nothing below means what "
           "it says";

    // CONTROL FIRST: on the real volume, a 4096-byte estimate is comfortably
    // satisfiable. Without this the assertion after it could be satisfied by a
    // check that always says "insufficient".
    const SaveSpaceEstimate real = checkFreeSpaceForSave(target, 4096);
    ASSERT_TRUE(real.determined);
    ASSERT_FALSE(real.insufficient)
        << "this machine genuinely has under 4096 bytes free; the control "
           "cannot distinguish the seam from the volume";

    std::filesystem::path probedPath;
    int probeCalls = 0;
    pdfe::core::setFreeSpaceProbeForTesting(
        [&](const std::filesystem::path& p, std::uintmax_t& out) {
            ++probeCalls;
            probedPath = p;
            out = 100;  // a nearly-full volume
            return true;
        });
    EXPECT_TRUE(pdfe::core::freeSpaceProbeInstalled());

    const SaveSpaceEstimate simulated = checkFreeSpaceForSave(target, 4096);
    EXPECT_EQ(probeCalls, 1)
        << "the installed probe was never called — checkFreeSpaceForSave still "
           "reads the real volume and a full disk cannot be simulated at all";
    EXPECT_EQ(probedPath, target)
        << "the probe was asked about a different path than the save target";
    ASSERT_TRUE(simulated.determined);
    EXPECT_EQ(simulated.freeBytes, 100u);
    EXPECT_TRUE(simulated.insufficient)
        << "100 bytes free against a 4096-byte estimate was not reported as "
           "insufficient — the PRODUCTION comparison is not being applied to "
           "the simulated figure";

    // The verdict is still computed, not dictated: same probe, smaller
    // estimate, opposite answer.
    const SaveSpaceEstimate fits = checkFreeSpaceForSave(target, 10);
    EXPECT_TRUE(fits.determined);
    EXPECT_FALSE(fits.insufficient)
        << "a 10-byte estimate against 100 simulated free bytes was called "
           "insufficient — the seam is forcing the verdict rather than "
           "supplying the free-space figure";

    EXPECT_EQ(probeCalls, 2)
        << "the second check did not consult the probe either";

    // And removing it restores the real reading.
    const int callsBeforeRemoval = probeCalls;
    pdfe::core::setFreeSpaceProbeForTesting({});
    EXPECT_FALSE(pdfe::core::freeSpaceProbeInstalled());
    const SaveSpaceEstimate restored = checkFreeSpaceForSave(target, 4096);
    EXPECT_EQ(probeCalls, callsBeforeRemoval)
        << "the probe was called after being removed";
    EXPECT_FALSE(restored.insufficient)
        << "removing the probe did not restore the real volume reading";
}

TEST_F(SaveSpacePrecheckTest, ProbeThatCannotAnswerYieldsNoOpinionNotSufficient)
{
    const auto target = writeFile("undetermined.pdf", 4096);

    pdfe::core::setFreeSpaceProbeForTesting(
        [](const std::filesystem::path&, std::uintmax_t&) { return false; });

    const SaveSpaceEstimate est = checkFreeSpaceForSave(target, 4096);
    EXPECT_FALSE(est.determined)
        << "a probe that reported it could not answer produced a DETERMINED "
           "estimate — an unanswerable question is being turned into a clean "
           "bill of health, which is the exact contract the header forbids";
    EXPECT_FALSE(est.insufficient);
}

TEST_F(SaveSpacePrecheckTest, SimulatedFullVolumeFiresTheWarningHandlerThroughTheRealPath)
{
    // This is the headless mirror of the live-UI assertion: the same
    // simulation, the same dispatch, but with the handler counted instead of a
    // QMessageBox observed. If this is green and the UI test is red, the fault
    // is in the GUI handler installed by main.cpp, not in the pre-check.
    const auto target = writeFile("fullvolume.pdf", 8192);

    int warnings = 0;
    SaveSpaceEstimate seen;
    setSaveSpaceWarningHandler(
        [&](const SaveSpaceEstimate& est, const std::filesystem::path&) {
            ++warnings;
            seen = est;
        });

    // Control: the real volume warns about nothing for an 8 KB save.
    (void)warnIfInsufficientSpaceForSave(target, target);
    ASSERT_EQ(warnings, 0)
        << "the unsimulated case already warned; the simulation below would "
           "prove nothing";

    pdfe::core::setFreeSpaceProbeForTesting(
        [](const std::filesystem::path&, std::uintmax_t& out) {
            out = 512;
            return true;
        });

    const SaveSpaceEstimate result = warnIfInsufficientSpaceForSave(target, target);
    EXPECT_EQ(warnings, 1)
        << "a simulated volume with 512 bytes free did not warn about an "
           "8192-byte save — the warning path is unreachable and GitHub #100 "
           "would again be closed on the pre-check alone";
    EXPECT_EQ(seen.freeBytes, 512u);
    EXPECT_EQ(seen.estimatedBytes, 8192u)
        << "the estimate reaching the handler is not the document's real size";
    EXPECT_TRUE(result.insufficient);

    // The record is updated too, so the live-UI test's readback is trustworthy.
    EXPECT_GT(lastSaveSpacePrecheck().sequence, 0u);
    EXPECT_TRUE(lastSaveSpacePrecheck().estimate.insufficient);
}

}  // namespace
