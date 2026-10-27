// -*- coding: utf-8 -*-
// ===========================================================================
// Meta-audit follow-up F47 item F5 — phase-49.
//
// Fault injection on the LTV /DSS save path.
//
// WHY THIS PATH IS DIFFERENT FROM EVERY OTHER SAVE PDFE PERFORMS, AND WHY
// NOTHING HAD EVER INJECTED A FAULT INTO IT.
//
// PDFE's ordinary save is temp-then-atomic-rename: write "<target>.pdfe_tmp",
// flush it, then MoveFileExW it over the target. Phase-10's F10 harness
// (tests/support/SaveFailureInjection.h) fault-injects that sequence at three
// named points, and the safety property it asserts — "the original is
// byte-identical afterwards" — is structurally GUARANTEED by the design: a
// failed save never touches the original at all.
//
// LtvDssWriter::write() did not use that sequence. It mutated the document in
// memory and called Document::saveIncremental(), i.e.
// pdf_save_document(do_incremental=1) directly onto the LIVE TARGET FILE. The
// bytes went on the end of the user's only copy. Nothing was staged, nothing
// was renamed, and so nothing about the design guaranteed the safety property —
// it had to be MEASURED. Before this file, it never was: TempWriteBlocker and
// RenameLock both poison "<target>.pdfe_tmp", a path that save never created,
// so they arm successfully and then inject nothing. That is the harness
// blindness DCA 0015 records.
//
// PAST TENSE SINCE F58 (phases 52–53). The /DSS write now commits through
// Document::saveIncrementalStaged(), which stages and publishes by an atomic
// rename — see the premise-inversion block above
// DssWriteStagesAndLeavesNoTempBehind further down this file, which is where
// this file's own invariant was flipped rather than deleted. TempWriteBlocker
// and RenameLock are STILL blind to it, for a new reason: the staging file is
// "<target>.pdfe_part.<128-bit CSPRNG hex>" behind an exclusive handle (F58-a),
// so it cannot be poisoned by name at all. Nothing below may treat a green
// result from either blocker as evidence about the staged append.
//
// This file uses InPlaceWriteBlocker (added to the shared F10 harness rather
// than open-coded here) to make the target genuinely unwritable at the OS
// level, so MuPDF's own fopen fails for real inside pdf_save_document.
// ===========================================================================

#include <gtest/gtest.h>

#include "support/SaveFailureInjection.h"

#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "signatures/LtvDssWriter.h"
#include "signatures/SignatureManager.h"

#include <QRectF>

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace pdfe::core;
using namespace pdfe::signatures;
using pdfe::testing::InPlaceWriteBlocker;
using pdfe::testing::fileIsByteIdenticalTo;
using pdfe::testing::readAllBytes;

namespace {

std::filesystem::path workDir()
{
    auto d = std::filesystem::temp_directory_path() / "pdfe_dss_fault";
    std::filesystem::create_directories(d);
    return d;
}

// ---------------------------------------------------------------------------
// Staging-residue helpers (DCA 0015, phase-10).
//
// The residue assertions in this file must name every shape the save layer can
// leave behind, not the one shape that happened to be current when they were
// written. stagingResidueOf() is the single authority for that set — the fixed
// "<target>.pdfe_tmp" of the full-rewrite branch PLUS every enumerated
// "<target>.pdfe_part.<hex>" sibling of the staged incremental branch — so
// these two helpers exist to keep both the pre-clean and the assertion routed
// through it. A test that pre-cleans one shape and asserts on another is how
// DCA 0015's control became unfalsifiable in the first place.
// ---------------------------------------------------------------------------
void clearStagingResidue(const std::filesystem::path& target)
{
    for (const auto& p : pdfe::testing::stagingResidueOf(target)) {
        std::error_code ec;
        std::filesystem::remove_all(p, ec);
    }
}

std::string describeResidue(const std::vector<std::filesystem::path>& residue)
{
    if (residue.empty()) return "<nothing>";
    std::string s;
    for (const auto& p : residue) {
        if (!s.empty()) s += ", ";
        s += p.string();
    }
    return s;
}

// ---------------------------------------------------------------------------
// F57 (phase-52): arm the rollback-capture fault for the duration of a scope.
//
// This is the ONE thing in this file that is not a real-world failure, and the
// reason is recorded in LtvDssWriter.cpp next to the gate itself: every
// document that genuinely drives pdf_deep_copy_obj() to null (a /Root/DSS
// whose indirect target will not load) ALSO makes writeDssDict() throw before
// it mutates anything, so the "mutation left behind" half of the defect can
// never be reached from a fixture alone. The gate separates the null return
// from the broken document that would otherwise be inseparable from it.
//
// The real deep copy still runs; only its result is discarded.
// ---------------------------------------------------------------------------
class ScopedDssCaptureFault {
public:
    ScopedDssCaptureFault()
    {
#ifdef _WIN32
        _putenv_s(kVar, "1");
#else
        setenv(kVar, "1", 1);
#endif
    }
    ~ScopedDssCaptureFault() { disarm(); }

    ScopedDssCaptureFault(const ScopedDssCaptureFault&) = delete;
    ScopedDssCaptureFault& operator=(const ScopedDssCaptureFault&) = delete;

    void disarm()
    {
#ifdef _WIN32
        _putenv_s(kVar, "");
#else
        unsetenv(kVar);
#endif
    }

    /// Confirms the variable really is set in THIS process's environment — an
    /// unarmed fault would turn every case below into a happy-path run that
    /// passes for the wrong reason, exactly as an unarmed InPlaceWriteBlocker
    /// would.
    static bool armed()
    {
#ifdef _WIN32
        size_t len = 0;
        char buf[8]{};
        if (getenv_s(&len, buf, sizeof(buf), kVar) != 0) return false;
        return len > 0 && buf[0] != '\0' && buf[0] != '0';
#else
        const char* v = std::getenv(kVar);
        return v && v[0] != '\0' && v[0] != '0';
#endif
    }

private:
    static constexpr const char* kVar = "PDFE_TEST_DSS_ROLLBACK_CAPTURE_FAULT";
};

// Sign a fixture PDF so there is a real /V/Contents for LtvDssWriter to hang a
// /DSS off. Mirrors the known-good flow in tests/test_ltv_dss_writer.cpp.
//
// F57 made the source parameterisable: the pre-existing-/DSS cases need
// dss_pre_direct.pdf / dss_pre_indirect.pdf rather than test_single.pdf.
// Signing is an incremental append onto a full rewrite, and both are
// structure-preserving, so a /Root/DSS that went in indirect comes out
// indirect — which the tests verify rather than assume.
bool buildSignedFixtureFrom(std::shared_ptr<MuPdfContext> ctx,
                            const char* sourceName,
                            const std::filesystem::path& outPath)
{
    auto fixtures = std::filesystem::path(TEST_FIXTURES_DIR);
    auto pfx = fixtures / "test_signer.pfx";
    auto srcPdf = fixtures / sourceName;
    if (!std::filesystem::exists(pfx) || !std::filesystem::exists(srcPdf))
        return false;

    std::error_code ec;
    std::filesystem::remove(outPath, ec);
    SignatureManager mgr(ctx);
    try {
        {
            auto doc = Document::open(ctx, srcPdf);
            if (!doc) return false;
            auto page = doc->page(0);
            mgr.createSignatureField(page.get(), "DssFaultSig",
                                     QRectF(100, 500, 200, 50));
            doc->saveAs(outPath);
        }
        {
            auto doc = Document::open(ctx, outPath);
            if (!doc) return false;
            auto page = doc->page(0);
            SigningOptions opts;
            opts.pfxPath = pfx.string();
            opts.pfxPassword = "test123";
            opts.reason = "F5 DSS fault injection";
            mgr.signField(doc.get(), page.get(), 0, opts);
        }
    } catch (...) {
        return false;
    }
    return std::filesystem::exists(outPath);
}

class DssSaveFaultInjectionTest : public ::testing::Test {
protected:
    static void SetUpTestSuite()
    {
        sigCtx_ = std::make_shared<MuPdfContext>();
        master_ = workDir() / "dss_fault_master.pdf";
        if (!buildSignedFixtureFrom(sigCtx_, "test_single.pdf", master_))
            master_.clear();

        // F57 — the two "document already HAS a /DSS" masters. See
        // tests/fixtures/make_dss_pre_fixtures.py for how they are built and
        // why the indirect one has to be hand-made.
        preDirect_ = workDir() / "dss_fault_pre_direct.pdf";
        if (!buildSignedFixtureFrom(sigCtx_, "dss_pre_direct.pdf", preDirect_))
            preDirect_.clear();
        preIndirect_ = workDir() / "dss_fault_pre_indirect.pdf";
        if (!buildSignedFixtureFrom(sigCtx_, "dss_pre_indirect.pdf",
                                    preIndirect_))
            preIndirect_.clear();
    }
    static void TearDownTestSuite()
    {
        std::error_code ec;
        if (!master_.empty()) std::filesystem::remove(master_, ec);
        if (!preDirect_.empty()) std::filesystem::remove(preDirect_, ec);
        if (!preIndirect_.empty()) std::filesystem::remove(preIndirect_, ec);
        sigCtx_.reset();
    }

    void SetUp() override
    {
        if (master_.empty() || !std::filesystem::exists(master_)) {
            GTEST_SKIP() << "on-the-fly signing failed; check test_signer.pfx "
                            "and the PKCS#7 bridge";
        }
        ctx = sigCtx_;
    }

    // A per-test copy of the signed master, plus the signature inputs the DSS
    // write needs.
    std::filesystem::path workCopyOf(const std::filesystem::path& source,
                                     const char* name, SignatureInputs& out)
    {
        auto dst = workDir() / name;
        std::error_code ec;
        std::filesystem::permissions(dst, std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::add, ec);
        std::filesystem::remove(dst, ec);
        std::filesystem::copy_file(source, dst, ec);
        if (ec) return {};
        auto doc = Document::open(ctx, dst);
        if (!doc || !LtvDssWriter::extractFirstSignatureInputs(doc.get(), out))
            return {};
        return dst;
    }

    std::filesystem::path workCopy(const char* name, SignatureInputs& out)
    {
        return workCopyOf(master_, name, out);
    }

    // F57 helper: what does the file on disk say about /DSS right now?
    DssCounts diskCounts(const std::filesystem::path& p,
                         const std::string& vriKey)
    {
        DssCounts c{};
        auto doc = Document::open(ctx, p);
        if (doc) LtvDssWriter::inspectDss(doc.get(), vriKey, c);
        return c;
    }

    // ── F57 shared bodies ───────────────────────────────────────────────
    // Both /DSS shapes get the identical treatment; only the fixture and the
    // expected pdf_is_indirect() verdict differ. Keeping the bodies shared is
    // what makes "the indirect case is covered in BOTH directions" checkable
    // by inspection rather than by trusting two hand-written near-copies.

    /// The rollback WORKS: a pre-existing /DSS survives a failed /DSS write
    /// byte-for-byte, in memory as well as on disk.
    void checkPreExistingDssSurvivesAFailedWrite(
        const std::filesystem::path& master, const char* copyName,
        bool expectIndirect)
    {
        ASSERT_FALSE(master.empty())
            << "the pre-existing-/DSS master could not be signed";
        SignatureInputs inputs;
        auto work = workCopyOf(master, copyName, inputs);
        ASSERT_FALSE(work.empty());
        ASSERT_FALSE(inputs.certChain.empty());
        const auto vriKey = LtvDssWriter::sha1HexUpper(inputs.contentsHex);

        const DssCounts before = diskCounts(work, vriKey);
        ASSERT_TRUE(before.hasDss)
            << "the fixture is supposed to ALREADY have a /DSS — without one "
               "this exercises the create-then-delete branch, not the "
               "snapshot-and-restore branch it claims to";
        ASSERT_EQ(before.dssIsIndirect, expectIndirect)
            << "/Root/DSS is not the shape this case exists to probe "
               "(pdf_is_indirect says "
            << before.dssIsIndirect << ", expected " << expectIndirect << ")";
        ASSERT_EQ(before.topCerts, 1);
        ASSERT_FALSE(before.hasVriKey);

        {
            auto doc = Document::open(ctx, work);
            ASSERT_NE(doc, nullptr);
            {
                InPlaceWriteBlocker blocker(work);
                ASSERT_TRUE(blocker.armed());
                LtvDssWriter writer;
                ASSERT_FALSE(writer.write(doc.get(), inputs.contentsHex,
                                          {inputs.certChain.front()}));
            }
            // The obstruction is gone; an unrelated save must not carry the
            // rejected /DSS additions to disk.
            try {
                doc->saveIncremental();
            } catch (...) {
            }
        }

        const DssCounts after = diskCounts(work, vriKey);
        EXPECT_TRUE(after.hasDss)
            << "the rollback DELETED a /DSS that was already there before the "
               "write — restoring is not the same as removing";
        EXPECT_EQ(after.dssIsIndirect, expectIndirect)
            << "the rollback changed the SHAPE of /Root/DSS";
        EXPECT_EQ(after.topCerts, before.topCerts)
            << "the rejected /Certs entry was published by the later save";
        EXPECT_FALSE(after.hasVriKey)
            << "the rejected /VRI entry was published by the later save";
    }

    /// The rollback CANNOT work: the snapshot of the pre-existing /DSS came
    /// back null. The write must be refused outright, with a surfaced reason,
    /// and nothing may be mutated — because a mutation here could never be
    /// undone. This is the case the two-branch rollbackDss() fell through.
    void checkCaptureFailureAbortsBeforeAnyMutation(
        const std::filesystem::path& master, const char* copyName,
        bool expectIndirect)
    {
        ASSERT_FALSE(master.empty())
            << "the pre-existing-/DSS master could not be signed";
        SignatureInputs inputs;
        auto work = workCopyOf(master, copyName, inputs);
        ASSERT_FALSE(work.empty());
        ASSERT_FALSE(inputs.certChain.empty());
        const auto vriKey = LtvDssWriter::sha1HexUpper(inputs.contentsHex);

        const DssCounts before = diskCounts(work, vriKey);
        ASSERT_TRUE(before.hasDss);
        ASSERT_EQ(before.dssIsIndirect, expectIndirect);
        ASSERT_EQ(before.topCerts, 1);
        ASSERT_FALSE(before.hasVriKey);
        const std::string beforeBytes = readAllBytes(work);
        ASSERT_FALSE(beforeBytes.empty());

        // ── Phase A: snapshot forced to fail. Nothing may happen. ──
        // Deliberately NO write blocker: the target is perfectly writable, so
        // the ONLY thing that can stop the /DSS being published is the code
        // under test refusing to proceed.
        {
            ScopedDssCaptureFault fault;
            ASSERT_TRUE(ScopedDssCaptureFault::armed())
                << "the capture fault did not arm; this case would otherwise "
                   "pass as an ordinary happy path";

            auto doc = Document::open(ctx, work);
            ASSERT_NE(doc, nullptr);
            LtvDssWriter writer;
            EXPECT_FALSE(writer.write(doc.get(), inputs.contentsHex,
                                      {inputs.certChain.front()}))
                << "a /DSS write whose rollback snapshot could not be taken "
                   "proceeded anyway — it has mutated a document it cannot "
                   "put back (F57)";
            // `contains` rather than `==`: MuPdfContext::execute prefixes
            // "MuPDF error: " when it converts the fz_throw into a C++
            // exception. The prefix is not the claim — the identified reason
            // is, and no other failure in this path produces it.
            EXPECT_NE(writer.lastError().find(
                          LtvDssWriter::kRollbackCaptureFailedMessage),
                      std::string::npos)
                << "the refusal was not reported as a rollback-capture "
                   "failure, so either a different failure fired (and the "
                   "injector proved nothing) or the reason was not surfaced "
                   "at all. Actual: '" << writer.lastError() << "'";

            // ...and the mutation must not be sitting in the open document
            // waiting for the next save to publish it.
            try {
                doc->saveIncremental();
            } catch (...) {
            }
        }

        EXPECT_TRUE(fileIsByteIdenticalTo(work, beforeBytes))
            << "the refused /DSS write changed the file on disk";
        const DssCounts afterRefusal = diskCounts(work, vriKey);
        EXPECT_TRUE(afterRefusal.hasDss);
        EXPECT_EQ(afterRefusal.dssIsIndirect, expectIndirect);
        EXPECT_EQ(afterRefusal.topCerts, before.topCerts)
            << "an un-rollbackable /DSS mutation was committed and then "
               "published — the caller was told the write failed and it "
               "happened anyway (F57, the F5 defect inside its own fix)";
        EXPECT_FALSE(afterRefusal.hasVriKey)
            << "the /VRI entry from a refused write reached the disk";

        // ── Phase B: THE POSITIVE CONTROL, and the proof the fault fired. ──
        // Identical document, identical call, one difference: the fault is
        // disarmed. If this did not succeed, Phase A's "returned false and
        // changed nothing" would be indistinguishable from /DSS writing simply
        // being broken for this fixture shape.
        ASSERT_FALSE(ScopedDssCaptureFault::armed())
            << "the fault outlived its scope";
        {
            auto doc = Document::open(ctx, work);
            ASSERT_NE(doc, nullptr);
            LtvDssWriter writer;
            ASSERT_TRUE(writer.write(doc.get(), inputs.contentsHex,
                                     {inputs.certChain.front()}))
                << "the same write fails with nothing injected, so Phase A "
                   "proves nothing: " << writer.lastError();
            EXPECT_TRUE(writer.lastError().empty());
        }
        const DssCounts afterControl = diskCounts(work, vriKey);
        EXPECT_EQ(afterControl.topCerts, before.topCerts + 1)
            << "the unobstructed write reported success but appended no cert";
        EXPECT_TRUE(afterControl.hasVriKey)
            << "the unobstructed write reported success but wrote no /VRI";
        EXPECT_EQ(afterControl.dssIsIndirect, expectIndirect)
            << "the successful write changed the SHAPE of /Root/DSS";
    }

    std::shared_ptr<MuPdfContext> ctx;
    static std::shared_ptr<MuPdfContext> sigCtx_;
    static std::filesystem::path master_;
    static std::filesystem::path preDirect_;
    static std::filesystem::path preIndirect_;
};

std::shared_ptr<MuPdfContext> DssSaveFaultInjectionTest::sigCtx_;
std::filesystem::path DssSaveFaultInjectionTest::master_;
std::filesystem::path DssSaveFaultInjectionTest::preDirect_;
std::filesystem::path DssSaveFaultInjectionTest::preIndirect_;

}  // namespace

// ───────────────────────────────────────────────────────────────────────
// F58 (phase-52) — THE PREMISE OF THIS FILE HAS BEEN INVERTED ON PURPOSE.
//
// This test used to be DssWriteNeverStagesAnAtomicRenameTempFile, and it
// asserted the opposite of what it asserts now: that the /DSS save never goes
// near "<target>.pdfe_tmp". That was a true statement about a defective design
// — the append went straight onto the user's only copy — and the test existed
// to keep the rest of this file honest about which mechanism it was measuring.
// Its own comment said what to do if it ever went red: "the DSS path has been
// re-routed through the atomic sequence — which would be an improvement, but
// ... they must be revisited rather than trusted."
//
// F58 made exactly that change, and the cases below have been revisited
// accordingly (see the F58 block at the end of this file). The invariant is
// therefore flipped rather than deleted: the /DSS save MUST stage, and it must
// leave no residue behind when it succeeds.
// ───────────────────────────────────────────────────────────────────────
TEST_F(DssSaveFaultInjectionTest, DssWriteStagesAndLeavesNoTempBehind)
{
    SignatureInputs inputs;
    auto work = workCopy("dss_fault_notemp.pdf", inputs);
    ASSERT_FALSE(work.empty());
    ASSERT_FALSE(inputs.certChain.empty());

    // DCA 0015 / phase-10 — WHICH RESIDUE SHAPE THIS LOOKS FOR, AND WHY IT
    // CHANGED. This used to check saveTempPathFor(work), i.e. the fixed
    // "<target>.pdfe_tmp". F58-a (phase-53) moved the staged incremental append
    // onto core::StagingFile's "<target>.pdfe_part.<128-bit hex>", so the
    // /DSS writer can no longer create a .pdfe_tmp at all — the assertion was
    // checking for the absence of a file nothing in this path produces, which
    // no mutation of the cleanup code could redden. stagingResidueOf() returns
    // BOTH shapes (fixed temp + enumerated .pdfe_part siblings), so the check
    // now covers the path actually taken and stays correct if the writer is
    // ever re-routed back through the full-rewrite branch.
    clearStagingResidue(work);

    const std::string before = readAllBytes(work);
    ASSERT_FALSE(before.empty());

    // The "Stages" half of this test's name (DCA 0015, phase-10). Until now
    // NOTHING here asserted it: the residue check below is satisfied both by
    // "staged and cleaned up" and by "never staged", and the byte-prefix check
    // is satisfied by an in-place append too. So reverting the writer to the
    // unstaged saveIncremental() — the exact regression this record warns
    // about — left the whole test green. File identity discriminates them
    // without racing the write; see the header note on fileIdentityOf().
    const auto idBefore = pdfe::testing::fileIdentityOf(work);
    ASSERT_TRUE(idBefore.valid)
        << "could not read the target's file identity before the save; the "
           "staging assertion below would be vacuous";

    {
        auto doc = Document::open(ctx, work);
        ASSERT_NE(doc, nullptr);
        LtvDssWriter writer;
        ASSERT_TRUE(writer.write(doc.get(), inputs.contentsHex,
                                 {inputs.certChain.front()}))
            << writer.lastError();
    }

    const auto idAfter = pdfe::testing::fileIdentityOf(work);
    ASSERT_TRUE(idAfter.valid) << "the target vanished across the save";
    EXPECT_NE(idAfter, idBefore)
        << "the /DSS write did NOT publish by rename — the target is still the "
           "same file object it was before the save, which means the append "
           "went straight onto the user's only signed copy (the pre-F58 shape). "
           "A mid-write failure there leaves a broken trailing xref and a "
           "/ByteRange that no longer resolves.";

    // The write happened...
    const std::string after = readAllBytes(work);
    EXPECT_NE(after, before);
    // ...the original bytes are still a PREFIX of it (this is an incremental
    // append staged through a copy, not a rewrite — if this ever fails, every
    // existing signature's /ByteRange has just been invalidated)...
    ASSERT_GT(after.size(), before.size());
    EXPECT_EQ(after.compare(0, before.size(), before), 0)
        << "the staged incremental save did not preserve the original bytes as "
           "a byte-identical prefix — /ByteRange is broken";
    // ...and nothing was left lying around.
    const auto residue = pdfe::testing::stagingResidueOf(work);
    EXPECT_TRUE(residue.empty())
        << "a SUCCESSFUL staged /DSS save left " << describeResidue(residue)
        << " behind; the atomic rename is supposed to consume the staging file";
}

// ───────────────────────────────────────────────────────────────────────
// F58 — the staged save must clean the temp up on the FAILURE paths too, not
// only on the success path where the rename consumes it. A blocked target
// fails at the commit, after the staging copy and the append have both
// completed and produced a real file on disk — so this is the case where a
// missing cleanup would leave litter.
// ───────────────────────────────────────────────────────────────────────
TEST_F(DssSaveFaultInjectionTest, FailedStagedDssWriteLeavesNoTempResidue)
{
    SignatureInputs inputs;
    auto work = workCopy("dss_fault_residue.pdf", inputs);
    ASSERT_FALSE(work.empty());
    ASSERT_FALSE(inputs.certChain.empty());

    // DCA 0015 / phase-10 — see the note on DssWriteStagesAndLeavesNoTempBehind
    // above. This case is the one the record's own addendum warned about, so it
    // is the one that most needed to be looking at the right filename.
    clearStagingResidue(work);
    const std::string before = readAllBytes(work);
    ASSERT_FALSE(before.empty());

    std::string why;
    {
        auto doc = Document::open(ctx, work);
        ASSERT_NE(doc, nullptr);
        InPlaceWriteBlocker blocker(work);
        ASSERT_TRUE(blocker.armed());
        LtvDssWriter writer;
        EXPECT_FALSE(writer.write(doc.get(), inputs.contentsHex,
                                  {inputs.certChain.front()}));
        why = writer.lastError();
    }

    EXPECT_TRUE(fileIsByteIdenticalTo(work, before));

    // WHERE THE SAVE FAILED, and why this assertion is what makes the residue
    // check below mean anything (DCA 0015, phase-10 — raised by the phase's own
    // validator gate, which was right).
    //
    // "The failure path left no staging residue" is satisfied for free by a save
    // that never staged: an unstaged incremental append has nothing to leave
    // behind. So this test, like the class-level one above, has to establish
    // that the mechanism RAN before its cleanup claim is worth anything — and
    // here it cannot do that with file identity, because the save FAILED and
    // therefore correctly published nothing and renamed nothing.
    //
    // What separates the two designs is WHERE InPlaceWriteBlocker bites, and
    // that is externally visible in the reported error. Measured, both ways:
    //
    //   staged   → "Failed to atomically publish the incremental save (original
    //              left intact): Access is denied."
    //              ...the staging copy and the whole append RAN; the COMMIT was
    //              refused. This is the path whose cleanup we are asserting.
    //   unstaged → "MuPDF error: cannot open file '...': Permission denied"
    //              ...fopen was refused; not one byte was written anywhere.
    //
    // Asserted against Document::kAtomicPublishFailedPrefix rather than a
    // literal, so a reword of the message is a visible edit to a declared
    // contract instead of a silently-passing substring miss (DCA 0028).
    EXPECT_NE(why.find(Document::kAtomicPublishFailedPrefix), std::string::npos)
        << "the /DSS save did not fail at the atomic PUBLISH, so the staging "
           "copy and the append never ran and there was never anything for the "
           "failure path to clean up — which makes the residue assertion below "
           "vacuous. Getting MuPDF's 'cannot open file' here means the write "
           "went back to an unstaged in-place append. lastError() was: \""
        << why << "\"";

    const auto residue = pdfe::testing::stagingResidueOf(work);
    EXPECT_TRUE(residue.empty())
        << "a FAILED staged /DSS save left " << describeResidue(residue)
        << " behind — every failure path must remove the staging file";
}

// ───────────────────────────────────────────────────────────────────────
// F5 (a) — a /DSS write that cannot reach the disk must REPORT failure.
//
// Silence is the defect being excluded: LtvDssWriter::write() collapses every
// error to a bool at its boundary, so if that bool came back true the caller
// (ValidationCoordinator) would tell the user the document had been upgraded
// to LTV when nothing had been written at all.
// ───────────────────────────────────────────────────────────────────────
TEST_F(DssSaveFaultInjectionTest, UnwritableTargetMakesDssWriteReportFailure)
{
    SignatureInputs inputs;
    auto work = workCopy("dss_fault_report.pdf", inputs);
    ASSERT_FALSE(work.empty());
    ASSERT_FALSE(inputs.certChain.empty());

    auto doc = Document::open(ctx, work);
    ASSERT_NE(doc, nullptr);

    InPlaceWriteBlocker blocker(work);
    ASSERT_TRUE(blocker.armed())
        << "failed to make " << work.string()
        << " unwritable — the case would otherwise pass as a happy path";

    LtvDssWriter writer;
    EXPECT_FALSE(writer.write(doc.get(), inputs.contentsHex,
                              {inputs.certChain.front()}))
        << "a /DSS write whose incremental save cannot open the target must "
           "report failure, not swallow it";
}

// ───────────────────────────────────────────────────────────────────────
// F5 (b) — a save that cannot OPEN the target must not damage it.
//
// WHAT THIS MEASURES, AND WHAT IT DOES NOT (phase-49 review, finding L2).
// The name used to be FailedDssWriteLeavesTheSignedFileIntact, which claimed
// more than the injection can deliver. InPlaceWriteBlocker fails the save at
// the fopen() — see the "SAME LIMITATION" note in SaveFailureInjection.h,
// which is honest about this — so no bytes are ever appended and byte-identity
// is the trivially expected outcome, not a hard-won one. What this test really
// pins is that nothing on the ERROR path (rollback, cleanup, a retry through a
// different route) touches the file after the open fails.
//
// F58 (phase-52) UPDATE — WHERE THIS BLOCKER NOW BITES, AND WHY THE NAME IS
// STILL RIGHT. The save is staged now, so the read-only attribute no longer
// stops the open of the write target (the write target is the staging copy).
// It stops the COMMIT: MoveFileExW onto a read-only destination fails. The
// point of the test is unchanged and its claim is unchanged — a /DSS write
// that cannot complete must leave the signed document byte-identical — and it
// now covers strictly more of the path than it did, because the staging copy
// and the whole append really run before the failure. "BlockedAtOpen" is kept
// as the name because the INJECTED CONDITION is still "the target cannot be
// opened for writing"; what changed is which step first notices.
//
// The genuine MID-WRITE case — bytes of the new revision already committed to
// disk when the failure hits — is what F58 exists to make safe, and it is
// asserted by DssMidAppendKill below rather than here. Before F58 that case
// was unprotected AND unreachable by this harness; do not read THIS test as
// covering it.
// ───────────────────────────────────────────────────────────────────────
TEST_F(DssSaveFaultInjectionTest, DssWriteBlockedAtOpenLeavesTheSignedFileIntact)
{
    SignatureInputs inputs;
    auto work = workCopy("dss_fault_intact.pdf", inputs);
    ASSERT_FALSE(work.empty());
    ASSERT_FALSE(inputs.certChain.empty());

    const std::string before = readAllBytes(work);
    ASSERT_FALSE(before.empty());

    {
        auto doc = Document::open(ctx, work);
        ASSERT_NE(doc, nullptr);
        InPlaceWriteBlocker blocker(work);
        ASSERT_TRUE(blocker.armed());
        LtvDssWriter writer;
        EXPECT_FALSE(writer.write(doc.get(), inputs.contentsHex,
                                  {inputs.certChain.front()}));
    }

    EXPECT_TRUE(fileIsByteIdenticalTo(work, before))
        << "a /DSS write that could not even OPEN the target MUTATED the "
           "signed document, so something on the error path is writing to it. "
           "Because this save appends in place there is no staging file to "
           "fall back on, and any mutation here is damage to the user's only "
           "copy (F47/F5)";

    // ...and it must still be a document, not merely the same bytes. A file
    // that survives byte-identical but no longer opens would satisfy the
    // comparison above while still being data loss.
    auto reopened = Document::open(ctx, work);
    EXPECT_NE(reopened, nullptr)
        << "the signed document no longer opens after a failed /DSS write";
}

// ───────────────────────────────────────────────────────────────────────
// F5 (c) — THE POSITIVE CONTROL.
//
// Without this, both cases above pass trivially if /DSS writing is simply
// broken: "returns false" and "changed nothing" are exactly what a
// no-op would produce. Same document, same inputs, same call — the ONLY
// difference is that the obstruction is lifted first.
// ───────────────────────────────────────────────────────────────────────
TEST_F(DssSaveFaultInjectionTest, TheSameWriteSucceedsOnceTheObstructionLifts)
{
    SignatureInputs inputs;
    auto work = workCopy("dss_fault_control.pdf", inputs);
    ASSERT_FALSE(work.empty());
    ASSERT_FALSE(inputs.certChain.empty());

    const std::string before = readAllBytes(work);
    ASSERT_FALSE(before.empty());

    {
        auto doc = Document::open(ctx, work);
        ASSERT_NE(doc, nullptr);

        InPlaceWriteBlocker blocker(work);
        ASSERT_TRUE(blocker.armed());
        LtvDssWriter blocked;
        ASSERT_FALSE(blocked.write(doc.get(), inputs.contentsHex,
                                   {inputs.certChain.front()}));
        ASSERT_TRUE(fileIsByteIdenticalTo(work, before));
    }

    // Obstruction gone (the blocker released at scope exit). Reopen cleanly and
    // repeat the identical call.
    {
        auto doc = Document::open(ctx, work);
        ASSERT_NE(doc, nullptr);
        LtvDssWriter writer;
        ASSERT_TRUE(writer.write(doc.get(), inputs.contentsHex,
                                 {inputs.certChain.front()}))
            << "the /DSS write fails even with nothing obstructing it, so the "
               "failure cases above prove nothing";
    }

    EXPECT_FALSE(fileIsByteIdenticalTo(work, before))
        << "the unobstructed write reported success but changed no bytes";

    DssCounts counts{};
    auto vriKey = LtvDssWriter::sha1HexUpper(inputs.contentsHex);
    {
        auto doc = Document::open(ctx, work);
        ASSERT_NE(doc, nullptr);
        LtvDssWriter::inspectDss(doc.get(), vriKey, counts);
    }
    EXPECT_TRUE(counts.hasDss) << "no /DSS after a write that reported success";
    EXPECT_GE(counts.topCerts, 1);
}

// ───────────────────────────────────────────────────────────────────────
// F5 (d) — a failed /DSS write must not leave the mutation LOADED, waiting
// to be published by the next save the user performs.
//
// LtvDssWriter::write() mutates the in-memory pdf_document FIRST and saves
// SECOND. When the save fails it returns false — but the /DSS objects it
// added are still sitting in the open document. If that document is then
// saved for any unrelated reason, it publishes revocation material the caller
// was explicitly told had NOT been written. That is the "swallowed failure"
// half of F5: the error is reported, and then quietly happens anyway.
// ───────────────────────────────────────────────────────────────────────
TEST_F(DssSaveFaultInjectionTest, FailedDssWriteIsNotPublishedByALaterSave)
{
    SignatureInputs inputs;
    auto work = workCopy("dss_fault_deferred.pdf", inputs);
    ASSERT_FALSE(work.empty());
    ASSERT_FALSE(inputs.certChain.empty());

    auto vriKey = LtvDssWriter::sha1HexUpper(inputs.contentsHex);

    {
        auto doc = Document::open(ctx, work);
        ASSERT_NE(doc, nullptr);

        {
            InPlaceWriteBlocker blocker(work);
            ASSERT_TRUE(blocker.armed());
            LtvDssWriter writer;
            ASSERT_FALSE(writer.write(doc.get(), inputs.contentsHex,
                                      {inputs.certChain.front()}));
        }

        // The obstruction is gone. An unrelated save of the SAME open document
        // must not carry the rejected /DSS to disk.
        try {
            doc->saveIncremental();
        } catch (...) {
            // A refusal here is an acceptable outcome too — what must not
            // happen is a silent success that publishes the /DSS.
        }
    }

    DssCounts counts{};
    {
        auto doc = Document::open(ctx, work);
        ASSERT_NE(doc, nullptr);
        LtvDssWriter::inspectDss(doc.get(), vriKey, counts);
    }
    EXPECT_FALSE(counts.hasDss)
        << "a /DSS write that reported FAILURE was published to disk by the "
           "next save of the same open document. The caller was told the "
           "upgrade did not happen, and then it happened anyway (F47/F5)";
}

// ═══════════════════════════════════════════════════════════════════════
// F57 (phase-52) — THE ORIGINAL DEFECT SURVIVED INSIDE ITS OWN FIX.
//
// F5 (above) closed "a failed save leaves a silently mutated document" by
// adding DssRollback. rollbackDss() then handled two of the three states it
// can be in:
//
//     if (!rb.hadDss)     { delete the /DSS we added }
//     else if (rb.backup) { restore the saved copy   }
//     // <- and nothing at all for hadDss && !backup
//
// A document that DID have a /DSS whose pdf_deep_copy_obj() returned null took
// neither branch: the mutated /DSS stayed exactly where it was and the caller
// was told the rollback had succeeded. That is F5 verbatim, living inside the
// code written to close it — which is why every case below is about a document
// that ALREADY HAS a /DSS. The four F5 cases above all start from a document
// with none, so they can only ever reach the first branch, and the missing
// third state was invisible to all of them.
//
// WHY BOTH /DSS SHAPES. /Root/DSS may be a direct dictionary (which is what
// PDFE itself writes, via pdf_dict_puts_dict) or an INDIRECT reference (which
// is what Acrobat writes). They take structurally different restore routes —
// pdf_dict_puts() versus pdf_update_object() — and before F57 the indirect
// route had never been executed by any test, in either direction. Each case
// below asserts pdf_is_indirect() on the fixture rather than assuming it.
// ═══════════════════════════════════════════════════════════════════════

TEST_F(DssSaveFaultInjectionTest, PreExistingDirectDssSurvivesAFailedDssWrite)
{
    checkPreExistingDssSurvivesAFailedWrite(
        preDirect_, "dss_f57_direct_restore.pdf", /*expectIndirect=*/false);
}

TEST_F(DssSaveFaultInjectionTest, PreExistingIndirectDssSurvivesAFailedDssWrite)
{
    checkPreExistingDssSurvivesAFailedWrite(
        preIndirect_, "dss_f57_indirect_restore.pdf", /*expectIndirect=*/true);
}

TEST_F(DssSaveFaultInjectionTest,
       UncapturableDirectDssAbortsTheWriteInsteadOfMutatingIt)
{
    checkCaptureFailureAbortsBeforeAnyMutation(
        preDirect_, "dss_f57_direct_nobackup.pdf", /*expectIndirect=*/false);
}

TEST_F(DssSaveFaultInjectionTest,
       UncapturableIndirectDssAbortsTheWriteInsteadOfMutatingIt)
{
    checkCaptureFailureAbortsBeforeAnyMutation(
        preIndirect_, "dss_f57_indirect_nobackup.pdf", /*expectIndirect=*/true);
}

#if defined(_WIN32)
// ═══════════════════════════════════════════════════════════════════════
// FINDING 6 (phase-53 adversarial review) — A PUBLISHED /DSS REPORTED AS A
// PLAIN FAILURE.
//
// WHAT WAS WRONG. Document::saveIncrementalStaged() grew a second
// out-parameter, `publishedButReopenFailed` (F58-f), set in exactly one
// situation: the staged revision WAS renamed over the target — the file on
// disk is correct and complete — but re-opening it afterwards failed, so the
// in-memory Document is dead and the method throws. LtvDssWriter::write()
// passed only the first out-parameter and ignored this one, so it took the
// generic catch and reported "Failed to write /DSS dictionary".
//
// WHY THAT IS WORSE THAN A COSMETIC WRONG MESSAGE. The user is told the LTV
// upgrade failed about an upgrade that succeeded, and the obvious response —
// retry it — appends a SECOND, redundant /DSS revision to a document that
// already carries a correct one. The usual cause on Windows is benign and
// transient (an A/V scanner or the search indexer holding the just-renamed
// file for a moment), i.e. precisely the condition under which a user would
// retry and succeed at doing the wrong thing.
//
// HOW THE RE-OPEN IS MADE TO FAIL. A helper thread waits until a staging
// sibling has actually GROWN past the original's size — the copy is done and
// the append is under way — and only then applies a protected DACL denying this
// user read on the target. commitOver() carries that DACL onto the staging file
// (F58-b) and publishes it, so the bytes are on disk and correct, and
// fz_open_document() on them then fails with access denied. The flip is
// OBSERVED, and the test asserts it fired: a run where it never fired would
// otherwise be indistinguishable from a working guard.
//
// PHASE-10 CORRECTION — THIS IS A RACE, AND SAYING IT WAS NOT COST A ~25%
// FAILURE RATE. The text here read "made to fail DETERMINISTICALLY. Not by
// racing." Measured on clean master at fac0acb, this test failed 2 runs in 6
// (and 1 in 4 in a second sample) with `ok == true` — the write SUCCEEDING —
// while `flipped` was true. So the flip fires, and the claim that it fires in
// time does not hold: the usable window is [staging copy finished] →
// [commitOver reads the target's DACL to carry it across], and the /DSS append
// on these small fixtures is fast enough that the publish frequently wins.
// Waiting on "grew PAST baseline" cannot be moved earlier without breaking the
// copy, which is still reading the target at that point.
//
// THE FIX IS TO THE TIMING, NOT TO ANY ASSERTION. Every assertion below is
// unchanged and still runs, on an attempt that genuinely landed inside the
// window. The scenario is retried until the flip lands there, bounded, and
// exhausting the budget is a hard FAIL — never a skip, and never a pass. The
// two ways an attempt can miss are counted separately and reported, so a future
// regression that stops the flip firing at all is distinguishable from one that
// merely narrows the window. A test that measures a concurrency window has to
// hit that window; retrying the attempt is how, and it is the same remedy the
// flake register records elsewhere ("fix the timing, never the assertion").
//
// KILLS: dropping `&publishedButReopenFailed` from the
// saveIncrementalStaged() call; deleting the branch that consumes it;
// reporting it through the generic "write failed" path.
// ═══════════════════════════════════════════════════════════════════════
TEST_F(DssSaveFaultInjectionTest, PublishedDssWithAFailedReopenIsNotReportedAsAPlainFailure)
{
    const std::string me = pdfe::testing::currentUserSidString();
    if (me.empty()) GTEST_SKIP() << "could not resolve the current user's SID";

    // Generous relative to a measured ~1-in-4 miss rate: the chance of 16
    // consecutive misses when the window is working is ~2e-10, so exhausting
    // this budget means the mechanism is broken, not that the run was unlucky.
    constexpr int kMaxAttempts = 16;
    int flipNeverFired = 0;
    int publishWonTheRace = 0;

    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        SignatureInputs inputs;
        const std::string name =
            "dss_reopen_published_" + std::to_string(attempt) + ".pdf";
        auto work = workCopy(name.c_str(), inputs);
        ASSERT_FALSE(work.empty());
        ASSERT_FALSE(inputs.certChain.empty());

        const std::string before = readAllBytes(work);
        ASSERT_FALSE(before.empty());
        const auto baseline = static_cast<std::uintmax_t>(before.size());

        std::atomic<bool> stop{false};
        std::atomic<bool> flipped{false};
        std::thread flipper([&] {
            pdfe::testing::denyReadOnTargetOnceStagingGrows(work, baseline, me,
                                                            stop, flipped);
        });

        bool ok = true;
        std::string why;
        {
            auto doc = Document::open(ctx, work);
            ASSERT_NE(doc, nullptr);

            LtvDssWriter writer;
            ok = writer.write(doc.get(), inputs.contentsHex,
                              {inputs.certChain.front()});
            why = writer.lastError();

            stop.store(true, std::memory_order_relaxed);
            flipper.join();
        }
        pdfe::testing::restoreFullControl(work, me);

        // MISS (a): the observation never triggered at all. Not the window
        // being narrow — the injector doing nothing, which is the exact shape
        // DCA 0015 recorded against a blocker that armed and injected nothing.
        if (!flipped.load()) {
            ++flipNeverFired;
            continue;
        }
        // MISS (b): the flip fired, but after commitOver had already carried
        // the old DACL onto the staging file, so the published file stayed
        // readable and the re-open succeeded. Nothing was measured; retry.
        if (ok) {
            ++publishWonTheRace;
            continue;
        }

        // HIT — the re-open genuinely failed on a genuinely published file.
        // Everything from here is the actual subject of the test.

        // The Document is dead, so write() correctly reports false (already
        // established by reaching this branch)...
        //
        // ...but the REASON must say the data reached disk. This is the
        // assertion finding 6 is about, and the one that reddens if the
        // out-parameter is ignored again.
        EXPECT_NE(why.find(LtvDssWriter::kPublishedButReopenFailedMessage),
                  std::string::npos)
            << "the /DSS revision was published and the user is being told the "
               "upgrade failed. lastError() was: \"" << why << "\"";
        EXPECT_EQ(why.find(LtvDssWriter::kRollbackIncompleteMessage),
                  std::string::npos)
            << "a published revision must NOT be reported as an un-rollbackable "
               "mutation — nothing needed rolling back. lastError(): " << why;

        // And the claim in that message must be TRUE: the bytes really are the
        // original plus an appended revision. Asserting the message without
        // this would let a wrong message pass for a right one.
        const std::string after = readAllBytes(work);
        EXPECT_GT(after.size(), before.size())
            << "the message says the data WAS written; the file did not grow";
        EXPECT_EQ(after.compare(0, before.size(), before), 0)
            << "the original bytes are no longer a byte-identical PREFIX, so "
               "the existing signature's /ByteRange no longer resolves";

        auto reopened = Document::open(ctx, work);
        ASSERT_NE(reopened, nullptr)
            << "the published file does not open once permissions are "
               "restored, so 'the file on disk is correct and complete' is "
               "false";
        DssCounts counts;
        ASSERT_TRUE(LtvDssWriter::inspectDss(reopened.get(), "", counts));
        EXPECT_TRUE(counts.hasDss)
            << "the message claims the LTV data was written; there is no /DSS";
        EXPECT_GT(counts.topCerts, 0);

        // No staging residue of either shape survives a publication.
        EXPECT_TRUE(pdfe::testing::stagingResidueOf(work).empty());

        RecordProperty("attempts_used", attempt + 1);
        RecordProperty("miss_publish_won_race", publishWonTheRace);
        RecordProperty("miss_flip_never_fired", flipNeverFired);
        return;
    }

    FAIL() << "the published-but-unreadable window was never reached in "
           << kMaxAttempts << " attempts (flip never fired: " << flipNeverFired
           << ", publish won the race: " << publishWonTheRace
           << "). This is NOT a flake to re-run: at a working ~1-in-4 miss rate "
              "this outcome has probability ~2e-10. Either "
              "denyReadOnTargetOnceStagingGrows no longer observes the staging "
              "sibling (flip never fired), or the staged save no longer carries "
              "the target's DACL onto the staging file before publishing it "
              "(publish won the race) — F58-b. Both are real regressions in the "
              "mechanism this test depends on.";
}
#endif  // _WIN32
