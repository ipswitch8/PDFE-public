// -*- coding: utf-8 -*-
// Phase 4 (pipeline 243a1203): permission enforcement & encryption correctness.
//
// Findings covered:
//   R2-09  print permission is enforced at the operation boundary (PrintEngine
//          entry + MainWindow::onPrint, which the Named /Print link routes to)
//          rather than only via a disable-able QAction.
//   R2-10  content extraction (Copy / Select All / Export Plain Text / batch
//          PlainTextExporter) is gated on the copy/extract permission.
//   R2-11  removeEncryption requires OWNER authentication; a user-password
//          holder cannot strip protection.
//   R2-12  authLevel is derived from the ACTUAL authentication state, not the
//          permission bitmask (a fully-permissioned user is not mislabeled
//          owner; an all-permissions owner is not mislabeled user).
//
// Encrypted fixtures are generated in-process (EncryptionManager::saveEncrypted)
// so no binary blobs are committed and the exact permission/password matrix is
// explicit in the test.

#include <gtest/gtest.h>

#include "core/MuPdfContext.h"
#include "core/Document.h"
#include "core/Error.h"
#include "core/PrintEngine.h"
#include "encryption/EncryptionManager.h"
#include "encryption/EncryptionSettings.h"
#include "exports/PlainTextExporter.h"
#include "ui/MainWindow.h"
#include "ui/PdfCanvas.h"
#include "textedit/TextEditTool.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QGuiApplication>
#include <QMetaObject>
#include <QMouseEvent>
#include <QPrinter>
#include <QStatusBar>
#include <QTemporaryDir>

#include <filesystem>
#include <memory>
#include <string>

using namespace pdfe::encryption;

namespace {

std::filesystem::path fixturesDir() {
    return std::filesystem::path(TEST_FIXTURES_DIR);
}

// Build an encrypted PDF from test_single.pdf with the given passwords and a
// single denied permission, writing it into `dir`. Returns the output path.
// `denyPrint`/`denyCopy` select which permission to clear; all others stay on.
std::filesystem::path makeEncryptedFrom(const std::filesystem::path& dir,
                                        const std::string& name,
                                        const std::string& sourceFixture,
                                        const std::string& userPw,
                                        const std::string& ownerPw,
                                        bool denyPrint,
                                        bool denyCopy) {
    auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
    auto src = pdfe::core::Document::open(ctx, fixturesDir() / sourceFixture);
    EXPECT_NE(src, nullptr);
    EncryptionManager mgr(ctx);

    EncryptionSettings settings;
    settings.algorithm = EncryptionAlgorithm::AES_256;
    settings.userPassword = userPw;   // may be empty (opens without a prompt)
    settings.ownerPassword = ownerPw; // required
    settings.permissions = PermissionFlags::all();
    if (denyPrint) settings.permissions.print = false;
    if (denyCopy)  settings.permissions.copy = false;

    auto out = dir / name;
    mgr.saveEncrypted(src.get(), out, settings);
    EXPECT_TRUE(std::filesystem::exists(out));
    return out;
}

// Convenience wrapper: encrypt test_single.pdf (the default source for the
// permission-query fixtures).
std::filesystem::path makeEncrypted(const std::filesystem::path& dir,
                                    const std::string& name,
                                    const std::string& userPw,
                                    const std::string& ownerPw,
                                    bool denyPrint,
                                    bool denyCopy) {
    return makeEncryptedFrom(dir, name, "test_single.pdf", userPw, ownerPw,
                             denyPrint, denyCopy);
}

} // namespace

// ===========================================================================
// R2-12 — authLevel from the ACTUAL authentication state
// ===========================================================================

TEST(PermissionEnforcement_R2_12, OwnerPasswordYieldsOwnerLevel) {
    QTemporaryDir tmp;
    auto enc = makeEncrypted(tmp.path().toStdString(), "authlvl.pdf",
                             "userpw", "ownerpw",
                             /*denyPrint=*/false, /*denyCopy=*/false);

    auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
    auto doc = pdfe::core::Document::open(ctx, enc, "ownerpw");
    ASSERT_NE(doc, nullptr);
    // Owner authority (bit 0x2 set). The old bitmask heuristic returned 1 (user)
    // for a QPDF all-permissions doc whose P value is 0xFFFFFFFC — this asserts
    // the real fz_authenticate_password result is used instead.
    EXPECT_TRUE(doc->isOwnerAuthenticated());
    EXPECT_NE(doc->authLevel() & 2, 0);

    EncryptionManager mgr(ctx);
    EXPECT_EQ(mgr.getEncryptionInfo(doc.get()).authLevel & 2, 2);
}

TEST(PermissionEnforcement_R2_12, UserPasswordNotMislabeledOwner) {
    QTemporaryDir tmp;
    auto enc = makeEncrypted(tmp.path().toStdString(), "authlvl.pdf",
                             "userpw", "ownerpw", false, false);

    auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
    auto doc = pdfe::core::Document::open(ctx, enc, "userpw");
    ASSERT_NE(doc, nullptr);
    // Even though every permission is granted, a USER-password caller must NOT
    // be reported as owner (the R2-12 defect).
    EXPECT_FALSE(doc->isOwnerAuthenticated());
    EXPECT_EQ(doc->authLevel(), 1);
}

TEST(PermissionEnforcement_R2_12, UnencryptedIsOwnerEquivalent) {
    auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
    auto doc = pdfe::core::Document::open(ctx, fixturesDir() / "test_single.pdf");
    ASSERT_NE(doc, nullptr);
    // An unencrypted document carries no access restrictions — owner-equivalent
    // so privileged ops (e.g. save-a-copy) are never spuriously blocked.
    EXPECT_TRUE(doc->isOwnerAuthenticated());
    EXPECT_TRUE(doc->printAllowed());
    EXPECT_TRUE(doc->copyAllowed());
}

// ===========================================================================
// R2-11 — removeEncryption requires OWNER authentication
// ===========================================================================

TEST(PermissionEnforcement_R2_11, UserPasswordCannotStripEncryption) {
    QTemporaryDir tmp;
    auto enc = makeEncrypted(tmp.path().toStdString(), "strip.pdf",
                             "userpw", "ownerpw", false, false);

    auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
    auto doc = pdfe::core::Document::open(ctx, enc, "userpw");
    ASSERT_NE(doc, nullptr);

    EncryptionManager mgr(ctx);
    auto dec = std::filesystem::path(tmp.path().toStdString()) / "stripped_user.pdf";
    // User-level auth must be refused.
    EXPECT_THROW(mgr.removeEncryption(doc.get(), dec), pdfe::core::PdfeException);
    EXPECT_FALSE(std::filesystem::exists(dec))
        << "no unprotected copy may be produced for a user-only caller";
}

TEST(PermissionEnforcement_R2_11, OwnerPasswordCanStripEncryption) {
    QTemporaryDir tmp;
    auto enc = makeEncrypted(tmp.path().toStdString(), "strip.pdf",
                             "userpw", "ownerpw", false, false);

    auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
    auto doc = pdfe::core::Document::open(ctx, enc, "ownerpw");
    ASSERT_NE(doc, nullptr);

    EncryptionManager mgr(ctx);
    auto dec = std::filesystem::path(tmp.path().toStdString()) / "stripped_owner.pdf";
    ASSERT_NO_THROW(mgr.removeEncryption(doc.get(), dec));
    ASSERT_TRUE(std::filesystem::exists(dec));

    // The produced copy must actually be unencrypted.
    auto reopened = pdfe::core::Document::open(ctx, dec);
    ASSERT_NE(reopened, nullptr);
    EXPECT_FALSE(mgr.isEncrypted(reopened.get()));
}

TEST(PermissionEnforcement_R2_11, UnencryptedSaveCopyNotBlocked) {
    // removeEncryption on an unencrypted document is the "save an unprotected
    // copy" path — owner-equivalent authority, so it must NOT be refused.
    auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
    auto doc = pdfe::core::Document::open(ctx, fixturesDir() / "test_single.pdf");
    ASSERT_NE(doc, nullptr);
    QTemporaryDir tmp;
    auto out = std::filesystem::path(tmp.path().toStdString()) / "copy.pdf";
    EncryptionManager mgr(ctx);
    EXPECT_NO_THROW(mgr.removeEncryption(doc.get(), out));
    EXPECT_TRUE(std::filesystem::exists(out));
}

// ===========================================================================
// R2-10 — content extraction gated on the copy/extract permission
// ===========================================================================

TEST(PermissionEnforcement_R2_10, DocumentCopyAllowedReflectsPermission) {
    QTemporaryDir tmp;
    auto enc = makeEncrypted(tmp.path().toStdString(), "copydenied.pdf",
                             "", "ownerpw", /*denyPrint=*/false, /*denyCopy=*/true);
    auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
    auto doc = pdfe::core::Document::open(ctx, enc);  // empty user password
    ASSERT_NE(doc, nullptr);
    EXPECT_FALSE(doc->copyAllowed());
    EXPECT_TRUE(doc->printAllowed());  // only copy was denied
}

TEST(PermissionEnforcement_R2_10, PlainTextExporterRefusesCopyDenied) {
    QTemporaryDir tmp;
    auto enc = makeEncrypted(tmp.path().toStdString(), "copydenied.pdf",
                             "", "ownerpw", false, true);
    auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
    auto doc = pdfe::core::Document::open(ctx, enc);
    ASSERT_NE(doc, nullptr);

    auto out = std::filesystem::path(tmp.path().toStdString()) / "leak.txt";
    // Batch/programmatic export must refuse and write NOTHING.
    EXPECT_FALSE(pdfe::exports::PlainTextExporter::write(doc.get(), out));
    EXPECT_FALSE(std::filesystem::exists(out))
        << "copy-denied text must not be written to disk";
}

TEST(PermissionEnforcement_R2_10, PlainTextExporterAllowsWhenPermitted) {
    // Positive control: an unencrypted (copy-allowed) doc still exports.
    auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
    auto doc = pdfe::core::Document::open(ctx, fixturesDir() / "test_3page.pdf");
    ASSERT_NE(doc, nullptr);
    QTemporaryDir tmp;
    auto out = std::filesystem::path(tmp.path().toStdString()) / "ok.txt";
    EXPECT_TRUE(pdfe::exports::PlainTextExporter::write(doc.get(), out));
    EXPECT_TRUE(std::filesystem::exists(out));
}

// ===========================================================================
// R2-09 — print permission enforced at the PrintEngine boundary
// ===========================================================================

// A print-denied document must be refused by the ENGINE at its permission gate,
// BEFORE any rasterization. Uses a fully valid PDF-output QPrinter so the engine
// would proceed to render (and, in this headless test environment, actually
// crash in Qt's print rasterizer — SEH 0xC0000005) were the gate absent. The
// gate short-circuits with `return false` before that render, so the clean
// EXPECT_FALSE here is only reachable WITH the gate: removing it turns this test
// into a caught SEH failure, i.e. the gate is load-bearing (mutation-killable).
// (The success path of PrintEngine::print cannot be exercised headlessly for the
// same rasterizer reason — real printing works in the running application; this
// is an environment limitation, not a product limitation. Print-ALLOWED behavior
// is covered non-destructively by the printAllowed() query test below and by the
// MainWindow onPrint UI tests.)
TEST(PermissionEnforcement_R2_09, PrintEngineRefusesPrintDeniedWithValidPrinter) {
    QTemporaryDir tmp;
    auto enc = makeEncrypted(tmp.path().toStdString(), "printdenied.pdf",
                             "", "ownerpw", /*denyPrint=*/true, /*denyCopy=*/false);
    auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
    auto doc = pdfe::core::Document::open(ctx, enc);
    ASSERT_NE(doc, nullptr);
    ASSERT_FALSE(doc->printAllowed());

    QPrinter printer(QPrinter::HighResolution);
    printer.setOutputFormat(QPrinter::PdfFormat);
    auto outPdf = std::filesystem::path(tmp.path().toStdString()) / "printed_denied.pdf";
    printer.setOutputFileName(QString::fromStdString(outPdf.string()));

    pdfe::core::PrintEngine engine(doc.get());
    pdfe::core::PrintOptions opts;
    EXPECT_FALSE(engine.print(&printer, opts))
        << "print-denied document must be refused by the engine before render";
}

// Headless-safe permission-query coverage (no rasterization): the live print
// permission the engine / onPrint consult must reflect the document's actual
// restriction — denied for a print-denied PDF, allowed for an unencrypted one.
TEST(PermissionEnforcement_R2_09, PrintAllowedReflectsPermission) {
    QTemporaryDir tmp;
    auto denied = makeEncrypted(tmp.path().toStdString(), "pd.pdf",
                                "", "ownerpw", /*denyPrint=*/true, /*denyCopy=*/false);
    auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
    auto deniedDoc = pdfe::core::Document::open(ctx, denied);
    ASSERT_NE(deniedDoc, nullptr);
    EXPECT_FALSE(deniedDoc->printAllowed());

    auto allowedDoc = pdfe::core::Document::open(ctx, fixturesDir() / "test_single.pdf");
    ASSERT_NE(allowedDoc, nullptr);
    EXPECT_TRUE(allowedDoc->printAllowed());
}

// ===========================================================================
// UI boundary — R2-09 / R2-10 through MainWindow slots
//
// These open the actual application window and drive the slots the menu,
// toolbar, and Named /Print link route to. Refusal is observed via the status
// bar message (deterministic, non-blocking — the gate returns before any modal
// dialog). onPrint is invoked directly, exactly as the Named /Print link does.
// ===========================================================================

namespace {
QString statusMessageAfter(pdfe::ui::MainWindow& mw, const char* slot) {
    // Private slots are still invokable by name via the meta-object system.
    QMetaObject::invokeMethod(&mw, slot, Qt::DirectConnection);
    QApplication::processEvents();
    return mw.statusBar() ? mw.statusBar()->currentMessage() : QString();
}
} // namespace

TEST(PermissionEnforcement_UI, OnPrintRefusesPrintDeniedLinkPath) {
    QTemporaryDir tmp;
    auto enc = makeEncrypted(tmp.path().toStdString(), "printdenied.pdf",
                             "", "ownerpw", /*denyPrint=*/true, /*denyCopy=*/false);
    pdfe::ui::MainWindow mw;
    mw.openFile(QString::fromStdString(enc.string()));
    QApplication::processEvents();

    // Named /Print link routes to onPrint(), bypassing the disabled QAction.
    const QString msg = statusMessageAfter(mw, "onPrint");
    EXPECT_TRUE(msg.contains("not permitted"))
        << "onPrint must refuse a print-denied document; msg=" << msg.toStdString();
}

TEST(PermissionEnforcement_UI, OnPrintRefusesOnLivePermissionEvenIfToggleCleared) {
    QTemporaryDir tmp;
    auto enc = makeEncrypted(tmp.path().toStdString(), "printdenied.pdf",
                             "", "ownerpw", true, false);
    pdfe::ui::MainWindow mw;
    mw.openFile(QString::fromStdString(enc.string()));
    QApplication::processEvents();

    // Clear the user-facing Prevent-Print toggle. The LIVE document permission
    // is the mandatory floor, so printing must STILL be refused — this kills a
    // mutant that gates only on the toggle.
    if (auto* toggle = mw.findChild<QAction*>("togglePrintPreventionAction")) {
        toggle->setChecked(false);
        QApplication::processEvents();
    }
    const QString msg = statusMessageAfter(mw, "onPrint");
    EXPECT_TRUE(msg.contains("not permitted"))
        << "live print permission must override a cleared toggle; msg="
        << msg.toStdString();
}

TEST(PermissionEnforcement_UI, OnExportPlainTextRefusesCopyDenied) {
    QTemporaryDir tmp;
    auto enc = makeEncrypted(tmp.path().toStdString(), "copydenied.pdf",
                             "", "ownerpw", false, /*denyCopy=*/true);
    pdfe::ui::MainWindow mw;
    mw.openFile(QString::fromStdString(enc.string()));
    QApplication::processEvents();

    const QString msg = statusMessageAfter(mw, "onExportPlainText");
    EXPECT_TRUE(msg.contains("not permitted"))
        << "onExportPlainText must refuse a copy-denied document; msg="
        << msg.toStdString();
}

TEST(PermissionEnforcement_UI, OnCopyRefusesCopyDeniedEvenIfToggleCleared) {
    QTemporaryDir tmp;
    auto enc = makeEncrypted(tmp.path().toStdString(), "copydenied.pdf",
                             "", "ownerpw", false, true);
    pdfe::ui::MainWindow mw;
    mw.openFile(QString::fromStdString(enc.string()));
    QApplication::processEvents();

    if (auto* toggle = mw.findChild<QAction*>("toggleCopyPreventionAction")) {
        toggle->setChecked(false);
        QApplication::processEvents();
    }
    const QString msg = statusMessageAfter(mw, "onCopy");
    EXPECT_TRUE(msg.contains("not permitted"))
        << "live copy permission must override a cleared toggle; msg="
        << msg.toStdString();
}

// R2-10 bypass (found by the Phase-4 security-audit gate): the text-edit tool's
// right-click context menu wires Copy/Cut straight to TextEditTool::performCopy/
// performCut -> copySelection(), which formerly checked NOTHING (not even the
// toggle). This drives that exact path on a copy-denied document and asserts the
// clipboard is never written and Cut never deletes the span. The enforcement
// funnel is TextEditTool::copyDenied() (live copyAllowed() || Prevent-Copy).
TEST(PermissionEnforcement_UI, TextEditCopyCutRefusesCopyDenied) {
    const char* kSpanFixture = "embedded_descriptor_style.pdf";
    auto srcPath = fixturesDir() / kSpanFixture;
    ASSERT_TRUE(std::filesystem::exists(srcPath))
        << kSpanFixture << " (editable-span fixture) is required";

    QTemporaryDir tmp;
    // Copy-denied encrypted variant of the editable-span fixture (empty user
    // password -> opens without a dialog; owner-protected with copy denied).
    auto enc = makeEncryptedFrom(tmp.path().toStdString(), "copydenied_span.pdf",
                                 kSpanFixture, "", "ownerpw",
                                 /*denyPrint=*/false, /*denyCopy=*/true);

    pdfe::ui::MainWindow mw;
    mw.openFile(QString::fromStdString(enc.string()));
    QApplication::processEvents();
    auto* c = mw.findChild<pdfe::ui::PdfCanvas*>("pdfCanvas");
    ASSERT_NE(c, nullptr);
    ASSERT_NE(c->document(), nullptr);
    ASSERT_FALSE(c->document()->copyAllowed()) << "fixture must deny copy";

    auto* editAction = mw.findChild<QAction*>("textEditToolAction");
    ASSERT_NE(editAction, nullptr);
    editAction->trigger();  // installs the TextEditTool
    QApplication::processEvents();

    auto* tool = c->textEditTool();
    ASSERT_NE(tool, nullptr);
    tool->setPage(0);
    ASSERT_GT(tool->spanCountForTesting(), 0) << "fixture must expose a vector span";
    QRectF bbox = tool->spanBboxForTesting(0);
    ASSERT_FALSE(bbox.isEmpty());
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(10, 10),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    tool->onMousePress(&press, 0, bbox.center());
    ASSERT_TRUE(tool->isEditing()) << "clicking the span must enter edit mode";

    const int spanLen = tool->spanTextForTesting(0).size();
    ASSERT_GT(spanLen, 0);
    tool->performSelectAll();  // select the whole span so copy has content
    ASSERT_NE(tool->selectionStartForTesting(), tool->selectionEndForTesting())
        << "a non-empty selection is required to make the bypass observable";

    const QString sentinel = QStringLiteral("PDFE-COPYDENIED-SENTINEL");

    // Context-menu Copy path.
    QGuiApplication::clipboard()->setText(sentinel);
    tool->performCopy();
    QApplication::processEvents();
    EXPECT_EQ(QGuiApplication::clipboard()->text(), sentinel)
        << "context-menu Copy must NOT place copy-denied text on the clipboard";

    // Context-menu Cut path: neither copies NOR deletes the span.
    const QString before = tool->spanTextForTesting(0);
    QGuiApplication::clipboard()->setText(sentinel);
    tool->performCut();
    QApplication::processEvents();
    EXPECT_EQ(QGuiApplication::clipboard()->text(), sentinel)
        << "context-menu Cut must NOT place copy-denied text on the clipboard";
    EXPECT_EQ(tool->spanTextForTesting(0), before)
        << "a denied Cut must not delete the span either";
}
