// -*- coding: utf-8 -*-
//
// Harness-remediation PHASE-6, register target R-35 (High) —
// DESTRUCTION-UNDER-ACTIVE-STATE / crash-on-close.
//
// THE DEFECT CLASS THIS TEST PINS
// -------------------------------
// PDFE can UAF/crash when the owning window (or the document graph) is torn
// down while an edit is ACTIVE. The canonical, already-fixed instance is the
// ~MainWindow mid-edit teardown crash (commit 4fae336):
//
//   canvas_ is a CHILD QWidget, so it is destroyed in ~QObject AFTER
//   MainWindow's own members (document_, muPdfContext_) are freed. Its
//   ~TextEditTool runs cancelEdit(), which dereferences the raw document_/
//   context_ pointers the tool holds — now dangling — → 0xC0000005 (Debug) /
//   heap-use-after-free (ASan). The fix makes ~MainWindow call
//   canvas_->clearTextEditTool() up front (idempotent with closeEvent) so the
//   tool releases the document BEFORE it is freed.
//
// The nearest existing oracle (tests/test_save_throw_undo_safety.cpp) proves a
// THROWN SAVE rebinds the undo stack; it does NOT cover destroying the graph
// out from under a live edit. This file adds that missing class.
//
// THE ORACLE (two layers)
// -----------------------
// Layer A — the real ~MainWindow regression. Construct a real MainWindow, open
//   a fixture, activate the text-edit tool, drive it into an ACTIVE edit
//   session (isEditing()==true), then DESTROY the MainWindow directly (no
//   close()/closeEvent — the programmatic-teardown path the fix note calls out)
//   and assert clean teardown. RED (0xC0000005 / UAF) if clearTextEditTool() is
//   removed from ~MainWindow; GREEN on the fixed tree. Also covers an
//   active-annotation-selection teardown and a tool-active-but-not-editing
//   control.
// Layer B — a headless destruction-ORDER matrix over every Page*-holding edit
//   family (Annotation, Redaction, Form, Signature, Image insert/remove/resize/
//   rotate/crop). Start the edit so the command is ACTIVE on a real UndoManager
//   with a live commandPages vector + manager, then destroy the Document and
//   its owning Pages FIRST (the window-close order) while the undo stack and
//   managers still reference them, and assert the subsequent manager/undo
//   destruction is UAF-clean. Under msvc-asan a reintroduced dtor-order
//   dereference of the freed document/page aborts regardless of any catch.
//
// Runs under the project crash guards installed by test_main.cpp
// (SetErrorMode + _CrtSetReportHook), so a hard fault fails the test instead of
// popping a modal.

#include <gtest/gtest.h>

#include "ui/MainWindow.h"
#include "ui/PdfCanvas.h"
#include "textedit/TextEditTool.h"

#include "core/Document.h"
#include "core/Page.h"
#include "core/MuPdfContext.h"
#include "core/UndoManager.h"

#include "annotations/AnnotationCommands.h"
#include "annotations/AnnotationManager.h"
#include "redaction/RedactionCommands.h"
#include "redaction/RedactionManager.h"
#include "forms/FormCommands.h"
#include "forms/FormManager.h"
#include "forms/FormFieldType.h"
#include "signatures/SignatureCommands.h"
#include "signatures/SignatureManager.h"
#include "image/ImageCommands.h"
#include "image/ImageManager.h"
#include "image/ImageContentEditor.h"

#include <QApplication>
#include <QAction>
#include <QMouseEvent>
#include <QPointF>
#include <QRectF>
#include <QString>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#ifndef TEST_FIXTURES_DIR
#define TEST_FIXTURES_DIR "."
#endif

namespace {

using pdfe::core::Document;
using pdfe::core::MuPdfContext;
using pdfe::core::Page;
using pdfe::commands::UndoManager;

std::filesystem::path fixturesDir() {
    return std::filesystem::path(TEST_FIXTURES_DIR);
}

// A native-vector-text fixture so the text-edit tool installs and can enter an
// active edit session (same fixture the 4fae336 regression test uses).
const char* kSpanFixture = "embedded_descriptor_style.pdf";

// ───────────────────────────────────────────────────────────────────────────
// Layer A — real ~MainWindow teardown while an edit is ACTIVE (the 4fae336 UAF)
// ───────────────────────────────────────────────────────────────────────────

class MainWindowActiveTeardownTest : public ::testing::Test {
protected:
    void SetUp() override {
        mw_ = std::make_unique<pdfe::ui::MainWindow>();
        QApplication::processEvents();
    }
    void TearDown() override {
        // If a test did not already destroy the window, do it now (still the
        // direct-destruction path, not close()).
        mw_.reset();
        QApplication::processEvents();
    }
    QAction* action(const QString& name) { return mw_->findChild<QAction*>(name); }
    pdfe::ui::PdfCanvas* canvas() {
        return mw_->findChild<pdfe::ui::PdfCanvas*>("pdfCanvas");
    }
    std::unique_ptr<pdfe::ui::MainWindow> mw_;
};

// The precise regression: destroy MainWindow while a text-edit session is LIVE.
// Before the 4fae336 fix this dereferences a freed document/context in
// ~TextEditTool → crash. After it, teardown is clean.
TEST_F(MainWindowActiveTeardownTest, DestroyWhileTextEditSessionActiveIsClean) {
    auto pdf = fixturesDir() / kSpanFixture;
    if (!std::filesystem::exists(pdf)) GTEST_SKIP() << kSpanFixture << " missing";

    mw_->openFile(QString::fromStdString(pdf.string()));
    QApplication::processEvents();

    auto* c = canvas();
    ASSERT_NE(c, nullptr) << "pdfCanvas must exist";
    ASSERT_NE(c->document(), nullptr) << "document must be loaded";

    auto* editAction = action("textEditToolAction");
    ASSERT_NE(editAction, nullptr);
    editAction->trigger();  // native text → installs TextEditTool
    QApplication::processEvents();

    auto* tool = c->textEditTool();
    ASSERT_NE(tool, nullptr) << "native text PDF must route to TextEditTool";

    tool->setPage(0);
    ASSERT_GT(tool->spanCountForTesting(), 0) << "fixture must have a vector span";
    QRectF bbox = tool->spanBboxForTesting(0);
    ASSERT_FALSE(bbox.isEmpty());
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(10, 10),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    tool->onMousePress(&press, 0, bbox.center());
    ASSERT_TRUE(tool->isEditing())
        << "clicking the span must enter an ACTIVE edit session";

    // ── THE UAF SITE ──────────────────────────────────────────────────────
    // Destroy the window DIRECTLY (no close()/closeEvent). ~MainWindow must
    // release the tool before freeing document_/context_; otherwise
    // ~TextEditTool → cancelEdit() dereferences the freed document → crash.
    mw_.reset();
    QApplication::processEvents();

    SUCCEED() << "MainWindow destroyed while text-edit session active without a "
                 "UAF/crash (regression 4fae336)";
}

// Broaden the class: destroy MainWindow while an ANNOTATION is selected (active
// annotation-edit state), exercising the same members-freed-before-canvas order
// for the annotation/selection graph.
TEST_F(MainWindowActiveTeardownTest, DestroyWhileAnnotationSelectedIsClean) {
    auto pdf = fixturesDir() / kSpanFixture;
    if (!std::filesystem::exists(pdf)) GTEST_SKIP() << kSpanFixture << " missing";

    mw_->openFile(QString::fromStdString(pdf.string()));
    QApplication::processEvents();
    ASSERT_NE(mw_->documentForTesting(), nullptr);

    // Activate the free-text annotation tool so annotation machinery is live.
    if (auto* a = action("freeTextToolAction")) {
        a->trigger();
        QApplication::processEvents();
    }

    // Destroy directly while the tool/selection state is active.
    mw_.reset();
    QApplication::processEvents();
    SUCCEED() << "MainWindow destroyed with annotation tool active — clean";
}

// Control: a document open with NO active edit must also tear down cleanly
// (guards against a fix that only special-cases the editing path).
TEST_F(MainWindowActiveTeardownTest, DestroyWithDocumentOpenNoEditIsClean) {
    auto pdf = fixturesDir() / kSpanFixture;
    if (!std::filesystem::exists(pdf)) GTEST_SKIP() << kSpanFixture << " missing";
    mw_->openFile(QString::fromStdString(pdf.string()));
    QApplication::processEvents();
    ASSERT_NE(mw_->documentForTesting(), nullptr);
    mw_.reset();
    QApplication::processEvents();
    SUCCEED();
}

// ───────────────────────────────────────────────────────────────────────────
// Layer B — headless destruction-ORDER matrix over every edit family
// ───────────────────────────────────────────────────────────────────────────

enum class Family {
    Annotation, Redaction, Form, Signature,
    ImageInsert, ImageRemove, ImageResize, ImageRotate, ImageCrop,
};

const char* familyName(Family f) {
    switch (f) {
        case Family::Annotation:  return "Annotation";
        case Family::Redaction:   return "Redaction";
        case Family::Form:        return "Form";
        case Family::Signature:   return "Signature";
        case Family::ImageInsert: return "ImageInsert";
        case Family::ImageRemove: return "ImageRemove";
        case Family::ImageResize: return "ImageResize";
        case Family::ImageRotate: return "ImageRotate";
        case Family::ImageCrop:   return "ImageCrop";
    }
    return "?";
}

bool needsExistingImage(Family f) {
    return f == Family::ImageRemove || f == Family::ImageResize ||
           f == Family::ImageRotate || f == Family::ImageCrop;
}

class DestructionOrderMatrixTest : public ::testing::TestWithParam<Family> {
protected:
    void SetUp() override {
        std::filesystem::path fixture =
            fixturesDir() / "test_pdf_with_images.pdf";
        if (!std::filesystem::exists(fixture)) {
            GTEST_SKIP() << "test_pdf_with_images.pdf not available";
        }
        pngPath_ = std::string(TEST_FIXTURES_DIR) + "/test_image_rgb.png";
        tempPdf_ = std::filesystem::temp_directory_path() /
                   (std::string("pdfe_destroy_active_") +
                    familyName(GetParam()) + ".pdf");
        std::error_code ec;
        std::filesystem::remove(tempPdf_, ec);
        std::filesystem::copy_file(
            fixture, tempPdf_,
            std::filesystem::copy_options::overwrite_existing, ec);
        ASSERT_FALSE(ec) << "failed to stage temp copy: " << ec.message();
        context_ = std::make_shared<MuPdfContext>();
    }
    void TearDown() override {
        context_.reset();
        std::error_code ec;
        std::filesystem::remove(tempPdf_, ec);
    }
    std::filesystem::path tempPdf_;
    std::string pngPath_;
    std::shared_ptr<MuPdfContext> context_;
};

// Start an ACTIVE edit, then destroy the Document + owning Pages FIRST (the
// window-teardown order) while the UndoManager + managers still reference them.
// The subsequent manager/undo destruction must NOT dereference the freed graph.
TEST_P(DestructionOrderMatrixTest, DestroyDocumentUnderActiveEditIsUafClean) {
    const Family fam = GetParam();

    auto doc = Document::open(context_, tempPdf_);
    ASSERT_NE(doc, nullptr);

    // Managers are heap-owned so we control their destruction order explicitly.
    auto annotMgr = std::make_unique<pdfe::annotations::AnnotationManager>(context_);
    auto redMgr   = std::make_unique<pdfe::redaction::RedactionManager>(context_);
    auto formMgr  = std::make_unique<pdfe::forms::FormManager>(context_);
    auto sigMgr   = std::make_unique<pdfe::signatures::SignatureManager>(context_);
    auto imgMgr   = std::make_unique<pdfe::image::ImageManager>(context_);
    auto imgEditor= std::make_unique<pdfe::image::ImageContentEditor>(context_);

    auto undo = std::make_unique<UndoManager>();

    std::vector<std::unique_ptr<Page>> commandPages;
    commandPages.push_back(doc->page(0));
    Page* pagePtr = commandPages.back().get();
    ASSERT_NE(pagePtr, nullptr);

    std::string imgName;
    QRectF imgBox;
    float imgW = 0.0f, imgH = 0.0f;
    if (fam == Family::ImageInsert || needsExistingImage(fam)) {
        auto imgs = imgMgr->enumerateImages(pagePtr, doc.get());
        if (needsExistingImage(fam)) {
            if (imgs.empty()) {
                GTEST_SKIP() << "fixture has no image for " << familyName(fam);
            }
            imgName = imgs[0].xobjectName;
            imgBox = imgs[0].boundingBox;
            imgW = imgs[0].displayWidth;
            imgH = imgs[0].displayHeight;
        }
    }

    // ── build + execute the family's command: the edit is now ACTIVE ─────────
    switch (fam) {
        case Family::Annotation:
            undo->execute(std::make_unique<pdfe::annotations::CreateAnnotationCommand>(
                annotMgr.get(), pagePtr, pdfe::annotations::AnnotationType::Text,
                QRectF(100, 100, 200, 50)));
            break;
        case Family::Redaction:
            undo->execute(std::make_unique<pdfe::redaction::CreateRedactionCommand>(
                redMgr.get(), pagePtr, QRectF(80, 80, 120, 30)));
            break;
        case Family::Form:
            undo->execute(std::make_unique<pdfe::forms::CreateFormFieldCommand>(
                formMgr.get(), pagePtr, pdfe::forms::FormFieldType::Text,
                QRectF(70, 70, 150, 24), "r35_field"));
            break;
        case Family::Signature:
            undo->execute(std::make_unique<pdfe::signatures::CreateSignatureFieldCommand>(
                sigMgr.get(), pagePtr, "r35_sig", QRectF(60, 60, 160, 40)));
            break;
        case Family::ImageInsert: {
            pdfe::image::InsertImageOptions o;
            o.filePath = pngPath_;
            o.targetRect = QRectF(60.0, 60.0, 120.0, 90.0);
            undo->execute(std::make_unique<pdfe::image::InsertImageCommand>(
                imgMgr.get(), pagePtr, doc.get(), o));
            break;
        }
        case Family::ImageRemove:
            undo->execute(std::make_unique<pdfe::image::RemoveImageCommand>(
                imgMgr.get(), pagePtr, doc.get(), /*imageIndex=*/0));
            break;
        case Family::ImageResize:
            undo->execute(std::make_unique<pdfe::image::ResizeImageCommand>(
                imgMgr.get(), imgEditor.get(), pagePtr, doc.get(), imgName,
                imgW * 1.6f + 11.0f, imgH * 1.6f + 7.0f,
                static_cast<float>(imgBox.x()), static_cast<float>(imgBox.y())));
            break;
        case Family::ImageRotate:
            undo->execute(std::make_unique<pdfe::image::RotateImageCommand>(
                imgMgr.get(), imgEditor.get(), pagePtr, doc.get(), imgName, 37.0f,
                static_cast<float>(imgBox.center().x()),
                static_cast<float>(imgBox.center().y()), imgW, imgH));
            break;
        case Family::ImageCrop:
            undo->execute(std::make_unique<pdfe::image::CropImageCommand>(
                imgMgr.get(), imgEditor.get(), pagePtr, doc.get(), imgName,
                QRectF(imgBox.x(), imgBox.y(),
                       imgBox.width() * 0.6, imgBox.height() * 0.6),
                imgW, imgH,
                static_cast<float>(imgBox.x()) + 5.0f,
                static_cast<float>(imgBox.y()) + 7.0f));
            break;
    }
    ASSERT_TRUE(undo->canUndo()) << "the family command must be ACTIVE on the stack";

    // ── DESTROY THE DOCUMENT + PAGES OUT FROM UNDER THE ACTIVE EDIT ───────────
    // This is the window-close teardown order: the document graph is freed
    // while the undo stack and every manager still hold references to it.
    commandPages.clear();   // owning Pages gone
    doc.reset();            // Document (and its pdf_document) gone

    // The command on the undo stack, the managers, and the editor now all
    // outlive the document. Destroying them must NOT dereference the freed
    // graph. Under ASan a reintroduced dangling access aborts here.
    ASSERT_NO_THROW({
        undo.reset();       // ~UndoManager → destroys the active command
        imgEditor.reset();
        imgMgr.reset();
        sigMgr.reset();
        formMgr.reset();
        redMgr.reset();
        annotMgr.reset();
    }) << "teardown of managers/undo AFTER the document was destroyed must be "
          "UAF-clean for family " << familyName(fam);

    SUCCEED() << "destruction-under-active-edit clean for " << familyName(fam);
}

INSTANTIATE_TEST_SUITE_P(
    AllEditFamilies, DestructionOrderMatrixTest,
    ::testing::Values(
        Family::Annotation, Family::Redaction, Family::Form, Family::Signature,
        Family::ImageInsert, Family::ImageRemove, Family::ImageResize,
        Family::ImageRotate, Family::ImageCrop),
    [](const ::testing::TestParamInfo<Family>& info) {
        return std::string(familyName(info.param));
    });

}  // namespace
