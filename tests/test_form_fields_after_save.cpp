// -*- coding: utf-8 -*-
//
// Form fields must stay fillable after a save, and after the file is opened
// again (user report, 2026-11-26: "saving a form renders the fields
// inoperative (text entered when they were created is saved but cannot be
// changed when the file is opened again)"). DCA 0128.
//
// TWO DEFECTS BEHIND ONE SYMPTOM
// ------------------------------
// 1. After File > Save, MainWindow::onDocumentStructureChanged calls
//    PdfCanvas::releaseActiveToolPageCache(). That nulled the active tool's
//    page_ (AnnotationTool::onDocumentClosing) but left the canvas's own
//    toolPage_/toolPageIndex_ binding in place, so the next click's
//    bindToolToPage() took its "already bound to this page" short-circuit and
//    never handed the tool a page again. FormTool::onMousePress then returned
//    at `if (!page_)`: every click on a field was dropped until the user
//    switched tools. Every tool that returns early on a null page could drop
//    presses the same way; only the Form Tool is tested for it here.
// 2. After reopening, the Select tool is active (it is installed on open), and
//    the Select tool never filled a field: a press on a field selected the
//    widget as an annotation ("Type: None" in the properties panel). Only the
//    Form Tool could fill, and nothing said so.
//
// The canvas-level test reproduces (1) without a MainWindow. The window-level
// tests drive the real MainWindow, its real Save action and the canvas click
// seam, at positions read from the fields' own rectangles.
//
// MUTATIONS KILLED (each applied, pdfe_tests rebuilt, FormFieldsAfterSave*
// run; 2026-11-26, Linux Release):
//   * releaseToolPage() removed from PdfCanvas::releaseActiveToolPageCache()
//     -> 2 fail: CanvasToolStillEditsAfterSaveReleasesTheToolPage,
//     FormToolStillFillsAFieldAfterFileSave.
//   * MainWindow's formFieldFillRequested callback returns false
//     -> 2 fail: SelectToolFillsATextFieldAfterReopen,
//     SelectToolTogglesACheckboxAfterReopen.
//   * switchToFormFillAt() leaves Text-field creation armed
//     -> 1 fails: SelectToolFillsATextFieldAfterReopen.
//   * switchToFormFillAt() also claims Signature and PushButton fields
//     -> 2 fail: SignatureFieldKeepsTheSelectTool, PushButtonKeepsTheSelectTool.
//     (Before the modal watchdog in expectSelectToolKeeps, the signature case
//     HUNG in the signing dialog's exec() instead of failing.)
//   * switchToFormFillAt() without its read-only check
//     -> 1 fails: ReadOnlyTextFieldKeepsTheSelectTool.
//   * switchToFormFillAt() leaves the field-creation toolbar visible (it moved
//     the page under the mouse; found by the phase-4 GUI check)
//     -> 1 fails: SelectToolFillsATextFieldAfterReopen. (An isVisible() first
//     version of that assertion let this survive: offscreen nothing is visible.)

#include <gtest/gtest.h>

#include "annotations/SelectTool.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "core/UndoManager.h"
#include "forms/FormFieldType.h"
#include "forms/FormManager.h"
#include "forms/FormTool.h"
#include "ui/MainWindow.h"
#include "ui/PdfCanvas.h"

#include <QAction>
#include <QApplication>
#include <QKeyEvent>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QToolBar>
#include <QTimer>

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

using namespace pdfe;

namespace {

std::filesystem::path fixture(const char* name)
{
    return std::filesystem::path(TEST_FIXTURES_DIR) / name;
}

void settle()
{
    for (int i = 0; i < 5; ++i) QApplication::processEvents();
}

// The first field of @p type on page 0 of the file at @p path, read through a
// FormManager of its own: the click position comes from the field's rectangle,
// never from a guessed canvas offset.
std::optional<forms::FormFieldData> firstField(const std::filesystem::path& path,
                                               forms::FormFieldType type)
{
    auto ctx = std::make_shared<core::MuPdfContext>();
    auto doc = core::Document::open(ctx, path);
    if (!doc) return std::nullopt;
    forms::FormManager mgr(ctx);
    auto page = doc->page(0);
    for (const auto& f : mgr.getFormFields(page.get()))
        if (f.type == type && !f.isReadOnly) return f;
    return std::nullopt;
}

// Types @p text into whatever has keyboard focus on @p canvas.
void typeInto(QWidget* canvas, const QString& text)
{
    for (const QChar c : text) {
        QKeyEvent press(QEvent::KeyPress, 0, Qt::NoModifier, QString(c));
        QApplication::sendEvent(canvas, &press);
        QKeyEvent release(QEvent::KeyRelease, 0, Qt::NoModifier, QString(c));
        QApplication::sendEvent(canvas, &release);
    }
}

// The first field of @p type on page 0, read-only or not.
std::optional<forms::FormFieldData> firstFieldOfType(const std::filesystem::path& path,
                                                     forms::FormFieldType type)
{
    auto ctx = std::make_shared<core::MuPdfContext>();
    auto doc = core::Document::open(ctx, path);
    if (!doc) return std::nullopt;
    forms::FormManager mgr(ctx);
    auto page = doc->page(0);
    for (const auto& f : mgr.getFormFields(page.get()))
        if (f.type == type) return f;
    return std::nullopt;
}

// Sets the read-only flag (/Ff bit 1) on the first text widget on page 0 of
// @p path, in place, with an independent MuPDF context. PDFE has no API that
// makes a field read-only, so the test writes the flag the way a form author's
// tool would. Returns false if there was no text widget or the save failed.
bool makeFirstTextFieldReadOnly(const std::filesystem::path& path)
{
    bool done = false;
    fz_context* ctx = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
    if (!ctx) return false;
    pdf_document* doc = nullptr;
    pdf_page* page = nullptr;
    fz_try(ctx) {
        fz_register_document_handlers(ctx);
        doc = pdf_open_document(ctx, path.string().c_str());
        page = pdf_load_page(ctx, doc, 0);
        for (pdf_annot* w = pdf_first_widget(ctx, page); w; w = pdf_next_widget(ctx, w)) {
            if (pdf_widget_type(ctx, w) != PDF_WIDGET_TYPE_TEXT) continue;
            pdf_obj* obj = pdf_annot_obj(ctx, w);
            const int ff = pdf_field_flags(ctx, obj);
            pdf_dict_put_int(ctx, obj, PDF_NAME(Ff), ff | PDF_FIELD_IS_READ_ONLY);
            done = true;
            break;
        }
        if (done) {
            const std::string tmp = path.string() + ".ro.pdf";
            pdf_save_document(ctx, doc, tmp.c_str(), nullptr);
            fz_drop_page(ctx, reinterpret_cast<fz_page*>(page));
            page = nullptr;
            pdf_drop_document(ctx, doc);
            doc = nullptr;
            std::filesystem::rename(tmp, path);
        }
    }
    fz_always(ctx) {
        fz_drop_page(ctx, reinterpret_cast<fz_page*>(page));
        pdf_drop_document(ctx, doc);
    }
    fz_catch(ctx) {
        done = false;
    }
    fz_drop_context(ctx);
    return done;
}

forms::FormTool* activeFormTool(ui::PdfCanvas* canvas)
{
    return dynamic_cast<forms::FormTool*>(canvas->currentTool());
}

}  // namespace

// ---------------------------------------------------------------------------
// Defect 1 at the canvas level.
// ---------------------------------------------------------------------------

TEST(FormFieldsAfterSave, CanvasToolStillEditsAfterSaveReleasesTheToolPage)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const auto path = std::filesystem::path(tmp.path().toStdString()) / "form.pdf";
    std::filesystem::copy_file(fixture("test_single.pdf"), path);

    auto ctx = std::make_shared<core::MuPdfContext>();
    auto doc = core::Document::open(ctx, path);
    ASSERT_NE(doc, nullptr);

    forms::FormManager mgr(ctx);
    commands::UndoManager undo;
    const QRectF rect(100, 100, 200, 30);
    {
        auto page = doc->page(0);
        int idx = mgr.createFormField(page.get(), forms::FormFieldType::Text, rect, "F1");
        ASSERT_GE(idx, 0);
        mgr.setFieldValue(page.get(), idx, "first");
    }

    ui::PdfCanvas canvas;
    canvas.resize(900, 1200);
    canvas.setDocument(doc.get(), nullptr, nullptr);

    auto tool = std::make_unique<forms::FormTool>(&mgr, nullptr, &undo);
    forms::FormTool* ft = tool.get();
    canvas.setCurrentTool(std::move(tool));

    const QPointF onField = rect.center();
    const QPointF offField(500, 700);

    ASSERT_TRUE(canvas.clickByPdfCoord(0, onField.x(), onField.y()));
    EXPECT_TRUE(ft->isEditingField()) << "a click on the field before the save must start an edit";
    ASSERT_TRUE(canvas.clickByPdfCoord(0, offField.x(), offField.y()));
    EXPECT_FALSE(ft->isEditingField());

    // What MainWindow::onDocumentStructureChanged does after File > Save.
    canvas.releaseActiveToolPageCache();

    ASSERT_TRUE(canvas.clickByPdfCoord(0, onField.x(), onField.y()));
    EXPECT_TRUE(ft->isEditingField())
        << "after releaseActiveToolPageCache() the click on the field was dropped: the "
           "tool's page was released but the canvas kept its page binding, so "
           "bindToolToPage() never handed the tool a page again";
}

// ---------------------------------------------------------------------------
// The same through the real MainWindow, Form Tool and Save action.
// ---------------------------------------------------------------------------

class FormFieldsAfterSaveWindow : public ::testing::Test {
protected:
    void SetUp() override
    {
        if (!qApp) GTEST_SKIP() << "QApplication not available";
        ASSERT_TRUE(tmp_.isValid());
        work_ = std::filesystem::path(tmp_.path().toStdString()) / "form.pdf";
        std::filesystem::copy_file(fixture("test_form.pdf"), work_);
        text_ = firstField(work_, forms::FormFieldType::Text);
        ASSERT_TRUE(text_.has_value()) << "test_form.pdf has no editable text field";
    }

    std::unique_ptr<ui::MainWindow> open()
    {
        auto mw = std::make_unique<ui::MainWindow>();
        mw->openFile(QString::fromStdString(work_.string()));
        settle();
        return mw;
    }

    static void save(ui::MainWindow& mw)
    {
        auto* save = mw.findChild<QAction*>(QStringLiteral("saveAction"));
        ASSERT_NE(save, nullptr);
        save->trigger();
        settle();
    }

    static void click(ui::MainWindow& mw, const QPointF& p)
    {
        ASSERT_TRUE(mw.canvasForTesting()->clickByPdfCoord(0, p.x(), p.y()));
        settle();
    }

    QTemporaryDir tmp_;
    std::filesystem::path work_;
    std::optional<forms::FormFieldData> text_;
};

TEST_F(FormFieldsAfterSaveWindow, FormToolStillFillsAFieldAfterFileSave)
{
    auto mw = open();
    ASSERT_NE(mw->documentForTesting(), nullptr);
    ASSERT_TRUE(mw->activateToolByObjectName(QStringLiteral("formToolAction")));
    settle();
    ui::PdfCanvas* canvas = mw->canvasForTesting();
    forms::FormTool* ft = activeFormTool(canvas);
    ASSERT_NE(ft, nullptr) << "the Form Tool is not the canvas's tool";

    click(*mw, text_->rect.center());
    ASSERT_TRUE(ft->isEditingField()) << "a click on the field before the save must start an edit";
    typeInto(canvas, QStringLiteral("abc"));
    click(*mw, QPointF(text_->rect.right() + 150, text_->rect.bottom() + 150));  // commit

    save(*mw);

    ft = activeFormTool(canvas);
    ASSERT_NE(ft, nullptr) << "the save replaced the Form Tool";
    click(*mw, text_->rect.center());
    EXPECT_TRUE(ft->isEditingField())
        << "after File > Save a click on the field no longer starts an edit";
}

// ---------------------------------------------------------------------------
// Defect 2: after reopening, the Select tool (the tool a reopened document
// starts with) fills fields.
// ---------------------------------------------------------------------------

TEST_F(FormFieldsAfterSaveWindow, SelectToolFillsATextFieldAfterReopen)
{
    {
        // Give the field a saved value first, through the model, and save it:
        // the state the user reopens.
        auto ctx = std::make_shared<core::MuPdfContext>();
        auto doc = core::Document::open(ctx, work_);
        ASSERT_NE(doc, nullptr);
        forms::FormManager mgr(ctx);
        auto page = doc->page(0);
        auto at = mgr.getFormFieldAt(page.get(), text_->rect.center());
        ASSERT_TRUE(at.has_value());
        mgr.setFieldValue(page.get(), *at, "saved");
        ASSERT_NO_THROW(doc->save(work_));
    }
    // Precondition: the value really is in the saved file, so the edit below
    // is filling a saved value, not one that only existed in memory.
    {
        const auto saved = firstField(work_, forms::FormFieldType::Text);
        ASSERT_TRUE(saved.has_value());
        ASSERT_EQ(saved->value, "saved") << "the setup save did not write the value";
    }

    auto mw = open();
    ASSERT_NE(mw->documentForTesting(), nullptr);
    ui::PdfCanvas* canvas = mw->canvasForTesting();
    ASSERT_NE(dynamic_cast<annotations::SelectTool*>(canvas->currentTool()), nullptr)
        << "precondition: a freshly opened document starts on the Select tool";

    click(*mw, text_->rect.center());
    forms::FormTool* ft = activeFormTool(canvas);
    ASSERT_NE(ft, nullptr) << "a Select-tool click on a text field did not reach the form editor";
    EXPECT_TRUE(ft->isEditingField()) << "a Select-tool click on a text field did not start an edit";
    EXPECT_FALSE(ft->isCreationMode())
        << "filling a field must not arm field creation: the next click on empty "
           "page space would draw a new field";
    ASSERT_NE(mw->formToolBarForTesting(), nullptr);
    // isHidden(), not isVisible(): offscreen the window itself is never shown,
    // so isVisible() is false for every child and would prove nothing.
    EXPECT_TRUE(mw->formToolBarForTesting()->isHidden())
        << "the field-creation toolbar appeared and moved the page under the mouse";

    // The edit holds the saved value and takes typing.
    typeInto(canvas, QStringLiteral("X"));
    EXPECT_NE(ft->editBufferForTesting().find("saved"), std::string::npos)
        << "edit buffer: '" << ft->editBufferForTesting() << "'";
    EXPECT_NE(ft->editBufferForTesting().find('X'), std::string::npos)
        << "typing did not reach the field";
}

TEST_F(FormFieldsAfterSaveWindow, SelectToolTogglesACheckboxAfterReopen)
{
    const auto box = firstField(work_, forms::FormFieldType::Checkbox);
    ASSERT_TRUE(box.has_value()) << "test_form.pdf has no editable checkbox";

    auto mw = open();
    ASSERT_NE(mw->documentForTesting(), nullptr);
    ui::PdfCanvas* canvas = mw->canvasForTesting();
    ASSERT_NE(dynamic_cast<annotations::SelectTool*>(canvas->currentTool()), nullptr);

    click(*mw, box->rect.center());

    // Read the value back from the live document, the way MainWindow sees it.
    forms::FormManager mgr(mw->documentForTesting()->context());
    auto page = mw->documentForTesting()->page(0);
    auto at = mgr.getFormFieldAt(page.get(), box->rect.center());
    ASSERT_TRUE(at.has_value());
    EXPECT_NE(mgr.getFormField(page.get(), *at).value, box->value)
        << "a Select-tool click on a checkbox did not toggle it (value still '"
        << box->value << "')";
}

TEST_F(FormFieldsAfterSaveWindow, SelectToolStillSelectsOrdinaryAnnotationsAndEmptySpace)
{
    // The routing claims fillable fields only. A click on empty page space
    // keeps the Select tool, so selection and rubber-banding still work.
    auto mw = open();
    ASSERT_NE(mw->documentForTesting(), nullptr);
    ui::PdfCanvas* canvas = mw->canvasForTesting();
    ASSERT_NE(dynamic_cast<annotations::SelectTool*>(canvas->currentTool()), nullptr);

    auto info = mw->documentForTesting()->pageInfo(0);
    // A point well away from every field on the page.
    QPointF empty(5, 5);
    {
        forms::FormManager mgr(mw->documentForTesting()->context());
        auto page = mw->documentForTesting()->page(0);
        for (QPointF p : {QPointF(5, 5), QPointF(info.widthPt - 5, 5),
                          QPointF(5, info.heightPt - 5), QPointF(info.widthPt - 5, info.heightPt - 5)}) {
            if (!mgr.getFormFieldAt(page.get(), p).has_value()) { empty = p; break; }
        }
        ASSERT_FALSE(mgr.getFormFieldAt(page.get(), empty).has_value());
    }
    click(*mw, empty);
    EXPECT_NE(dynamic_cast<annotations::SelectTool*>(canvas->currentTool()), nullptr)
        << "a click on empty page space switched away from the Select tool";
}

// ---------------------------------------------------------------------------
// What the Select-tool routing must NOT claim. Signatures go to the signing
// flow, push buttons run their actions, and read-only fields cannot be filled;
// for each of these a Select-tool click keeps the Select tool and changes
// nothing in the field.
// ---------------------------------------------------------------------------

namespace {

void expectSelectToolKeeps(const std::filesystem::path& file, const forms::FormFieldData& field,
                           const char* what)
{
    ui::MainWindow mw;
    mw.openFile(QString::fromStdString(file.string()));
    settle();
    ASSERT_NE(mw.documentForTesting(), nullptr) << what;
    ui::PdfCanvas* canvas = mw.canvasForTesting();
    ASSERT_NE(dynamic_cast<annotations::SelectTool*>(canvas->currentTool()), nullptr) << what;

    // A wrongly routed signature click opens the modal signing dialog from a
    // deferred call; close any modal that appears, and count it, so the test
    // fails instead of hanging in its exec() loop.
    int modalsOpened = 0;
    QTimer watchdog;
    watchdog.setInterval(20);
    QObject::connect(&watchdog, &QTimer::timeout, [&modalsOpened]() {
        if (QWidget* modal = QApplication::activeModalWidget()) {
            ++modalsOpened;
            modal->close();
        }
    });
    watchdog.start();

    ASSERT_TRUE(canvas->clickByPdfCoord(0, field.rect.center().x(), field.rect.center().y()));
    settle();
    QElapsedTimer waited;
    waited.start();
    while (waited.elapsed() < 300) QApplication::processEvents(QEventLoop::AllEvents, 20);
    watchdog.stop();

    EXPECT_EQ(modalsOpened, 0) << what << ": the click opened a modal dialog";

    EXPECT_NE(dynamic_cast<annotations::SelectTool*>(canvas->currentTool()), nullptr)
        << what << ": a Select-tool click switched to the form editor";
    EXPECT_EQ(dynamic_cast<forms::FormTool*>(canvas->currentTool()), nullptr) << what;

    forms::FormManager mgr(mw.documentForTesting()->context());
    auto page = mw.documentForTesting()->page(0);
    auto at = mgr.getFormFieldAt(page.get(), field.rect.center());
    ASSERT_TRUE(at.has_value()) << what;
    EXPECT_EQ(mgr.getFormField(page.get(), *at).value, field.value)
        << what << ": the click changed the field's value";
}

}  // namespace

TEST(FormFieldsSelectRouting, SignatureFieldKeepsTheSelectTool)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const auto work = std::filesystem::path(tmp.path().toStdString()) / "all.pdf";
    std::filesystem::copy_file(fixture("phase3_form_all_field_types.pdf"), work);
    const auto sig = firstFieldOfType(work, forms::FormFieldType::Signature);
    ASSERT_TRUE(sig.has_value()) << "phase3_form_all_field_types.pdf has no signature field";
    expectSelectToolKeeps(work, *sig, "signature field");
}

TEST(FormFieldsSelectRouting, PushButtonKeepsTheSelectTool)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const auto work = std::filesystem::path(tmp.path().toStdString()) / "button.pdf";
    std::filesystem::copy_file(fixture("test_form_with_pushbutton.pdf"), work);
    const auto button = firstFieldOfType(work, forms::FormFieldType::PushButton);
    ASSERT_TRUE(button.has_value()) << "test_form_with_pushbutton.pdf has no push button";
    expectSelectToolKeeps(work, *button, "push button");
}

TEST(FormFieldsSelectRouting, ReadOnlyTextFieldKeepsTheSelectTool)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const auto work = std::filesystem::path(tmp.path().toStdString()) / "ro.pdf";
    std::filesystem::copy_file(fixture("test_form.pdf"), work);
    ASSERT_TRUE(makeFirstTextFieldReadOnly(work));
    const auto text = firstFieldOfType(work, forms::FormFieldType::Text);
    ASSERT_TRUE(text.has_value());
    ASSERT_TRUE(text->isReadOnly) << "the read-only flag did not take: the test would prove nothing";
    expectSelectToolKeeps(work, *text, "read-only text field");
}
