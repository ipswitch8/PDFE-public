// -*- coding: utf-8 -*-
//
// WIN-87 phase-59 — hostile-filesystem cases for the two COMPLIANCE CONVERTERS
// (PdfAConverter::convertAndSave, PdfUAConverter::convertAndSave), plus the
// R2-04 pin that their migrated residue is never offered to a user as a
// recoverable document.
//
// WHAT WAS ACTUALLY WRONG HERE, WHICH IS NOT WHAT THE PHASE WAS SCOPED TO FIND.
// Both converters ran the site-#3 relay verbatim — MuPDF wrote
// "<outputPath>.pdfe_pdfa_tmp" (resp. ".pdfe_pdfua_tmp") BY NAME, nothing held
// it, and QPDF REOPENED IT BY NAME. That is the WIN-87 substitution class: an
// attacker who knows where you are converting knows that name, and bytes
// stamped into it between the bake finishing and QPDF opening it are what gets
// republished as your "PDF/A" output.
//
// AND THERE WAS A SECOND, INDEPENDENT DEFECT the class sweep does not name:
// QPDFWriter then wrote DIRECTLY OVER outputPath. There was no atomic publish
// at any point, so a failure part-way through that write left the user's output
// TRUNCATED — the R2-04 data-loss shape that every other publish path in this
// product had already had removed. Both converters now go through
// Document::saveAsThroughExternalRewrite: bake into a StagingFile behind an
// exclusive handle at an unpredictable name, hand QPDF DUPLICATES of that
// handle and of a second staging handle, and publish only by renaming the
// output staging file through its own handle.
//
// WHY A FREE-RUNNING ENUMERATING ATTACKER IS THE RIGHT SHAPE, as in phase-58
// and unlike phases 56/57: the artifacts under attack are PDFE's own staging
// files in a directory PDFE controls, so what an enumerating attacker can
// observe IS the property. At sites #4/#5 the artifact was the user's own
// document, whose name cannot be made unpredictable, and a racing thread hit
// the window only by luck — MEASURED it did not, which is why those sites
// needed deterministic seams instead.
//
// EVERY CASE HAS A VACUITY GUARD (`saw > 0`). A green run in which the attacker
// observed nothing is equally explained by a conversion that never staged
// anything, and that is evidence about nothing.

#include <gtest/gtest.h>

#include "compliance/ComplianceTypes.h"
#include "compliance/PdfAConverter.h"
#include "compliance/PdfUAConverter.h"
#include "core/AtomicFileReplace.h"
#include "support/DirectoryAttacker.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"

#include <atomic>
#include <chrono>  // the beginOperation() rendezvous below is deadline-bounded
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#ifndef TEST_FIXTURES_DIR
#define TEST_FIXTURES_DIR "."
#endif

namespace {

namespace fs = std::filesystem;

constexpr const char* kPayload = "PWNED-WIN87-PHASE59-0123456789ABCDEF";

fs::path fixtures() { return fs::path(TEST_FIXTURES_DIR); }

fs::path makeWorkDir(const std::string& tag)
{
    static std::atomic<int> counter{0};
    auto d = fs::temp_directory_path()
           / ("pdfe_win87_p59_" + tag + "_"
              + std::to_string(::GetCurrentProcessId()) + "_"
              + std::to_string(counter.fetch_add(1)));
    std::error_code ec;
    fs::remove_all(d, ec);
    fs::create_directories(d, ec);
    return d;
}

std::string readAll(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)),
                       std::istreambuf_iterator<char>());
}

/// True when `p` can be opened with DELETE access — i.e. nothing is protecting
/// it. The control: if the attacker's technique does not work on a file nothing
/// is guarding, a clean result above it proves nothing.
bool canTakeDeleteAccess(const fs::path& p)
{
    HANDLE h = ::CreateFileW(p.native().c_str(), DELETE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                             nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    ::CloseHandle(h);
    return true;
}

fs::path makeDecoy(const fs::path& dir)
{
    const fs::path p = dir / "decoy_source.bin";
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    for (int i = 0; i < 64; ++i) f << kPayload;
    return p;
}

// DirectoryAttacker now lives in tests/support/DirectoryAttacker.h (GitHub #275).
//
// It was defined HERE and in test_publish_handle_hardening_phase58.cpp, and the two copies
// had drifted four ways by the time they were merged -- including the attack
// PAYLOAD, which is why the shared version takes it as a parameter. The cost was
// measured: a poll-race fix applied to one copy left the other broken, all 20
// TOCTTOU cases passed, and a stability run under load then failed 9 of 12 in the
// copy nobody had touched. See the header for the full account.
using pdfe::testing::AttackPayload;
using pdfe::testing::DirectoryAttacker;

/// Every artifact the attacker saw, other than the finished output, must be an
/// UNPREDICTABLE staging residue — a "fix" that merely swapped
/// ".pdfe_pdfa_tmp" for another guessable suffix fails here.
///
/// The target is everything before kSuffixMarker. fs::path::stem() is WRONG for
/// this and measured wrong in phase-58: for "out.pdf.pdfe_part.<hex>" it
/// returns "out.pdf.pdfe_part", the predicate then rejects a perfectly good
/// name, and a red over working code is the same species of defect as a green
/// over broken code.
void expectOnlyUnpredictableResidue(const std::set<std::string>& names,
                                    const std::string& outputName)
{
    for (const auto& name : names) {
        if (name == outputName) continue;
        if (name == "control_unprotected.bin") continue;
        const std::string marker = pdfe::core::StagingFile::kSuffixMarker;
        const auto at = name.rfind(marker);
        ASSERT_NE(at, std::string::npos)
            << "the conversion staged under a name that is not an unpredictable "
               "staging residue at all: " << name;
        const std::string target = name.substr(0, at);
        EXPECT_TRUE(pdfe::core::StagingFile::isStagingResidueName(target, name))
            << "the conversion staged under a predictable name: " << name;
        // The two names this phase deleted, pinned explicitly: they were cited
        // by name in docs/audit/meta-audit-2026-07/followup/
        // phase-26-dispositions.md, so a revert would be silent otherwise.
        EXPECT_EQ(name.find(".pdfe_pdfa_tmp"), std::string::npos) << name;
        EXPECT_EQ(name.find(".pdfe_pdfua_tmp"), std::string::npos) << name;
    }
}

class ComplianceConverterPublishTocttou : public ::testing::Test {
protected:
    void SetUp() override
    {
        const auto src = fixtures() / "test_single.pdf";
        if (!fs::exists(src)) GTEST_SKIP() << "test_single.pdf missing";
        dir_ = makeWorkDir("conv");
        outDir_ = dir_ / "out";
        fs::create_directories(outDir_);
        decoy_ = makeDecoy(dir_);
        ctx_ = std::make_shared<pdfe::core::MuPdfContext>();
        doc_ = pdfe::core::Document::open(ctx_, src);
        ASSERT_TRUE(doc_);
    }
    void TearDown() override
    {
        doc_.reset();
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    /// Shared assertions for a completed conversion under attack. The output
    /// name is skipped by the attacker: PDFE does not claim to stop a process
    /// that already has write access to your output directory from scribbling
    /// on a FINISHED file, and without the exemption the case fails on "the
    /// published file is not a PDF" — which it would not be, because the test
    /// had just overwritten it.
    void assertPublishSurvived(DirectoryAttacker& attacker,
                               const fs::path& output)
    {
        EXPECT_GT(attacker.saw.load(), 0)
            << "the attacker observed no artifact at all during a conversion "
               "that definitely stages two. Seen: [" << attacker.seenNames()
            << "]";

        expectOnlyUnpredictableResidue(attacker.observed(),
                                       output.filename().string());

        // Exclusivity IS achievable at these sites — unlike the batch chain,
        // whose producers open their own output by name — so it is asserted,
        // not merely diagnosed.
        EXPECT_EQ(attacker.openedForWrite.load(), 0)
            << "a staging artifact was writable across the publish window. "
               "Wins: [" << attacker.wins << "]";
        EXPECT_EQ(attacker.substituted.load(), 0)
            << "a decoy was substituted over a staging artifact. Wins: ["
            << attacker.wins << "]";
        EXPECT_EQ(attacker.deleted.load(), 0)
            << "a staging artifact was deletable across the publish window. "
               "Wins: [" << attacker.wins << "]";

        ASSERT_TRUE(fs::exists(output))
            << "the conversion published nothing, so nothing above is evidence "
               "about a publish";
        const auto body = readAll(output);
        EXPECT_EQ(body.find(kPayload), std::string::npos)
            << "ATTACKER BYTES WERE PUBLISHED AS THE CONVERTED DOCUMENT";
        ASSERT_GE(body.size(), 5u);
        EXPECT_EQ(body.compare(0, 5, "%PDF-"), 0)
            << "the published conversion output is not a PDF";

        // No residue of ANY name survives a completed conversion.
        std::set<std::string> left;
        std::error_code ec;
        for (fs::directory_iterator it(outDir_, ec), end; !ec && it != end;
             it.increment(ec)) {
            if (it->path().filename() == output.filename()) continue;
            left.insert(it->path().filename().string());
        }
        EXPECT_TRUE(left.empty())
            << "conversion residue survived in the output directory";

        // CONTROL: the technique must work on an unprotected file in the same
        // directory, or none of the zeroes above are evidence.
        const fs::path control = outDir_ / "control_unprotected.bin";
        { std::ofstream f(control, std::ios::binary); f << "CONTROL"; }
        EXPECT_TRUE(canTakeDeleteAccess(control))
            << "the attacker's technique does not work even unprotected here";
    }

    fs::path dir_, outDir_, decoy_;
    std::shared_ptr<pdfe::core::MuPdfContext> ctx_;
    std::unique_ptr<pdfe::core::Document> doc_;
};

TEST_F(ComplianceConverterPublishTocttou,
       PoisoningAnyStagedPdfAArtifactCannotSubstituteTheConvertedDocument)
{
    const fs::path output = outDir_ / "converted_a.pdf";

    DirectoryAttacker attacker;
    attacker.start(outDir_, decoy_, /*skip=*/{output.filename().string()},
                   AttackPayload(kPayload));

    pdfe::compliance::PdfAConverter converter(ctx_);
    attacker.beginOperation();
    auto result = converter.convertAndSave(
        doc_.get(), pdfe::compliance::PdfALevel::PdfA_2b, output);
    attacker.endOperation();
    attacker.join();

    // A refusal under attack is an acceptable outcome — WIN-87 trades
    // availability for integrity — but it must be diagnosable, and the
    // published bytes must never be the attacker's either way.
    if (!result.success && !result.errorMessage.empty()) {
        RecordProperty("conversion_error", result.errorMessage);
    }

    assertPublishSurvived(attacker, output);
}

TEST_F(ComplianceConverterPublishTocttou,
       PoisoningAnyStagedPdfUAArtifactCannotSubstituteTheConvertedDocument)
{
    const fs::path output = outDir_ / "converted_ua.pdf";

    DirectoryAttacker attacker;
    attacker.start(outDir_, decoy_, /*skip=*/{output.filename().string()},
                   AttackPayload(kPayload));

    pdfe::compliance::PdfUAConverter converter(ctx_);
    attacker.beginOperation();
    auto result = converter.convertAndSave(doc_.get(), output, "en-US");
    attacker.endOperation();
    attacker.join();

    if (!result.success && !result.errorMessage.empty()) {
        RecordProperty("conversion_error", result.errorMessage);
    }

    assertPublishSurvived(attacker, output);
}

// ===========================================================================
// R2-04 pin — migrated residue must never be offered as a recoverable document
// ===========================================================================
//
// F58-d's contract is that ".pdfe_tmp" means "a COMPLETE, finished save
// awaiting rename" and IS offered to a user for recovery, while
// ".pdfe_part.<hex>" means internal, possibly-truncated residue that is SWEPT
// and never surfaced. The converters' old temps ('.pdfe_pdfa_tmp',
// '.pdfe_pdfua_tmp') were neither: not recognised by the recovery scan, and not
// swept by anything — unrecognised orphans. Their replacements are
// StagingFile residues, so they fall on the swept side.
//
// Document.cpp builds an EXACT "<filePath>.pdfe_tmp" for the recovery scan, so
// this cannot regress today. It is pinned because a future refactor to
// SUFFIX-MATCHING would silently start offering possibly-truncated converter
// residue to users as "your unsaved changes" — which is precisely the trap
// F58-d closed, and it would be invisible without this case.
TEST(ComplianceConverterResidueRecovery,
     StagingResidueIsSweptAndNeverOfferedAsARecoverableSave)
{
    const auto src = fixtures() / "test_single.pdf";
    if (!fs::exists(src)) GTEST_SKIP() << "test_single.pdf missing";

    const auto dir = makeWorkDir("r204");
    const fs::path target = dir / "doc.pdf";
    fs::copy_file(src, target);

    // A residue of exactly the shape the migrated converters leave if killed:
    // "<target filename>.pdfe_part.<32 hex>".
    const std::string residueName =
        target.filename().string()
        + std::string(pdfe::core::StagingFile::kSuffixMarker)
        + "0123456789abcdef0123456789abcdef";
    const fs::path residue = dir / residueName;
    { std::ofstream f(residue, std::ios::binary); f << "%PDF-1.7\nnot a save\n"; }
    ASSERT_TRUE(pdfe::core::StagingFile::isStagingResidueName(
        target.filename(), residue.filename()))
        << "this case is not exercising the residue shape it claims to";

    auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
    auto doc = pdfe::core::Document::open(ctx, target);
    ASSERT_TRUE(doc);

    EXPECT_FALSE(doc->pendingRecoveredTemp().has_value())
        << "a '.pdfe_part.<hex>' residue was offered to the user as a "
           "recoverable save; only the exact '<filePath>.pdfe_tmp' full-rewrite "
           "temp may ever be (F58-d)";
    EXPECT_FALSE(fs::exists(residue))
        << "the residue was neither offered nor swept — an unrecognised orphan, "
           "which is the state the converters' old temps were in";

    // VACUITY GUARD FOR THE PREDICATE ITSELF: the scan must still surface a
    // genuine, complete full-rewrite temp, or the expectations above would be
    // satisfied by a recovery scan that had simply stopped working.
    doc.reset();
    const fs::path completeTemp = dir / (target.filename().string() + ".pdfe_tmp");
    {
        auto body = readAll(target);
        std::ofstream f(completeTemp, std::ios::binary | std::ios::trunc);
        f.write(body.data(), static_cast<std::streamsize>(body.size()));
        // Differ in size from the original AND stay structurally finished: the
        // trailing "%%EOF" is what orphanTempLooksComplete() looks for, so
        // appending a comment after it would make this an INCOMPLETE orphan and
        // the guard would then be measuring the wrong thing.
        f << "% phase-59 makes this differ in size from the original\n%%EOF\n";
    }
    auto doc2 = pdfe::core::Document::open(ctx, target);
    ASSERT_TRUE(doc2);
    EXPECT_TRUE(doc2->pendingRecoveredTemp().has_value())
        << "the recovery scan no longer surfaces a genuine full-rewrite temp, "
           "so the negative assertions above prove nothing";

    doc2.reset();
    std::error_code ec;
    fs::remove_all(dir, ec);
}

}  // namespace

#endif  // _WIN32
