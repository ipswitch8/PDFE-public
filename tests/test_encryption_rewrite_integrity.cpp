// -*- coding: utf-8 -*-
// Meta-audit 2026-07 follow-up, phase F26 — item 1.
//
// WHAT WAS WRONG
// --------------
// EncryptionManager::removeEncryption writes with do_garbage + do_compress: a
// FULL REWRITE that rebuilds the file from the object graph, discarding the
// incremental revision chain and moving every byte offset in it. Pointed at a
// signed or certified document it silently produced a file that no validating
// reader will accept — the signature dictionaries survive the copy intact,
// their /ByteRange arrays no longer describe the file they sit in, and nothing
// anywhere said so.
//
// It is deliberately EXEMPT from the DocMDP direct-write guard F24 installed,
// on an argued rule: this is a security-property change the user explicitly
// requested, producing a new artifact at a named output path. Refusing it would
// break the entirely lawful "strip the password so I can archive this"
// workflow.
//
// SO THE REQUIRED OUTCOME IS A WARNING, NEVER A REFUSAL. Getting that backwards
// would be a worse defect than the one being fixed, and these tests assert the
// direction explicitly: every signed case below also asserts the output file
// was written and reopens.
//
// WHAT THE SECURITY-AUDIT GATE FOUND (F26 remediation)
// ----------------------------------------------------
// The first cut of the probe walked ONLY /Root /AcroForm /Fields. That is
// narrower than every other signature-enumeration path in this codebase —
// SignatureManager lists via pdf_first_widget/pdf_next_widget (page
// annotations) and DocMdpEnforcer walks page /Annots directly, written that
// way precisely because AcroForm linkage cannot be assumed. A document PDFE
// itself recognises and verifies as signed was therefore invisible to the
// probe, and removeEncryption destroyed the signature with no warning on
// either channel: UNDER-warning, the security-relevant direction.
//
// OrphanedSignatureWidgetOutsideAcroFormStillWarns pins that, and
// DoublyReachableSignatureIsCountedOnce pins the other end — the union of the
// two traversals must not report two signatures for one.
//
// THE DISCRIMINATING CONTROL
// --------------------------
// An indiscriminate warning-on-every-call is not a control; it is noise that
// trains the user to click through. RemoveEncryptionOnUnsignedSourceIsSilent
// runs the SAME call against an unsigned, unprotected source and asserts NO
// warning on either channel, so every positive assertion below is attributable
// to the source carrying signatures rather than to the call being made at all.
//
// See docs/audit/meta-audit-2026-07/followup/phase-26-dispositions.md §1.

#include <gtest/gtest.h>

#include "encryption/EncryptionManager.h"
#include "encryption/EncryptionSettings.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"

#include <filesystem>
#include <memory>
#include <string>

using pdfe::encryption::EncryptionManager;
using pdfe::encryption::RemoveEncryptionReport;

namespace {

class RemoveEncryptionRewriteIntegrityTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        ctx_ = std::make_shared<pdfe::core::MuPdfContext>();
        mgr_ = std::make_unique<EncryptionManager>(ctx_);

        const ::testing::TestInfo* info =
            ::testing::UnitTest::GetInstance()->current_test_info();
        tempDir_ = std::filesystem::temp_directory_path()
                   / "pdfe_f26_rewrite_integrity" / info->name();
        std::filesystem::remove_all(tempDir_);
        std::filesystem::create_directories(tempDir_);

        // The thread-local channel is process-wide state. Start from a known
        // point so an assertion about it can never be satisfied by a leftover
        // from an earlier test in the same shard.
        pdfe::encryption::clearLastRemoveEncryptionWarning();
    }

    void TearDown() override
    {
        pdfe::encryption::clearLastRemoveEncryptionWarning();
        mgr_.reset();
        ctx_.reset();
    }

    static std::filesystem::path fixture(const char* name)
    {
        return std::filesystem::path(TEST_FIXTURES_DIR) / name;
    }

    std::shared_ptr<pdfe::core::MuPdfContext> ctx_;
    std::unique_ptr<EncryptionManager> mgr_;
    std::filesystem::path tempDir_;
};

// ===========================================================================
// AC #1 — a CERTIFIED source warns, and the warning names what breaks.
// ===========================================================================
TEST_F(RemoveEncryptionRewriteIntegrityTest,
       CertifiedSourceWarnsThatSignaturesWillNotVerify)
{
    const auto src = fixture("sig_iso_docmdp_p1.pdf");
    ASSERT_TRUE(std::filesystem::exists(src)) << src.string();

    auto doc = pdfe::core::Document::open(ctx_, src);
    ASSERT_NE(doc, nullptr);

    const auto out = tempDir_ / "decrypted_certified.pdf";
    RemoveEncryptionReport report;
    mgr_->removeEncryption(doc.get(), out, &report);

    // --- WARNED, NOT REFUSED. This is the direction the whole item turns on.
    EXPECT_TRUE(std::filesystem::exists(out))
        << "removeEncryption must still PRODUCE the unprotected copy; warning "
           "is the disposition, refusing is not";
    EXPECT_GT(std::filesystem::file_size(out), 0u);
    {
        auto reopened = pdfe::core::Document::open(ctx_, out);
        EXPECT_NE(reopened, nullptr)
            << "the written copy must be a readable PDF";
    }

    // --- The finding itself, on specific fields, not on "a message exists".
    EXPECT_TRUE(report.rewriteBreaksSignatures);
    EXPECT_TRUE(report.sourceCertified)
        << "sig_iso_docmdp_p1.pdf carries /Root /Perms /DocMDP";
    EXPECT_TRUE(report.sourceSigned);
    EXPECT_EQ(report.signatureCount, 1);

    // --- The TEXT. Asserting the clause, not merely that text is present:
    // F24 lost a whole clause to MuPdfContext::execute's errorMsg[256] and
    // only caught it because its test asserted the clause. This warning is
    // deliberately built outside any fz_throw path so it cannot be truncated,
    // and this assertion is what would notice if that ever changed.
    const std::string& w = report.rewriteIntegrityWarning;
    EXPECT_NE(w.find("will NOT verify"), std::string::npos) << w;
    EXPECT_NE(w.find("/ByteRange"), std::string::npos)
        << "the warning must say WHY the signature stops verifying: " << w;
    EXPECT_NE(w.find("/Perms /DocMDP"), std::string::npos)
        << "a certified source must be reported as certified: " << w;
    EXPECT_NE(w.find(out.filename().string()), std::string::npos)
        << "the warning must name the file it is about: " << w;
    EXPECT_NE(w.find("The original file is unchanged"), std::string::npos)
        << "the actionable tail must survive — this is the clause F24 lost to "
           "the 256-byte errorMsg buffer: " << w;

    // --- The NON-UI channel carries the same text, so a caller that passes
    // no report (every pre-F26 call site) can still find out.
    EXPECT_EQ(pdfe::encryption::lastRemoveEncryptionWarning(), w);
}

// ===========================================================================
// AC #1b — SIGNED BUT NOT CERTIFIED also warns.
//
// Certification is not the condition; a byte range is. This arm differs from
// the one above in exactly one property of the source (no /Perms /DocMDP) and
// isolates it: the warning still fires, and sourceCertified is now false.
// ===========================================================================
TEST_F(RemoveEncryptionRewriteIntegrityTest,
       SignedButUncertifiedSourceAlsoWarns)
{
    const auto src = fixture("sig_clean_signed.pdf");
    ASSERT_TRUE(std::filesystem::exists(src)) << src.string();

    auto doc = pdfe::core::Document::open(ctx_, src);
    ASSERT_NE(doc, nullptr);

    const auto out = tempDir_ / "decrypted_signed.pdf";
    RemoveEncryptionReport report;
    mgr_->removeEncryption(doc.get(), out, &report);

    EXPECT_TRUE(std::filesystem::exists(out));
    EXPECT_TRUE(report.rewriteBreaksSignatures);
    EXPECT_TRUE(report.sourceSigned);
    EXPECT_FALSE(report.sourceCertified)
        << "sig_clean_signed.pdf has no /Perms /DocMDP; reporting it as "
           "certified would mean the probe is not reading /Perms at all";
    EXPECT_EQ(report.signatureCount, 1);
    EXPECT_NE(report.rewriteIntegrityWarning.find("signature"),
              std::string::npos);
    EXPECT_EQ(report.rewriteIntegrityWarning.find("/Perms /DocMDP"),
              std::string::npos)
        << "an uncertified source must not be described as certified: "
        << report.rewriteIntegrityWarning;
}

// ===========================================================================
// F26 REMEDIATION (security-audit gate finding) — AC #1c.
//
// A signature that is NOT reachable from /Root /AcroForm /Fields still breaks.
//
// The original probe walked only the AcroForm field tree. That is narrower
// than every other signature-enumeration path in this codebase:
// SignatureManager lists via pdf_first_widget/pdf_next_widget (page
// annotations), and DocMdpEnforcer walks page /Annots directly, precisely
// because AcroForm linkage cannot be assumed. So a document PDFE itself
// recognises, displays and verifies as signed was invisible to the probe, and
// removeEncryption destroyed the signature with NO warning on either channel.
//
// Under-warning silently loses a user's signatures; over-warning is merely
// annoying. That asymmetry is the whole point, and this is the population —
// malformed or adversarially-crafted files — removeEncryption must be safe on,
// since it is deliberately exempt from the DocMDP direct-write guard.
//
// sig_orphan_widget_signed.pdf is a ONE-PROPERTY change from
// sig_clean_signed.pdf: no /AcroForm in the catalog. The signature is genuine,
// the widget is still in the page's /Annots, and /ByteRange still covers the
// whole file.
// ===========================================================================
TEST_F(RemoveEncryptionRewriteIntegrityTest,
       OrphanedSignatureWidgetOutsideAcroFormStillWarns)
{
    const auto src = fixture("sig_orphan_widget_signed.pdf");
    ASSERT_TRUE(std::filesystem::exists(src)) << src.string();

    auto doc = pdfe::core::Document::open(ctx_, src);
    ASSERT_NE(doc, nullptr);

    const auto out = tempDir_ / "decrypted_orphan_widget.pdf";
    RemoveEncryptionReport report;
    mgr_->removeEncryption(doc.get(), out, &report);

    // Still WARN, never REFUSE — the direction the whole item turns on.
    EXPECT_TRUE(std::filesystem::exists(out));
    EXPECT_GT(std::filesystem::file_size(out), 0u);
    {
        auto reopened = pdfe::core::Document::open(ctx_, out);
        EXPECT_NE(reopened, nullptr)
            << "the written copy must be a readable PDF";
    }

    EXPECT_TRUE(report.sourceSigned)
        << "the signature widget is in the page /Annots and is what "
           "SignatureManager itself lists; a probe that cannot see it "
           "UNDER-WARNS, and removeEncryption then destroys the signature "
           "silently";
    EXPECT_TRUE(report.rewriteBreaksSignatures);
    EXPECT_EQ(report.signatureCount, 1);
    EXPECT_FALSE(report.sourceCertified)
        << "this fixture carries no /Perms /DocMDP";
    EXPECT_NE(report.rewriteIntegrityWarning.find("/ByteRange"),
              std::string::npos)
        << report.rewriteIntegrityWarning;
    EXPECT_EQ(pdfe::encryption::lastRemoveEncryptionWarning(),
              report.rewriteIntegrityWarning);
}

// ===========================================================================
// The COUNTING control for the above. sig_clean_signed.pdf reaches the SAME
// signature by BOTH routes — it is in /Root /AcroForm /Fields AND in the
// page's /Annots. A union traversal that does not de-duplicate reports 2 here
// and the warning text says "2 signatures" about a document carrying one.
//
// This is the over-count half of the pair; SignedButUncertifiedSourceAlsoWarns
// asserts the same figure from the report, and this test states WHY the figure
// is load-bearing.
// ===========================================================================
TEST_F(RemoveEncryptionRewriteIntegrityTest,
       DoublyReachableSignatureIsCountedOnce)
{
    auto doc = pdfe::core::Document::open(ctx_, fixture("sig_clean_signed.pdf"));
    ASSERT_NE(doc, nullptr);

    RemoveEncryptionReport report;
    mgr_->removeEncryption(doc.get(), tempDir_ / "decrypted_dedup.pdf", &report);

    EXPECT_EQ(report.signatureCount, 1)
        << "the AcroForm field tree and the page /Annots reach the same "
           "signature; the union traversal must de-duplicate them";
    EXPECT_NE(report.rewriteIntegrityWarning.find("1 signature"),
              std::string::npos)
        << "the warning must not tell the user about signatures that are not "
           "there: " << report.rewriteIntegrityWarning;
    EXPECT_EQ(report.rewriteIntegrityWarning.find("signatures and"),
              std::string::npos)
        << report.rewriteIntegrityWarning;
}

// ===========================================================================
// THE DISCRIMINATING CONTROL — an unsigned, unprotected source is SILENT.
//
// Same manager, same call, same output handling. The only thing that differs
// is the source. If this ever starts producing a warning, the warning above
// stops being evidence of anything.
// ===========================================================================
TEST_F(RemoveEncryptionRewriteIntegrityTest,
       UnsignedUnprotectedSourceProducesNoWarning)
{
    const auto src = fixture("test_single.pdf");
    ASSERT_TRUE(std::filesystem::exists(src)) << src.string();

    auto doc = pdfe::core::Document::open(ctx_, src);
    ASSERT_NE(doc, nullptr);

    const auto out = tempDir_ / "decrypted_plain.pdf";
    RemoveEncryptionReport report;
    mgr_->removeEncryption(doc.get(), out, &report);

    // Proceeds exactly as before...
    EXPECT_TRUE(std::filesystem::exists(out));
    EXPECT_GT(std::filesystem::file_size(out), 0u);
    {
        auto reopened = pdfe::core::Document::open(ctx_, out);
        EXPECT_NE(reopened, nullptr);
    }

    // ...and says nothing, on either channel.
    EXPECT_FALSE(report.rewriteBreaksSignatures);
    EXPECT_FALSE(report.sourceSigned);
    EXPECT_FALSE(report.sourceCertified);
    EXPECT_EQ(report.signatureCount, 0);
    EXPECT_TRUE(report.rewriteIntegrityWarning.empty())
        << "unexpected warning on an unsigned source: "
        << report.rewriteIntegrityWarning;
    EXPECT_TRUE(pdfe::encryption::lastRemoveEncryptionWarning().empty())
        << "unexpected thread-local warning on an unsigned source: "
        << pdfe::encryption::lastRemoveEncryptionWarning();
}

// ===========================================================================
// The thread-local channel must not be able to report a PREVIOUS call's
// finding as this one's. Without the unconditional clear on entry, the
// unsigned run below would inherit the signed run's warning — which is the
// exact shape of false-positive this programme keeps finding.
// ===========================================================================
TEST_F(RemoveEncryptionRewriteIntegrityTest,
       ThreadLocalWarningIsClearedByTheNextCall)
{
    auto signedDoc = pdfe::core::Document::open(
        ctx_, fixture("sig_iso_docmdp_p1.pdf"));
    ASSERT_NE(signedDoc, nullptr);
    mgr_->removeEncryption(signedDoc.get(), tempDir_ / "first.pdf");
    ASSERT_FALSE(pdfe::encryption::lastRemoveEncryptionWarning().empty())
        << "the no-report overload must still record the warning, or the "
           "non-UI channel does not exist";

    auto plainDoc = pdfe::core::Document::open(ctx_, fixture("test_single.pdf"));
    ASSERT_NE(plainDoc, nullptr);
    mgr_->removeEncryption(plainDoc.get(), tempDir_ / "second.pdf");

    EXPECT_TRUE(pdfe::encryption::lastRemoveEncryptionWarning().empty())
        << "a stale warning was readable as this call's: "
        << pdfe::encryption::lastRemoveEncryptionWarning();
}

} // namespace
