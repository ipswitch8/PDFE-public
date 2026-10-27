// -*- coding: utf-8 -*-
//
// GitHub #179 — pipeline p83-outstanding, phase-2.
//
// #179 records two PDF 2.0 gaps left behind by #3, both about SURFACING
// rather than detecting:
//
//   1. `UnencryptedWrapperProbe` builds the user-facing summary in tested core
//      code (tests/test_pdf20_features.cpp asserts the message itself). What
//      nothing tested is `MainWindow::openFile` RAISING it. A detector whose
//      answer nobody surfaces meets the letter of "detected and reported" and
//      none of its intent.
//
//   2. `/EncryptMetadata`. The issue says the flag "appears nowhere except a
//      comment". That was true of the literal spelling only — re-checked
//      before anything was written here: EncryptionManager.cpp:173 already
//      reads it (`pdf_crypt_encrypt_metadata` -> EncryptionInfo::
//      metadataEncrypted) and EncryptionManager.cpp already passes
//      EncryptionSettings::encryptMetadata to QPDF's R4/R6 writers. What was
//      genuinely missing is the SURFACE: the only place the value reached a
//      user was a string concatenated inline inside onDocumentPermissions'
//      QMessageBox, which no test can read. This file adds the missing seam
//      and asserts the value differs correctly between two documents.
//
// WHY THESE ARE GOOGLETESTS AND NOT pywinauto. #179 itself states the rule:
// UIA cannot enumerate a Qt app-modal `QDialog::exec()` — a dialog once sat
// open for four minutes while a 240 s UIA poll reported nothing — and directs
// the test at `registerDialogIntrospection()` or the Win32 EnumWindows helper.
// `registerDialogIntrospection()` is the dialog's OWN seam and is reachable
// in-process, so it answers the question ("did openFile raise the advisory,
// with what children") without a second process, a launched pdfe.exe, or a
// desktop session at all. The pywinauto route would be strictly weaker
// evidence at several times the cost.
//
// FIXTURES ARE BUILT IN-TEST. The wrapper PDF is generated the same way
// test_pdf20_features.cpp generates it; the two encrypted documents are
// produced by EncryptionManager itself, differing ONLY in
// EncryptionSettings::encryptMetadata. Both are then READ BACK through an
// independent path (MuPDF's pdf_crypt_encrypt_metadata, via the product), so
// the writer is not asserting about itself.

#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "encryption/EncryptionManager.h"
#include "encryption/EncryptionSettings.h"
#include "ui/MainWindow.h"
#include "ui/TestDialogSeam.h"

#include <QApplication>
#include <QByteArray>
#include <QStatusBar>
#include <QString>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

using namespace pdfe::ui;
using pdfe::core::Document;
using pdfe::core::MuPdfContext;
using pdfe::encryption::EncryptionAlgorithm;
using pdfe::encryption::EncryptionManager;
using pdfe::encryption::EncryptionSettings;

namespace {

namespace fs = std::filesystem;

fs::path tempDir()
{
    auto d = fs::temp_directory_path() / "pdfe_pdf20_ui_surfacing";
    fs::create_directories(d);
    return d;
}

void writeBytes(const fs::path& p, const std::string& bytes)
{
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

// Same shape as tests/test_pdf20_features.cpp's generator: a PDF 2.0 document
// with a cover-sheet page plus an /AFRelationship /EncryptedPayload attachment.
// `payload=false` gives the ordinary document used as the control.
std::string wrapperPdf(bool payload)
{
    const std::string cover =
        "BT /F1 14 Tf 60 700 Td (This document is protected.) Tj ET\n";

    std::string catalog = "1 0 obj\n<< /Type /Catalog /Pages 2 0 R";
    if (payload) {
        catalog += " /Collection << /Type /Collection /View /H >> /AF [5 0 R]";
    }
    catalog += " >>\nendobj\n";

    std::string out = "%PDF-2.0\n";
    out += catalog;
    out += "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n";
    out += "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
           "/Resources << /Font << /F1 << /Type /Font /Subtype /Type1 "
           "/BaseFont /Helvetica >> >> >> /Contents 4 0 R >>\nendobj\n";
    out += "4 0 obj\n<< /Length " + std::to_string(cover.size()) +
           " >>\nstream\n" + cover + "endstream\nendobj\n";
    if (payload) {
        out += "5 0 obj\n<< /Type /Filespec /F (payload.pdf) /UF (payload.pdf) "
               "/AFRelationship /EncryptedPayload /EF << /F 6 0 R >> "
               ">>\nendobj\n";
        const std::string blob = "ENCRYPTED-PAYLOAD-BYTES";
        out += "6 0 obj\n<< /Type /EmbeddedFile /Subtype /application#2Fpdf "
               "/EP << /Type /EncryptedPayload /Name /AcmeSecure /V 2 >> "
               "/Length " + std::to_string(blob.size()) + " >>\nstream\n" +
               blob + "\nendstream\nendobj\n";
    }
    out += "trailer\n<< /Size 7 /Root 1 0 R >>\nstartxref\n0\n%%EOF\n";
    return out;
}

constexpr const char* kAdvisoryDialog = "unencryptedWrapperAdvisoryDialog";

class Pdf20UiSurfacingTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        hadPipes_ = qEnvironmentVariableIsSet("PDFE_TEST_PIPES");
        savedPipes_ = hadPipes_ ? qgetenv("PDFE_TEST_PIPES") : QByteArray();
        hadDismiss_ = qEnvironmentVariableIsSet("PDFE_TEST_DIALOG_AUTODISMISS");
        savedDismiss_ = hadDismiss_
            ? qgetenv("PDFE_TEST_DIALOG_AUTODISMISS") : QByteArray();

        qputenv("PDFE_TEST_PIPES", "1");
        // Without this the advisory's exec() would never return and the test
        // would hang rather than fail — the seam's own auto-dismiss path.
        qputenv("PDFE_TEST_DIALOG_AUTODISMISS", "1");

        td_ = tempDir();
        ctx_ = std::make_shared<MuPdfContext>();
        // The registry is process-wide and persists across tests. Clear the
        // key first, so "present" can only mean "this test's openFile raised
        // it" and never "some earlier test did".
        testseam::dialogRegistry().remove(QString::fromLatin1(kAdvisoryDialog));
        mw_ = std::make_unique<MainWindow>();
    }

    void TearDown() override
    {
        mw_.reset();
        QApplication::processEvents();
        testseam::dialogRegistry().remove(QString::fromLatin1(kAdvisoryDialog));

        if (hadPipes_) { qputenv("PDFE_TEST_PIPES", savedPipes_); }
        else           { qunsetenv("PDFE_TEST_PIPES"); }
        if (hadDismiss_) {
            qputenv("PDFE_TEST_DIALOG_AUTODISMISS", savedDismiss_);
        } else {
            qunsetenv("PDFE_TEST_DIALOG_AUTODISMISS");
        }
    }

    fs::path td_;
    std::shared_ptr<MuPdfContext> ctx_;
    std::unique_ptr<MainWindow> mw_;
    bool hadPipes_ = false;
    bool hadDismiss_ = false;
    QByteArray savedPipes_;
    QByteArray savedDismiss_;
};

}  // namespace

// ---------------------------------------------------------------------------
// #179 part 1 — openFile RAISES the wrapper advisory.
// ---------------------------------------------------------------------------
TEST_F(Pdf20UiSurfacingTest, OpeningAWrapperRaisesTheAdvisoryDialog)
{
    const auto path = td_ / "wrapper.pdf";
    writeBytes(path, wrapperPdf(/*payload=*/true));

    mw_->openFile(QString::fromStdString(path.string()));
    QApplication::processEvents();

    const QString record =
        testseam::dialogIntrospection(QString::fromLatin1(kAdvisoryDialog));
    EXPECT_FALSE(record.isEmpty())
        << "MainWindow::openFile did not raise the unencrypted-wrapper "
           "advisory for a document carrying an /AFRelationship "
           "/EncryptedPayload attachment. The probe detects it; nothing told "
           "the user.";
    EXPECT_TRUE(record.startsWith(QString::fromLatin1(kAdvisoryDialog)))
        << "advisory inventory recorded under the wrong key: "
        << record.toStdString();

    // The status-line half of the report, which is what remains on screen
    // after the modal is dismissed. It is the LAST showMessage in openFile,
    // so reading it here is not racing a later one.
    const QString status = mw_->statusBar()->currentMessage();
    EXPECT_TRUE(status.contains(QLatin1String("cover sheet")))
        << "the status line does not say the visible page is a cover sheet; "
           "it said: " << status.toStdString();
}

// CONTROL. An ordinary document must raise nothing — otherwise the assertion
// above is satisfied by an advisory that fires for every file, which would
// train the user to dismiss it and is worse than silence.
TEST_F(Pdf20UiSurfacingTest, OpeningAnOrdinaryDocumentRaisesNoAdvisory)
{
    const auto path = td_ / "plain.pdf";
    writeBytes(path, wrapperPdf(/*payload=*/false));

    mw_->openFile(QString::fromStdString(path.string()));
    QApplication::processEvents();

    EXPECT_TRUE(
        testseam::dialogIntrospection(QString::fromLatin1(kAdvisoryDialog))
            .isEmpty())
        << "the wrapper advisory was raised for an ordinary PDF";
    EXPECT_FALSE(mw_->statusBar()->currentMessage()
                     .contains(QLatin1String("cover sheet")));
}

// ---------------------------------------------------------------------------
// #179 part 2 — /EncryptMetadata, surfaced where a test can read it.
// ---------------------------------------------------------------------------
namespace {

// Produce an encrypted copy of `source` differing only in encryptMetadata.
//
// The USER password is deliberately EMPTY so MainWindow::openFile opens it
// without a PasswordDialog: the subject here is the surfaced flag, and a
// modal password prompt in the middle of it would be testing something else.
// An owner password is still set, so the document is genuinely encrypted.
bool makeEncryptedCopy(EncryptionManager& mgr,
                       const std::shared_ptr<MuPdfContext>& ctx,
                       const fs::path& source,
                       const fs::path& out,
                       bool encryptMetadata)
{
    auto doc = Document::open(ctx, source);
    if (!doc) return false;
    EncryptionSettings settings;
    settings.algorithm = EncryptionAlgorithm::AES_256;
    settings.userPassword = "";
    settings.ownerPassword = "owner-p83";
    settings.encryptMetadata = encryptMetadata;
    std::error_code ec;
    fs::remove(out, ec);
    mgr.saveEncrypted(doc.get(), out, settings);
    return fs::exists(out);
}

}  // namespace

TEST_F(Pdf20UiSurfacingTest, EncryptMetadataIsSurfacedAndDiffersBetweenDocuments)
{
    const fs::path source = fs::path(TEST_FIXTURES_DIR) / "test_single.pdf";
    if (!fs::exists(source)) {
        GTEST_SKIP() << "test_single.pdf fixture missing";
    }
    EncryptionManager mgr(ctx_);
    const auto offPath = td_ / "encrypt_metadata_false.pdf";
    const auto onPath = td_ / "encrypt_metadata_true.pdf";
    ASSERT_TRUE(makeEncryptedCopy(mgr, ctx_, source, offPath, false));
    ASSERT_TRUE(makeEncryptedCopy(mgr, ctx_, source, onPath, true));

    mw_->openFile(QString::fromStdString(offPath.string()));
    QApplication::processEvents();
    const QString off = mw_->documentEncryptionDiag();

    mw_->openFile(QString::fromStdString(onPath.string()));
    QApplication::processEvents();
    const QString on = mw_->documentEncryptionDiag();

    // Both must actually be encrypted, or "metadataEncrypted=0" would just be
    // the absence of an /Encrypt dictionary and would mean nothing.
    ASSERT_TRUE(off.contains(QLatin1String("encrypted=1")))
        << "the EncryptMetadata=false document did not open as encrypted: "
        << off.toStdString();
    ASSERT_TRUE(on.contains(QLatin1String("encrypted=1")))
        << "the EncryptMetadata=true document did not open as encrypted: "
        << on.toStdString();

    EXPECT_TRUE(off.contains(QLatin1String("metadataEncrypted=0")))
        << "/EncryptMetadata false was not surfaced: " << off.toStdString();
    EXPECT_TRUE(on.contains(QLatin1String("metadataEncrypted=1")))
        << "/EncryptMetadata true was not surfaced: " << on.toStdString();
    EXPECT_NE(off, on)
        << "the surfaced encryption facts are identical for a document whose "
           "metadata is unencrypted and one whose metadata is encrypted, so "
           "the seam is not reading the flag at all";
}

// An UNENCRYPTED document has no /EncryptMetadata at all. The seam must say
// so rather than reporting the struct default (`metadataEncrypted = true`)
// as though it had been read from a document.
TEST_F(Pdf20UiSurfacingTest, AnUnencryptedDocumentSurfacesAsNotEncrypted)
{
    const auto path = td_ / "plain_for_encdiag.pdf";
    writeBytes(path, wrapperPdf(/*payload=*/false));
    mw_->openFile(QString::fromStdString(path.string()));
    QApplication::processEvents();

    const QString diag = mw_->documentEncryptionDiag();
    EXPECT_TRUE(diag.contains(QLatin1String("encrypted=0")))
        << "an unencrypted document reported as encrypted: "
        << diag.toStdString();
}

TEST_F(Pdf20UiSurfacingTest, TheEncryptionDiagSeamIsGatedOnTestPipes)
{
    const auto path = td_ / "plain_for_gate.pdf";
    writeBytes(path, wrapperPdf(/*payload=*/false));
    mw_->openFile(QString::fromStdString(path.string()));
    QApplication::processEvents();

    qunsetenv("PDFE_TEST_PIPES");
    EXPECT_EQ(mw_->documentEncryptionDiag(), QString("seam-disabled"));
    qputenv("PDFE_TEST_PIPES", "1");
    EXPECT_NE(mw_->documentEncryptionDiag(), QString("seam-disabled"))
        << "the seam stayed off with PDFE_TEST_PIPES set, so the check above "
           "proves nothing";
}
