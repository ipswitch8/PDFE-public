// -*- coding: utf-8 -*-
// ===========================================================================
// Meta-audit follow-up F58 — phase-52.
//
// THE DISCRIMINATING CONTROL FOR "THE USER'S ONLY COPY IS THE WRITE TARGET".
//
// WHAT WAS WRONG
// --------------
// LtvDssWriter::write() committed by calling Document::saveIncremental(), i.e.
// pdf_save_document(do_incremental=1) in APPEND mode straight onto the signed
// document the user already has. A genuine mid-write failure — disk full,
// media pulled, power loss, an I/O error — therefore left that document
// carrying a partial appended revision: a truncated trailing xref, a
// startxref pointing at nothing, and a /ByteRange that no longer resolves.
// The signature is destroyed by the operation whose entire purpose was to make
// it verifiable for longer. DssRollback (F5/F57) cannot reach it: that undoes
// in-memory OBJECT mutations, not bytes already on disk.
//
// WHY NOTHING HAD EVER MEASURED IT
// --------------------------------
// Every injector in tests/support/SaveFailureInjection.h fails a save at an
// OPEN boundary — the header says so about itself. A failure at the open has
// written zero bytes, so "the original is unchanged" is the trivially expected
// outcome and holds just as well for the broken in-place design as for a safe
// staged one. It discriminates nothing. And the two temp-path injectors
// (TempWriteBlocker, RenameLock) poison "<target>.pdfe_tmp", a path the
// in-place append never created, so they ARMED SUCCESSFULLY AND INJECTED
// NOTHING — armed() cannot tell that from working. That is the harness
// blindness DCA 0015 recorded, and it is why this file uses a different
// injector rather than another blocker.
//
// THE INJECTOR
// ------------
// pdfe::testing::startAppendGrowthWatcher + killChildInsideSaveWindow: a child
// process really killed with TerminateProcess at the moment the append has
// committed bytes past the target's pre-save size — to the target itself (the
// pre-F58 in-place shape) or to the staging copy (the post-F58 shape). One
// watcher, both code paths, no branch on which one is compiled in, which is
// what makes the revert-and-observe comparison apples-to-apples.
//
// It is a literal power loss: no unwinding, no destructors, no CRT flush. The
// one thing it is not is a literal ENOSPC, for the reason the harness header
// already records — a volume that genuinely fills up needs administrative
// privilege, which a unit-test process must not assume.
//
// THE ORACLE
// ----------
// After the kill, the signed document at `target` must be BYTE-IDENTICAL to
// its pre-save snapshot and must still open. Against the staged
// implementation it is, because the partial bytes went into a staging file
// that is simply abandoned. Against the pre-F58 direct append it is not: the
// partial revision is on the end of the user's file.
// ===========================================================================

#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "signatures/LtvDssWriter.h"
#include "signatures/SignatureManager.h"
#include "support/SaveFailureInjection.h"

#include <QRectF>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#ifndef TEST_FIXTURES_DIR
#define TEST_FIXTURES_DIR "."
#endif

using pdfe::core::Document;
using pdfe::core::MuPdfContext;
using pdfe::signatures::LtvDssWriter;
using pdfe::signatures::SignatureInputs;
using pdfe::signatures::SigningOptions;
using pdfe::signatures::SignatureManager;
using pdfe::testing::KilledSaveResult;
using pdfe::testing::readAllBytes;
// NOT saveTempPathFor — deliberately (DCA 0015, phase-10). Every save in this
// file is the STAGED incremental /DSS write, which produces
// "<target>.pdfe_part.<hex>" and can never produce the fixed "<target>.pdfe_tmp"
// that helper returns. Importing it here is what let a residue assertion be
// written against a filename nothing in this file creates. Use
// pdfe::testing::stagingResidueOf(), which covers both shapes.

namespace {

// HOW WIDE THE WINDOW IS, AND WHY IT IS SIZED THE WAY IT IS.
//
// The kill has to land between "the first byte of the revision reached the
// disk" and "the append finished". That interval is proportional to how much
// data the /DSS write appends, and the round trip from the child's watcher
// firing to TerminateProcess returning is on the order of a couple of
// milliseconds (child polls at 100 us, parent at 1 ms). A default-sized /DSS —
// one ~1 KB certificate — would be written in well under a millisecond, and
// the kill would routinely arrive after the save had already finished, which
// would make this test a coin flip.
//
// So the payload is deliberately huge. /DSS certificate entries are written to
// the file VERBATIM (LtvDssWriter never parses them: `certs` is a vector of
// opaque byte blobs handed to pdf_add_stream), so padding each blob is a
// legitimate way to buy a wide window without changing the code path by one
// branch. kBlobBytes * kBlobCount * 2 bytes are appended — twice, because
// writeDssDict emits each cert both under the top-level /Certs and under
// /VRI/<key>/Cert.
//
// If this test ever reports "the child finished before the kill landed",
// raising kBlobCount is the correct response. Loosening the oracle is not.
constexpr std::size_t kBlobBytes = 2u * 1024u * 1024u;
constexpr std::size_t kBlobCount = 48;

std::filesystem::path fixtures()
{
    return std::filesystem::path(TEST_FIXTURES_DIR);
}

/// The oversized cert list both the victim and the positive control write, so
/// they exercise byte-for-byte the same production call with the same payload.
std::vector<std::vector<uint8_t>> inflatedCerts(const std::vector<uint8_t>& seed)
{
    std::vector<std::vector<uint8_t>> out;
    out.reserve(kBlobCount);
    for (std::size_t i = 0; i < kBlobCount; ++i) {
        std::vector<uint8_t> blob;
        blob.reserve(kBlobBytes);
        while (blob.size() < kBlobBytes) {
            const std::size_t take =
                std::min(seed.size(), kBlobBytes - blob.size());
            blob.insert(blob.end(), seed.begin(), seed.begin() + take);
        }
        // Make every blob distinct so nothing downstream can collapse them.
        blob[0] = static_cast<uint8_t>(i & 0xFF);
        out.push_back(std::move(blob));
    }
    return out;
}

/// Sign `sourceName` from the fixture directory into `outPath`. Same flow as
/// tests/test_dss_save_fault_injection.cpp's helper.
bool buildSignedFixture(const std::shared_ptr<MuPdfContext>& ctx,
                        const std::filesystem::path& outPath)
{
    const auto pfx = fixtures() / "test_signer.pfx";
    const auto srcPdf = fixtures() / "test_single.pdf";
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
            mgr.createSignatureField(page.get(), "F58MidAppendSig",
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
            opts.reason = "F58 mid-append kill";
            mgr.signField(doc.get(), page.get(), 0, opts);
        }
    } catch (...) {
        return false;
    }
    return std::filesystem::exists(outPath);
}

}  // namespace

// ---------------------------------------------------------------------------
// THE VICTIM.
//
// In the parent process this is a no-op that passes immediately (the env var is
// unset), so it costs nothing in a normal suite run and is never DISABLED_ or
// skipped. In the child — launched with PDFE_F10_SAVE_VICTIM set — it opens the
// already-signed target, starts the growth watcher with the target's pre-save
// size as the baseline, and performs ONE real /DSS write with an oversized
// payload. The parent kills it partway through the append; this test is not
// expected to complete there.
// ---------------------------------------------------------------------------
TEST(DssMidAppendVictim, WritesDssUntilKilled)
{
    if (!pdfe::testing::runningAsSaveVictim()) {
        // Parent process: nothing to do. Asserting the harness agrees keeps
        // this from being an empty always-green test.
        EXPECT_TRUE(pdfe::testing::saveVictimTarget().empty());
        return;
    }

    const auto target = pdfe::testing::saveVictimTarget();
    ASSERT_FALSE(target.empty());

    std::error_code ec;
    const std::uintmax_t baseline = std::filesystem::file_size(target, ec);
    ASSERT_FALSE(ec);
    ASSERT_GT(baseline, 0u);

    auto ctx = std::make_shared<MuPdfContext>();
    auto doc = Document::open(ctx, target);
    ASSERT_NE(doc, nullptr);

    SignatureInputs inputs;
    ASSERT_TRUE(LtvDssWriter::extractFirstSignatureInputs(doc.get(), inputs));
    ASSERT_FALSE(inputs.certChain.empty());

    const auto certs = inflatedCerts(inputs.certChain.front());

    // Announce the instant the revision starts hitting the disk — whether that
    // is the target (in-place append) or the staging copy (staged append).
    pdfe::testing::startAppendGrowthWatcher(target, baseline);

    LtvDssWriter writer;
    // The return value is irrelevant: the parent's oracle is the on-disk state
    // after the kill, and reaching this line at all means the kill was late.
    (void)writer.write(doc.get(), inputs.contentsHex, certs);
}

#if defined(_WIN32)

// ---------------------------------------------------------------------------
// THE CONTROL ITSELF.
// ---------------------------------------------------------------------------
TEST(DssMidAppendKill, KillMidAppendLeavesTheSignedDocumentByteIdentical)
{
    std::error_code ec;
    const auto dir =
        std::filesystem::temp_directory_path()
        / ("pdfe_f58_midappend_"
           + std::to_string(
               std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(dir, ec);
    ASSERT_FALSE(ec);

    const auto target = dir / "f58_signed.pdf";
    {
        auto ctx = std::make_shared<MuPdfContext>();
        ASSERT_TRUE(buildSignedFixture(ctx, target))
            << "on-the-fly signing failed; check test_signer.pfx and the "
               "PKCS#7 bridge";
    }

    const std::string before = readAllBytes(target);
    ASSERT_FALSE(before.empty());

    const KilledSaveResult r = pdfe::testing::killChildInsideSaveWindow(
        target, target, "DssMidAppendVictim.WritesDssUntilKilled");

    ASSERT_TRUE(r.childLaunched)
        << "could not spawn the victim child: " << r.diagnostic;
    // PROOF THE INJECTOR FIRED, not merely armed. childReachedWindow is set
    // only when the child's marker file actually appeared, and the child only
    // writes that marker after OBSERVING the on-disk size grow past the
    // pre-save baseline. It is not a claim about intent — it is a filesystem
    // observation of committed bytes, so it cannot report a fault that did not
    // happen. This is the exact property TempWriteBlocker's armed() could not
    // give on this path.
    ASSERT_TRUE(r.childReachedWindow)
        << "the child never got bytes of the /DSS revision onto disk, so "
           "NOTHING was injected and a green result here would prove nothing: "
        << r.diagnostic;
    ASSERT_TRUE(r.childKilled) << r.diagnostic;

    // ── DID THE KILL ACTUALLY INTERRUPT ANYTHING? (GitHub #291) ─────────
    //
    // `childKilled` is TerminateProcess's return value and says NOTHING about
    // timing -- that call succeeds on a child which has already exited. Once,
    // this test reported "the signature this operation existed to strengthen
    // has been destroyed" on a run where the child had simply finished its
    // /DSS write first: a grown file was the EXPECTED result of a completed
    // append, and the oracle read it as data loss.
    //
    // So classify before asserting. A late kill injected nothing, which makes
    // the on-disk state no evidence at all about interruption -- it may not be
    // promoted to a pass OR to a data-loss claim.
    const std::string after = readAllBytes(target);
    const bool sizeChanged = after.size() != before.size();
    const auto verdict = pdfe::testing::classifyMidAppendKill(
        r.exitCode, sizeChanged, after == before);

    ASSERT_NE(verdict, pdfe::testing::MidAppendVerdict::ChildDiedUnexpectedly)
        << "the victim died of something that was neither our kill nor a clean "
           "exit (code " << r.exitCode << "). That is a broken victim, not a "
           "statement about F58: " << r.diagnostic;

    ASSERT_NE(verdict, pdfe::testing::MidAppendVerdict::KillLandedLate)
        << "THE KILL LANDED LATE and this run proves nothing (GitHub #291).\n"
           "The child finished its /DSS write before TerminateProcess arrived "
           "(exit code " << r.exitCode << ", not "
        << pdfe::testing::kKilledWhileRunningExitCode << "), so nothing was "
           "injected mid-append.\n"
           "The file is "
        << (sizeChanged ? "larger, which is what a COMPLETED append looks like"
                        : "unchanged")
        << " -- in neither case is it evidence about what an interrupted "
           "append does.\n"
           "This is a lost race in the harness, NOT a damaged signature. "
           "F58's mid-append point is a moving window, so the 1 ms poll can "
           "miss it; re-run. Do not read this as data loss, and do not read a "
           "pass here as proof that interruption is safe.\n"
        << r.diagnostic;

    // ── THE ORACLE ──────────────────────────────────────────────────────
    // Reached only when the kill demonstrably interrupted a running append.
    ASSERT_TRUE(std::filesystem::exists(target))
        << "the kill left the signed document GONE";

    EXPECT_EQ(after.size(), before.size())
        << "the signed document CHANGED SIZE across a /DSS write that was "
           "killed mid-append: the append was landing on the user's only copy "
           "(F58). A staged append cannot do this — the partial bytes go into "
           "the staging file (\"<target>.pdfe_part.<CSPRNG hex>\" since F58-a; "
           "\"<target>.pdfe_tmp\" in the phase-52 shape) and are abandoned.";
    EXPECT_TRUE(after == before)
        << "the signed document is no longer byte-identical after a /DSS write "
           "killed mid-append. Its /ByteRange no longer covers the file it is "
           "in, i.e. the signature this operation existed to strengthen has "
           "been destroyed (F58).";

    // Byte-identity alone is not enough: it must still be a document.
    {
        auto ctx = std::make_shared<MuPdfContext>();
        auto reopened = Document::open(ctx, target);
        ASSERT_NE(reopened, nullptr)
            << "the signed document no longer opens after the killed /DSS "
               "write";
        EXPECT_GT(reopened->pageCount(), 0);
    }

    // A staging orphan is the EXPECTED residue of a killed staged save — it is
    // the R2-04 crash-recovery artifact, surfaced rather than applied — so its
    // presence is not a failure. It is only cleaned up here so the temp dir
    // goes away.
    std::filesystem::remove_all(dir, ec);
}

// ---------------------------------------------------------------------------
// THE POSITIVE CONTROL — the injector's other half.
//
// Without this, the assertion above passes trivially if the /DSS write with
// this payload is simply broken and never writes anything: "byte-identical
// afterwards" is exactly what a no-op produces. Same document, same call, same
// oversized payload, one difference — nobody kills it. The file must change,
// the original bytes must survive as a PREFIX (this is an incremental append,
// not a rewrite), and no staging residue may remain.
// ---------------------------------------------------------------------------
TEST(DssMidAppendKill, UnkilledTheSameWriteSucceedsAndAppends)
{
    std::error_code ec;
    const auto dir =
        std::filesystem::temp_directory_path()
        / ("pdfe_f58_nokill_"
           + std::to_string(
               std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(dir, ec);
    ASSERT_FALSE(ec);

    const auto target = dir / "f58_signed_nokill.pdf";
    auto ctx = std::make_shared<MuPdfContext>();
    ASSERT_TRUE(buildSignedFixture(ctx, target));

    const std::string before = readAllBytes(target);
    ASSERT_FALSE(before.empty());

    // DCA 0015 / phase-10 — the same wrong-shape residue check this file's
    // sibling carried, found by sweeping for it after fixing that one. See the
    // assertion at the end of this test for what was wrong and why the byte
    // prefix above it does not cover the same ground.
    const auto idBefore = pdfe::testing::fileIdentityOf(target);
    ASSERT_TRUE(idBefore.valid)
        << "could not read the target's file identity before the save";

    SignatureInputs inputs;
    {
        auto doc = Document::open(ctx, target);
        ASSERT_NE(doc, nullptr);
        ASSERT_TRUE(
            LtvDssWriter::extractFirstSignatureInputs(doc.get(), inputs));
        ASSERT_FALSE(inputs.certChain.empty());

        LtvDssWriter writer;
        ASSERT_TRUE(writer.write(doc.get(), inputs.contentsHex,
                                 inflatedCerts(inputs.certChain.front())))
            << "the identical write fails with nothing injected, so the kill "
               "case proves nothing: " << writer.lastError();
    }

    const std::string after = readAllBytes(target);
    ASSERT_GT(after.size(), before.size())
        << "the unobstructed write reported success but appended nothing";
    EXPECT_EQ(after.compare(0, before.size(), before), 0)
        << "the staged incremental save did not preserve the original bytes as "
           "a byte-identical prefix — every existing /ByteRange is now wrong";
    // DCA 0015 / phase-10 — THIS ASSERTION USED TO NAME THE WRONG FILE, and
    // its own message says why that mattered: it claims something about a
    // STAGED save, and saveTempPathFor() returns the fixed "<target>.pdfe_tmp"
    // of the FULL-REWRITE branch, which the staged /DSS path cannot create. It
    // therefore asserted the absence of a file nothing here produces. Found by
    // sweeping for the shape after fixing the identical defect in
    // test_dss_save_fault_injection.cpp — one file over, which is where this
    // class keeps turning up.
    const auto residue = pdfe::testing::stagingResidueOf(target);
    EXPECT_TRUE(residue.empty())
        << "an uninterrupted staged /DSS save must leave no staging residue; "
           "found " << residue.size() << " sibling(s), first: "
        << (residue.empty() ? std::string("<none>") : residue.front().string());

    // ...and it really did STAGE. "No residue" is satisfied both by "staged and
    // cleaned up" and by "never staged", so on its own it cannot establish the
    // mechanism this test's header claims. The byte-prefix check above does not
    // separate them either — an in-place incremental append preserves the
    // prefix too. A rename changes the target's file identity; an in-place
    // append preserves it.
    const auto idAfter = pdfe::testing::fileIdentityOf(target);
    ASSERT_TRUE(idAfter.valid) << "the target vanished across the save";
    EXPECT_NE(idAfter, idBefore)
        << "the /DSS write did not publish by rename — it appended straight "
           "onto the signed file, which is the pre-F58 shape this whole file "
           "exists to prove has gone";

    // ...and the /DSS really is there.
    {
        auto doc = Document::open(ctx, target);
        ASSERT_NE(doc, nullptr);
        pdfe::signatures::DssCounts counts{};
        const auto vriKey = LtvDssWriter::sha1HexUpper(inputs.contentsHex);
        EXPECT_TRUE(LtvDssWriter::inspectDss(doc.get(), vriKey, counts));
        EXPECT_EQ(counts.topCerts, static_cast<int>(kBlobCount));
        EXPECT_TRUE(counts.hasVriKey);
    }

    std::filesystem::remove_all(dir, ec);
}

#endif  // _WIN32
