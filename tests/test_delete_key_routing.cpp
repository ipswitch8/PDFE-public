// -*- coding: utf-8 -*-
//
// tests/test_delete_key_routing.cpp
//
// ui-tweaks-2 phase-3: pressing Del deleted the PAGE even when an image, an
// annotation, a text box or a form field was selected, or while text was being
// edited.
//
// Cause: deletePageAction ("Delete Selected Pages") owns QKeySequence::Delete
// as a window shortcut, and nothing on the canvas claimed the key through
// QEvent::ShortcutOverride, so Qt's shortcut map ran onDeletePages and the
// KeyPress never reached PdfCanvas::keyPressEvent. Every tool's Key_Delete
// branch was dead code for Del.
//
// Fix: PdfCanvas::event accepts the ShortcutOverride for a bare
// Delete/Backspace when PdfCanvas::wantsDeleteKey() (text being edited, an
// image selected, or AnnotationTool::wantsDeleteKey() on the active tool).
// The UndoFallthroughFilter pattern, in the claiming direction.
//
// Three layers, each able to fail on its own:
//   DeleteKeyDecision        -- the pure rule (PdfCanvas::claimsDeleteShortcut)
//                               and the per-tool wantsDeleteKey() overrides.
//   DeleteKeyShortcutOverride -- a QEvent::ShortcutOverride sent to the live
//                               canvas inside a real MainWindow: accepted in
//                               every claimed state, NOT accepted when idle.
//   DeleteKeyEndToEnd        -- QTest::keyClick(Qt::Key_Delete) on the focused
//                               widget of an active window, so QTest runs
//                               qt_sendShortcutOverrideEvent and the real
//                               QShortcutMap. Each asserts the page count, the
//                               deletePageAction trigger count (QSignalSpy) and
//                               what was actually deleted.
//
// A modal watcher answers every QMessageBox (No by default) so a regression --
// the page shortcut firing -- fails on the assertions instead of hanging on
// onDeletePages's confirmation box.
//
// Every assertion runs unconditionally: no DISABLED_, GTEST_SKIP or
// expected-fail marker.

#include <gtest/gtest.h>

#include "annotations/AnnotationData.h"
#include "annotations/AnnotationManager.h"
#include "annotations/AnnotationType.h"
#include "annotations/SelectTool.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "core/UndoManager.h"
#include "forms/FormFieldType.h"
#include "forms/FormManager.h"
#include "forms/FormTool.h"
#include "image/ImageTool.h"
#include "textedit/TextEditTool.h"
#include "ui/MainWindow.h"
#include "ui/PagePanel.h"
#include "ui/PdfCanvas.h"
#include "ui/TestDialogSeam.h"

#include <QAction>
#include <QApplication>
#include <QColor>
#include <QComboBox>
#include <QDockWidget>
#include <QCoreApplication>
#include <QImage>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPushButton>
#include <QSignalSpy>
#include <QStringList>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#ifndef TEST_FIXTURES_DIR
#error "TEST_FIXTURES_DIR must be defined by the test target"
#endif

using pdfe::annotations::AnnotationManager;
using pdfe::annotations::AnnotationType;
using pdfe::annotations::SelectTool;
using pdfe::forms::FormFieldType;
using pdfe::forms::FormManager;
using pdfe::forms::FormTool;
using pdfe::ui::MainWindow;
using pdfe::ui::PdfCanvas;

namespace {

void settle()
{
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();
}

std::filesystem::path fixture(const char* name)
{
    return std::filesystem::path(TEST_FIXTURES_DIR) / name;
}

QKeyEvent overrideEvent(int key, Qt::KeyboardModifiers mods = Qt::NoModifier)
{
    // A fresh ShortcutOverride starts IGNORED (QKeyEvent's constructor does
    // that for this type), exactly as qt_sendShortcutOverrideEvent builds it.
    return QKeyEvent(QEvent::ShortcutOverride, key, mods);
}

// Answers every QMessageBox that becomes the active modal with @p answer and
// records its text. Polls on a timer, so it also runs inside the nested event
// loop QMessageBox::question() spins.
class ModalWatcher : public QObject {
public:
    explicit ModalWatcher(QMessageBox::StandardButton answer)
        : answer_(answer)
    {
        setObjectName(QStringLiteral("deleteKeyRoutingModalWatcher"));
        timer_.setObjectName(QStringLiteral("deleteKeyRoutingModalWatcherTimer"));
        timer_.setInterval(10);
        QObject::connect(&timer_, &QTimer::timeout, this, [this] { tick(); });
        timer_.start();
    }
    void setAnswer(QMessageBox::StandardButton a) { answer_ = a; }
    int answered() const { return static_cast<int>(texts_.size()); }
    const QStringList& texts() const { return texts_; }

private:
    void tick()
    {
        QWidget* modal = QApplication::activeModalWidget();
        if (!modal || modal == last_) return;
        if (auto* box = qobject_cast<QMessageBox*>(modal)) {
            last_ = modal;
            texts_ << box->text();
            if (auto* btn = box->button(answer_)) {
                btn->click();
            } else {
                box->reject();
            }
            return;
        }
        last_ = modal;
        texts_ << QStringLiteral("<non-QMessageBox modal: %1>").arg(modal->objectName());
        modal->close();
    }

    QTimer timer_;
    QMessageBox::StandardButton answer_;
    QWidget* last_ = nullptr;
    QStringList texts_;
};

// Insert Image's options dialog answered with a fixed placement.
class PlacingWindow : public MainWindow {
protected:
    std::optional<InsertImageChoice> promptInsertImageOptions(const QString&, QSize,
                                                              QSizeF) override
    {
        InsertImageChoice c;
        c.targetRect = QRectF(150.0, 200.0, 200.0, 120.0);
        c.jpegQuality = 90;
        c.stripExif = true;
        c.maintainAspectRatio = false;
        return c;
    }
};

// ═══════════════════════════════════════════════════════════════════════════
// DeleteKeyDecision -- the rule, without a window
// ═══════════════════════════════════════════════════════════════════════════

TEST(DeleteKeyDecision, ClaimsBareDeleteAndBackspaceOnlyWhenTheCanvasWantsThem)
{
    for (int key : {int(Qt::Key_Delete), int(Qt::Key_Backspace)}) {
        QKeyEvent bare = overrideEvent(key);
        EXPECT_TRUE(PdfCanvas::claimsDeleteShortcut(&bare, true)) << "key " << key;
        EXPECT_FALSE(PdfCanvas::claimsDeleteShortcut(&bare, false))
            << "nothing to delete on the canvas: the key must stay with the "
               "page shortcut (key " << key << ")";
        QKeyEvent keypad = overrideEvent(key, Qt::KeypadModifier);
        EXPECT_TRUE(PdfCanvas::claimsDeleteShortcut(&keypad, true))
            << "keypad Del is the same key to the user (key " << key << ")";
    }
}

TEST(DeleteKeyDecision, LeavesModifiedChordsAndOtherKeysAlone)
{
    for (Qt::KeyboardModifiers m : {Qt::KeyboardModifiers(Qt::ShiftModifier),
                                    Qt::KeyboardModifiers(Qt::ControlModifier),
                                    Qt::KeyboardModifiers(Qt::AltModifier)}) {
        QKeyEvent ev = overrideEvent(Qt::Key_Delete, m);
        EXPECT_FALSE(PdfCanvas::claimsDeleteShortcut(&ev, true))
            << "a modified Delete (e.g. Shift+Del = Cut) is another shortcut's";
    }
    for (int key : {int(Qt::Key_A), int(Qt::Key_Z), int(Qt::Key_Escape), int(Qt::Key_PageDown)}) {
        QKeyEvent ev = overrideEvent(key);
        EXPECT_FALSE(PdfCanvas::claimsDeleteShortcut(&ev, true)) << "key " << key;
    }
    EXPECT_FALSE(PdfCanvas::claimsDeleteShortcut(nullptr, true));
}

// The per-tool overrides, on a real document page.
class DeleteKeyToolDecision : public ::testing::Test {
protected:
    void SetUp() override
    {
        ctx_ = std::make_shared<pdfe::core::MuPdfContext>();
        doc_ = pdfe::core::Document::open(ctx_, fixture("test_3page.pdf"));
        ASSERT_NE(doc_, nullptr);
        page_ = doc_->page(0);
        ASSERT_NE(page_, nullptr);
    }
    std::shared_ptr<pdfe::core::MuPdfContext> ctx_;
    std::unique_ptr<pdfe::core::Document> doc_;
    std::unique_ptr<pdfe::core::Page> page_;
};

TEST_F(DeleteKeyToolDecision, SelectToolWantsDeleteExactlyWhileAnAnnotationIsSelected)
{
    AnnotationManager mgr(ctx_);
    mgr.createAnnotation(page_.get(), AnnotationType::Square, QRectF(60, 60, 80, 40));
    SelectTool tool(&mgr);
    tool.setPage(page_.get());
    EXPECT_FALSE(tool.wantsDeleteKey()) << "nothing selected yet";
    tool.selectByIndex(0, 0);
    ASSERT_EQ(tool.selectedAnnotIndex(), 0);
    EXPECT_TRUE(tool.wantsDeleteKey());
    tool.onAnnotationsChanged();  // drops the selection
    EXPECT_FALSE(tool.wantsDeleteKey());
}

TEST_F(DeleteKeyToolDecision, FormToolWantsDeleteWhileAFieldIsSelectedOrBeingTyped)
{
    FormManager mgr(doc_->context());
    const QRectF rect(72, 72, 180, 30);
    ASSERT_GE(mgr.createFormField(page_.get(), FormFieldType::Text, rect, "DelDecision"), 0);
    int idx = -1;
    for (const auto& f : mgr.getFormFields(page_.get()))
        if (f.fieldName == "DelDecision") idx = f.widgetIndex;
    ASSERT_GE(idx, 0);

    pdfe::commands::UndoManager undo;
    FormTool tool(&mgr, nullptr, &undo);
    tool.setPage(page_.get());
    tool.setCreationFieldType(FormFieldType::Unknown);
    EXPECT_FALSE(tool.wantsDeleteKey()) << "no field selected";

    QMouseEvent click(QEvent::MouseButtonPress, QPointF(0, 0), QPointF(0, 0), Qt::LeftButton,
                      Qt::LeftButton, Qt::NoModifier);
    tool.onMousePress(&click, 0, mgr.getFormField(page_.get(), idx).rect.center());
    ASSERT_TRUE(tool.isEditingField());
    EXPECT_TRUE(tool.wantsDeleteKey()) << "typing into a field: Del erases a character";

    QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    tool.onKeyPress(&esc);
    ASSERT_FALSE(tool.isEditingField());
    ASSERT_EQ(tool.selectedFieldIndex(), idx);
    EXPECT_TRUE(tool.wantsDeleteKey()) << "a selected field: Del removes it";
}

// ═══════════════════════════════════════════════════════════════════════════
// The window fixture shared by the ShortcutOverride and end-to-end suites
// ═══════════════════════════════════════════════════════════════════════════

class DeleteKeyWindow : public ::testing::Test {
protected:
    void SetUp() override
    {
        ASSERT_NE(QApplication::instance(), nullptr);
        ASSERT_TRUE(tmp_.isValid());
        watcher_ = std::make_unique<ModalWatcher>(QMessageBox::No);

        mw_ = std::make_unique<PlacingWindow>();
        mw_->resize(1300, 1000);
        mw_->show();
        mw_->openFile(QString::fromStdString(fixture("test_3page.pdf").string()));
        settle();
        doc_ = mw_->documentForTesting();
        ASSERT_NE(doc_, nullptr) << "test_3page.pdf did not open";
        ASSERT_EQ(doc_->pageCount(), 3);
        canvas_ = mw_->canvasForTesting();
        ASSERT_NE(canvas_, nullptr);
        deletePage_ = mw_->findChild<QAction*>(QStringLiteral("deletePageAction"));
        ASSERT_NE(deletePage_, nullptr);
        ASSERT_TRUE(deletePage_->isEnabled())
            << "the page shortcut must be live, or a pass proves nothing";
        spy_ = std::make_unique<QSignalSpy>(deletePage_, &QAction::triggered);

        mw_->activateWindow();
        mw_->raise();
        windowActive_ = QTest::qWaitForWindowActive(mw_.get());
    }

    void TearDown() override
    {
        spy_.reset();
        mw_.reset();
        settle();
        watcher_.reset();
    }

    // The window must be active: a Qt::WindowShortcut only matches in the
    // active window, so without this the end-to-end tests could not tell a
    // claimed key from a shortcut map that never looked.
    void focusCanvas()
    {
        ASSERT_TRUE(windowActive_) << "QTest::qWaitForWindowActive failed";
        canvas_->setFocus(Qt::OtherFocusReason);
        settle();
        ASSERT_TRUE(canvas_->hasFocus()) << "the canvas did not take focus";
    }

    bool overrideAccepted(int key = Qt::Key_Delete)
    {
        QKeyEvent ev = overrideEvent(key);
        QCoreApplication::sendEvent(canvas_, &ev);
        return ev.isAccepted();
    }

    void pressDelete(QWidget* target)
    {
        QTest::keyClick(target, Qt::Key_Delete);
        settle();
    }

    // The common oracle for "Del did not delete pages".
    void expectPagesUntouched()
    {
        EXPECT_EQ(doc_->pageCount(), 3) << "Del deleted a page";
        EXPECT_EQ(spy_->count(), 0) << "deletePageAction fired on Del";
        EXPECT_EQ(watcher_->answered(), 0)
            << "a modal opened on Del: " << watcher_->texts().join(" | ").toStdString();
    }

    // Insert a 64x64 PNG through the real Insert Image action; the image tool
    // is left set with the new image selected.
    pdfe::image::ImageTool* insertImage()
    {
        const QString png = tmp_.filePath(QStringLiteral("red.png"));
        QImage red(64, 64, QImage::Format_RGB32);
        red.fill(QColor(255, 0, 0));
        if (!red.save(png)) {
            ADD_FAILURE() << "could not write the PNG";
            return nullptr;
        }
        pdfe::ui::testseam::enqueueFileDialogPath(png);
        auto* insert = mw_->findChild<QAction*>(QStringLiteral("insertImageAction"));
        if (!insert) {
            ADD_FAILURE() << "no insertImageAction";
            return nullptr;
        }
        insert->trigger();
        settle();
        auto* tool = canvas_->imageTool();
        if (!tool || !tool->hasSelection()) {
            ADD_FAILURE() << "the inserted image is not selected";
            return nullptr;
        }
        return tool;
    }

    // Create an annotation on page 0 and select it with the live Select tool.
    int selectNewAnnotation(AnnotationType type, int& countBefore)
    {
        auto* mgr = mw_->annotationManagerForTesting();
        if (!mgr) {
            ADD_FAILURE() << "no annotation manager";
            return -1;
        }
        auto page = doc_->page(0);
        const auto data = mgr->createAnnotation(page.get(), type, QRectF(80, 80, 160, 50));
        countBefore = static_cast<int>(mgr->getAnnotations(page.get()).size());
        if (data.annotIndex < 0) {
            ADD_FAILURE() << "createAnnotation failed";
            return -1;
        }
        settle();
        if (!mw_->selectAnnotationRegionForTesting(0, data.annotIndex)) {
            ADD_FAILURE() << "selectAnnotationRegionForTesting refused";
            return -1;
        }
        settle();
        auto* tool = dynamic_cast<SelectTool*>(canvas_->currentTool());
        if (!tool || tool->selectedAnnotIndex() != data.annotIndex) {
            ADD_FAILURE() << "the Select tool does not hold the new annotation";
            return -1;
        }
        return data.annotIndex;
    }

    int annotationCount()
    {
        auto page = doc_->page(0);
        return static_cast<int>(
            mw_->annotationManagerForTesting()->getAnnotations(page.get()).size());
    }

    // A text field on page 0 of the 3-page document, then the live Form tool in
    // interaction mode with that field pressed (selected and being typed into).
    FormTool* pressNewTextField(int& widgetIndex)
    {
        FormManager mgr(doc_->context());
        {
            auto page = doc_->page(0);
            if (mgr.createFormField(page.get(), FormFieldType::Text, QRectF(72, 72, 180, 30),
                                    "DelRoutingField") < 0) {
                ADD_FAILURE() << "createFormField failed";
                return nullptr;
            }
        }
        auto* act = mw_->findChild<QAction*>(QStringLiteral("formToolAction"));
        if (!act) {
            ADD_FAILURE() << "no formToolAction";
            return nullptr;
        }
        act->trigger();
        settle();
        auto* tool = dynamic_cast<FormTool*>(canvas_->currentTool());
        if (!tool) {
            ADD_FAILURE() << "no live FormTool";
            return nullptr;
        }
        tool->setCreationFieldType(FormFieldType::Unknown);
        widgetIndex = fieldIndex();
        if (widgetIndex < 0) {
            ADD_FAILURE() << "the created field is not enumerated";
            return nullptr;
        }
        auto page = doc_->page(0);
        const QPointF centre = mgr.getFormField(page.get(), widgetIndex).rect.center();
        QMouseEvent press(QEvent::MouseButtonPress, QPointF(0, 0), QPointF(0, 0),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        tool->onMousePress(&press, 0, centre);
        QMouseEvent release(QEvent::MouseButtonRelease, QPointF(0, 0), QPointF(0, 0),
                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        tool->onMouseRelease(&release, 0, centre);
        settle();
        if (tool->selectedFieldIndex() != widgetIndex || !tool->isEditingField()) {
            ADD_FAILURE() << "the press did not select and start editing the field";
            return nullptr;
        }
        return tool;
    }

    int fieldIndex()
    {
        FormManager mgr(doc_->context());
        auto page = doc_->page(0);
        for (const auto& f : mgr.getFormFields(page.get()))
            if (f.fieldName == "DelRoutingField") return f.widgetIndex;
        return -1;
    }

    pdfe::textedit::TextEditTool* editFirstSpan()
    {
        auto* act = mw_->findChild<QAction*>(QStringLiteral("textEditToolAction"));
        if (!act) {
            ADD_FAILURE() << "no textEditToolAction";
            return nullptr;
        }
        act->trigger();
        settle();
        auto* tool = canvas_->textEditTool();
        if (!tool) {
            ADD_FAILURE() << "test_3page.pdf did not route to TextEditTool";
            return nullptr;
        }
        tool->setPage(0);
        if (tool->spanCountForTesting() <= 0) {
            ADD_FAILURE() << "page 0 has no editable span";
            return nullptr;
        }
        if (!tool->selectSpanForTesting(0) || !tool->isEditing()) {
            ADD_FAILURE() << "span 0 did not enter an edit session";
            return nullptr;
        }
        settle();
        return tool;
    }

    QTemporaryDir tmp_;
    std::unique_ptr<ModalWatcher> watcher_;
    std::unique_ptr<PlacingWindow> mw_;
    pdfe::core::Document* doc_ = nullptr;
    PdfCanvas* canvas_ = nullptr;
    QAction* deletePage_ = nullptr;
    std::unique_ptr<QSignalSpy> spy_;
    bool windowActive_ = false;
};

// ═══════════════════════════════════════════════════════════════════════════
// DeleteKeyShortcutOverride -- the canvas answers Qt's question correctly
// ═══════════════════════════════════════════════════════════════════════════

class DeleteKeyShortcutOverride : public DeleteKeyWindow {};

TEST_F(DeleteKeyShortcutOverride, NotClaimedWhenNothingIsSelectedOrEdited)
{
    EXPECT_FALSE(canvas_->wantsDeleteKey());
    EXPECT_FALSE(overrideAccepted(Qt::Key_Delete))
        << "an idle canvas must leave Del to deletePageAction";
    EXPECT_FALSE(overrideAccepted(Qt::Key_Backspace));
}

TEST_F(DeleteKeyShortcutOverride, ClaimedWhileAnImageIsSelected)
{
    auto* tool = insertImage();
    ASSERT_NE(tool, nullptr);
    EXPECT_TRUE(overrideAccepted(Qt::Key_Delete));
    EXPECT_TRUE(overrideAccepted(Qt::Key_Backspace));
    tool->deselect();
    EXPECT_FALSE(overrideAccepted(Qt::Key_Delete)) << "claimed after the image was deselected";
}

TEST_F(DeleteKeyShortcutOverride, ClaimedWhileAnAnnotationIsSelected)
{
    int before = 0;
    ASSERT_GE(selectNewAnnotation(AnnotationType::Square, before), 0);
    EXPECT_TRUE(overrideAccepted(Qt::Key_Delete));
}

TEST_F(DeleteKeyShortcutOverride, ClaimedWhileATextBoxIsSelected)
{
    int before = 0;
    ASSERT_GE(selectNewAnnotation(AnnotationType::FreeText, before), 0);
    EXPECT_TRUE(overrideAccepted(Qt::Key_Delete));
}

TEST_F(DeleteKeyShortcutOverride, ClaimedWhileAFormFieldIsSelectedOrTyped)
{
    int idx = -1;
    auto* tool = pressNewTextField(idx);
    ASSERT_NE(tool, nullptr);
    EXPECT_TRUE(overrideAccepted(Qt::Key_Delete)) << "typing into the field";
    QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    tool->onKeyPress(&esc);
    ASSERT_FALSE(tool->isEditingField());
    ASSERT_EQ(tool->selectedFieldIndex(), idx);
    EXPECT_TRUE(overrideAccepted(Qt::Key_Delete)) << "field selected, not editing";
}

TEST_F(DeleteKeyShortcutOverride, ClaimedWhileTextIsBeingEdited)
{
    auto* tool = editFirstSpan();
    ASSERT_NE(tool, nullptr);
    EXPECT_TRUE(overrideAccepted(Qt::Key_Delete));
    tool->cancelEdit();
    settle();
    EXPECT_FALSE(overrideAccepted(Qt::Key_Delete)) << "claimed after the edit ended";
}

// The old mitigation disabled deletePageAction while editing text. It is gone:
// the action stays enabled (Pages > Delete is not greyed out) and the claim
// above is what protects the keystroke.
TEST_F(DeleteKeyShortcutOverride, EditingTextNoLongerDisablesTheDeletePageAction)
{
    auto* tool = editFirstSpan();
    ASSERT_NE(tool, nullptr);
    EXPECT_TRUE(deletePage_->isEnabled()) << "editing text disabled Delete Selected Pages";
    // Re-entering a span without committing used to cache `false` and stick
    // the action disabled after the edit ended.
    ASSERT_TRUE(tool->selectSpanForTesting(0));
    settle();
    tool->cancelEdit();
    settle();
    EXPECT_TRUE(deletePage_->isEnabled()) << "the action stayed disabled after editing";
}

// ═══════════════════════════════════════════════════════════════════════════
// DeleteKeyEndToEnd -- a real Del through Qt's shortcut machinery
// ═══════════════════════════════════════════════════════════════════════════

class DeleteKeyEndToEnd : public DeleteKeyWindow {};

TEST_F(DeleteKeyEndToEnd, DelRemovesTheSelectedImageNotAPage)
{
    auto* tool = insertImage();
    ASSERT_NE(tool, nullptr);
    const int imagesBefore = mw_->pageImageCountForTesting(0);
    ASSERT_GE(imagesBefore, 1);
    ASSERT_NO_FATAL_FAILURE(focusCanvas());

    pressDelete(canvas_);

    expectPagesUntouched();
    EXPECT_EQ(mw_->pageImageCountForTesting(0), imagesBefore - 1)
        << "Del did not remove the selected image";
}

TEST_F(DeleteKeyEndToEnd, DelRemovesTheSelectedAnnotationNotAPage)
{
    int before = 0;
    ASSERT_GE(selectNewAnnotation(AnnotationType::Square, before), 0);
    ASSERT_NO_FATAL_FAILURE(focusCanvas());

    pressDelete(canvas_);

    expectPagesUntouched();
    EXPECT_EQ(annotationCount(), before - 1) << "Del did not remove the selected annotation";
}

TEST_F(DeleteKeyEndToEnd, DelRemovesTheSelectedTextBoxNotAPage)
{
    int before = 0;
    const int idx = selectNewAnnotation(AnnotationType::FreeText, before);
    ASSERT_GE(idx, 0);
    ASSERT_NO_FATAL_FAILURE(focusCanvas());

    pressDelete(canvas_);

    expectPagesUntouched();
    EXPECT_EQ(annotationCount(), before - 1) << "Del did not remove the selected FreeText box";
    auto page = doc_->page(0);
    for (const auto& a : mw_->annotationManagerForTesting()->getAnnotations(page.get())) {
        EXPECT_FALSE(a.type == AnnotationType::FreeText && a.rect == QRectF(80, 80, 160, 50))
            << "the FreeText box is still on the page";
    }
}

TEST_F(DeleteKeyEndToEnd, DelRemovesTheSelectedFormFieldNotAPage)
{
    int idx = -1;
    auto* tool = pressNewTextField(idx);
    ASSERT_NE(tool, nullptr);
    // Escape leaves the field selected but no longer typed into (the
    // "selected, not editing" state the field-delete branch is for).
    QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    tool->onKeyPress(&esc);
    ASSERT_FALSE(tool->isEditingField());
    ASSERT_EQ(tool->selectedFieldIndex(), idx);
    ASSERT_NO_FATAL_FAILURE(focusCanvas());

    pressDelete(canvas_);

    expectPagesUntouched();
    EXPECT_LT(fieldIndex(), 0) << "Del did not remove the selected form field";
}

TEST_F(DeleteKeyEndToEnd, DelForwardDeletesOneCharacterWhileTypingInAFormField)
{
    int idx = -1;
    auto* tool = pressNewTextField(idx);
    ASSERT_NE(tool, nullptr);
    ASSERT_NO_FATAL_FAILURE(focusCanvas());
    QTest::keyClicks(canvas_, QStringLiteral("abc"));
    settle();
    ASSERT_EQ(tool->editBufferForTesting(), "abc") << "typing did not reach the field";
    QTest::keyClick(canvas_, Qt::Key_Home);
    settle();
    ASSERT_EQ(tool->cursorPositionForTesting(), 0);

    pressDelete(canvas_);

    expectPagesUntouched();
    EXPECT_TRUE(tool->isEditingField()) << "Del ended the field edit";
    EXPECT_EQ(tool->editBufferForTesting(), "bc")
        << "Del must forward-delete exactly the character after the cursor";
    EXPECT_GE(fieldIndex(), 0) << "Del removed the field being typed into";
}

TEST_F(DeleteKeyEndToEnd, DelForwardDeletesOneCharacterWhileEditingText)
{
    auto* tool = editFirstSpan();
    ASSERT_NE(tool, nullptr);
    const QString before = tool->spanTextForTesting(0);
    ASSERT_GE(before.size(), 2) << "span 0 is too short to tell one deletion from more";
    ASSERT_NO_FATAL_FAILURE(focusCanvas());

    pressDelete(canvas_);

    expectPagesUntouched();
    EXPECT_TRUE(tool->isEditing()) << "Del ended the text edit";
    EXPECT_EQ(tool->spanTextForTesting(0), before.mid(1))
        << "Del must forward-delete exactly the first character (cursor at 0)";
}

// A focused editable QLineEdit claims Del for itself (Qt's own
// ShortcutOverride handling); the page shortcut must not take it.
TEST_F(DeleteKeyEndToEnd, DelInAFocusedLineEditEditsTheTextNotThePages)
{
    ASSERT_TRUE(windowActive_) << "QTest::qWaitForWindowActive failed";
    // The find bar: Edit > Find shows the search dock and focuses its box.
    auto* find = mw_->findChild<QAction*>(QStringLiteral("findAction"));
    ASSERT_NE(find, nullptr);
    find->trigger();
    settle();
    auto* edit = qobject_cast<QLineEdit*>(QApplication::focusWidget());
    ASSERT_NE(edit, nullptr) << "Find did not focus a QLineEdit";
    ASSERT_TRUE(edit->isVisible());
    ASSERT_FALSE(edit->isReadOnly());
    edit->setText(QStringLiteral("hello"));
    edit->setCursorPosition(0);

    pressDelete(edit);

    expectPagesUntouched();
    EXPECT_EQ(edit->text(), QStringLiteral("ello"))
        << "Del in the find box did not forward-delete one character";
}

// The page shortcut itself still works where it should.

TEST_F(DeleteKeyEndToEnd, DelInThePagesPanelDeletesTheSelectedPages)
{
    ASSERT_TRUE(windowActive_) << "QTest::qWaitForWindowActive failed";
    auto* panel = mw_->findChild<pdfe::ui::PagePanel*>(QStringLiteral("pagePanel"));
    ASSERT_NE(panel, nullptr);
    auto* list = panel->findChild<QListWidget*>(QStringLiteral("pagePanelList"));
    ASSERT_NE(list, nullptr);
    if (auto* dock = mw_->pagePanelDockForTesting()) {
        dock->show();
        settle();
    }
    ASSERT_TRUE(list->isVisible()) << "the Pages panel is not visible";
    ASSERT_TRUE(panel->selectPagesProgrammatic({1}));
    list->setFocus(Qt::OtherFocusReason);
    settle();
    ASSERT_TRUE(list->hasFocus());
    ASSERT_FALSE(canvas_->wantsDeleteKey());
    watcher_->setAnswer(QMessageBox::Yes);

    pressDelete(list);

    EXPECT_EQ(spy_->count(), 1) << "Del in the Pages panel did not run Delete Selected Pages";
    EXPECT_EQ(watcher_->answered(), 1) << "the confirmation was not asked exactly once";
    EXPECT_EQ(doc_->pageCount(), 2) << "the selected page was not deleted";
}

// Documented rule: with the canvas focused and nothing on it selected or being
// edited, Del is the page shortcut -- the Pages-panel selection, else the
// current page -- behind the same confirmation.
TEST_F(DeleteKeyEndToEnd, DelOnAnIdleCanvasDeletesTheCurrentPageAfterConfirmation)
{
    ASSERT_NO_FATAL_FAILURE(focusCanvas());
    ASSERT_FALSE(canvas_->wantsDeleteKey());
    watcher_->setAnswer(QMessageBox::Yes);

    pressDelete(canvas_);

    EXPECT_EQ(spy_->count(), 1) << "Del on an idle canvas did not run Delete Selected Pages";
    EXPECT_EQ(watcher_->answered(), 1) << "the confirmation was not asked exactly once";
    EXPECT_EQ(doc_->pageCount(), 2);
}

TEST_F(DeleteKeyEndToEnd, DelOnAnIdleCanvasAnsweredNoKeepsThePages)
{
    ASSERT_NO_FATAL_FAILURE(focusCanvas());
    pressDelete(canvas_);
    EXPECT_EQ(spy_->count(), 1);
    EXPECT_EQ(watcher_->answered(), 1);
    EXPECT_EQ(doc_->pageCount(), 3) << "answering No still deleted a page";
}

// Side fix: ImageTool::keyPressEvent left every key accepted, so canvas
// navigation (PageDown etc.) was swallowed while the image tool was set.
TEST_F(DeleteKeyEndToEnd, NavigationKeysStillWorkWhileTheImageToolIsSet)
{
    auto* tool = insertImage();
    ASSERT_NE(tool, nullptr);
    ASSERT_NO_FATAL_FAILURE(focusCanvas());
    canvas_->goToPage(0);
    settle();
    ASSERT_EQ(canvas_->currentPage(), 0);

    QTest::keyClick(canvas_, Qt::Key_PageDown);
    settle();

    EXPECT_EQ(canvas_->currentPage(), 1) << "PageDown was swallowed by the image tool";
    EXPECT_TRUE(tool->hasSelection()) << "PageDown must not touch the image selection";
}

}  // namespace
