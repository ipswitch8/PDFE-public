// -*- coding: utf-8 -*-
// Forms designer — the form-field creation palette and tab-order
// editor. Data layer (FormManager.createFormField, CreateFormFieldCommand,
// setTabOrder) and per-type setCreationFieldType on FormTool were
// already shipped; this test covers the UI palette wiring (all 7
// field-type actions present) and the drag-reorder affordance on
// the tab-order dialog.
#include <gtest/gtest.h>
#include "ui/MainWindow.h"
#include "ui/TabOrderDialog.h"
#include "core/Document.h"
#include "core/Error.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "forms/FormFieldType.h"
#include "forms/FormManager.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QListWidget>
#include <QRectF>
#include <QToolBar>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace pdfe::ui;

TEST(FormsDesignerToolbarTest, AllSevenFieldTypeActionsPresent)
{
    MainWindow mw;
    // PDF spec field types the palette should offer:
    const char* kActions[] = {
        "formTextFieldAction",
        "formCheckboxAction",
        "formRadioButtonAction",
        "formComboBoxAction",
        "formListBoxAction",
        "formPushButtonAction",
        "formSignatureAction",
    };
    for (const char* name : kActions) {
        auto* a = mw.findChild<QAction*>(QString::fromLatin1(name));
        EXPECT_NE(a, nullptr) << "missing action: " << name;
        if (a) {
            EXPECT_TRUE(a->isCheckable())
                << name << " should be checkable (exclusive palette)";
        }
    }
}

TEST(FormsDesignerToolbarTest, FieldTypeActionsAreExclusive)
{
    MainWindow mw;
    auto* textAct = mw.findChild<QAction*>("formTextFieldAction");
    auto* cbAct   = mw.findChild<QAction*>("formCheckboxAction");
    ASSERT_NE(textAct, nullptr);
    ASSERT_NE(cbAct, nullptr);
    ASSERT_NE(textAct->actionGroup(), nullptr)
        << "field-type actions must belong to a QActionGroup for exclusivity";
    EXPECT_EQ(textAct->actionGroup(), cbAct->actionGroup());
    EXPECT_TRUE(textAct->actionGroup()->isExclusive());
}

TEST(FormsDesignerToolbarTest, DefaultSelectedActionIsText)
{
    MainWindow mw;
    auto* textAct = mw.findChild<QAction*>("formTextFieldAction");
    ASSERT_NE(textAct, nullptr);
    EXPECT_TRUE(textAct->isChecked());
}

TEST(FormsDesignerToolbarTest, FormToolBarObjectNameIsSet)
{
    MainWindow mw;
    auto* bar = mw.findChild<QToolBar*>("FormToolBar");
    ASSERT_NE(bar, nullptr);
}

// Tab order dialog exposes drag-drop reorder on the field list. Uses
// a null FormManager + null Page — the dialog's list widget is
// constructed with drag-drop enabled regardless of the populated
// content.
TEST(TabOrderDialogDragTest, FieldListSupportsInternalDragReorder)
{
    TabOrderDialog dlg(/*formManager=*/nullptr, /*page=*/nullptr);
    auto* list = dlg.findChild<QListWidget*>("tabOrderFieldList");
    ASSERT_NE(list, nullptr);
    EXPECT_EQ(list->dragDropMode(), QAbstractItemView::InternalMove);
    EXPECT_EQ(list->defaultDropAction(), Qt::MoveAction);
}

TEST(TabOrderDialogDragTest, DialogObjectNameIsSet)
{
    TabOrderDialog dlg(nullptr, nullptr);
    EXPECT_EQ(dlg.objectName(), QString("tabOrderDialog"));
}

// ═════════════════════════════════════════════════════════════════════
// A1-34 REPAIR (meta-audit-2026-07 status register): "Forms designer
// HAPPY-PATH-ONLY — 0 negative cases".
//
// Everything above walks the success path: the palette has its seven
// actions, they are exclusive, the tab-order list allows drag-reorder.
// Nothing asked what the designer does with input it must REFUSE, so a
// regression that silently accepted a null page, an unsupported field
// type, an out-of-range widget index, or a corrupt tab-order permutation
// had no oracle at all.
//
// The cases below assert only behaviour the implementation actually
// PROMISES — each one is traced to the source that promises it:
//   * FormManager.cpp: every public entry point opens with
//         if (!page) throw std::invalid_argument("Page cannot be null");
//   * FormManager::createFormField's `default:` arm ends in
//         fz_throw(ctx, FZ_ERROR_ARGUMENT, "Unsupported form field type")
//     which MuPdfContext translates into pdfe::core::MuPdfException.
//   * getNthWidget misses are `fz_throw(... "Widget index out of range")`
//     in getFormField / setFieldValue / toggleField.
//   * FormManager::setTabOrder validates explicitly — wrong length, an
//     out-of-range index, or a repeated index all `return` WITHOUT
//     mutating the stored order.
//
// SEE ALSO the suspected-defect note at the end of this file: several
// designer inputs are validated NOWHERE, and are deliberately not
// asserted here rather than freezing today's behaviour.
// ═════════════════════════════════════════════════════════════════════

namespace {

using pdfe::core::Document;
using pdfe::core::MuPdfContext;
using pdfe::core::MuPdfException;
using pdfe::core::Page;
using pdfe::forms::FormFieldType;
using pdfe::forms::FormManager;

class FormsDesignerNegativeTest : public ::testing::Test {
protected:
    void SetUp() override {
        ctx_ = std::make_shared<MuPdfContext>();
        auto fixture = std::filesystem::path(TEST_FIXTURES_DIR) / "test_form.pdf";
        ASSERT_TRUE(std::filesystem::exists(fixture))
            << "committed fixture missing: " << fixture;
        doc_ = Document::open(ctx_, fixture);
        ASSERT_NE(doc_, nullptr);
        pagePtr_ = doc_->page(0);
        ASSERT_NE(pagePtr_, nullptr);
        mgr_ = std::make_unique<FormManager>(ctx_);
        widgetCount_ =
            static_cast<int>(mgr_->getFormFields(pagePtr_.get()).size());
        ASSERT_GT(widgetCount_, 1)
            << "the negative cases need a fixture with several widgets";
    }
    void TearDown() override {
        mgr_.reset();
        pagePtr_.reset();
        doc_.reset();
        ctx_.reset();
    }
    Page* page() { return pagePtr_.get(); }

    std::shared_ptr<MuPdfContext> ctx_;
    std::unique_ptr<Document> doc_;
    std::unique_ptr<Page> pagePtr_;
    std::unique_ptr<FormManager> mgr_;
    int widgetCount_ = 0;
};

}  // namespace

// A null page is a programming error, not a recoverable state: every
// FormManager entry point the designer drives must refuse it loudly rather
// than dereference it.
//
// MUTATION KILLED: delete any of the `if (!page) throw` guards -> the call
// dereferences nullptr instead of throwing (crash, or silent corruption).
TEST_F(FormsDesignerNegativeTest, DesignerEntryPointsRejectNullPage)
{
    EXPECT_THROW(mgr_->createFormField(nullptr, FormFieldType::Text,
                                       QRectF(10, 10, 100, 20), "NullPage"),
                 std::invalid_argument);
    EXPECT_THROW(mgr_->getFormField(nullptr, 0), std::invalid_argument);
    EXPECT_THROW(mgr_->setFieldValue(nullptr, 0, "x"), std::invalid_argument);
    EXPECT_THROW(mgr_->toggleField(nullptr, 0), std::invalid_argument);
    EXPECT_THROW(mgr_->deleteFormField(nullptr, 0), std::invalid_argument);
    EXPECT_THROW(mgr_->setTabOrder(nullptr, {0}), std::invalid_argument);
    EXPECT_THROW(mgr_->getTabOrder(nullptr), std::invalid_argument);
}

// The palette offers exactly seven PDF field types. Anything outside that
// set — FormFieldType::Unknown is the only such value the enum can carry —
// must be REFUSED, not written to the document as a typeless widget that no
// reader can interpret.
//
// MUTATION KILLED: replace the `default: fz_throw(...)` arm with a silent
// fall-through (or a default to /Tx) -> a bogus type is accepted and the
// widget count grows.
TEST_F(FormsDesignerNegativeTest, CreateFormFieldRejectsUnsupportedType)
{
    const int before =
        static_cast<int>(mgr_->getFormFields(page()).size());

    EXPECT_THROW(mgr_->createFormField(page(), FormFieldType::Unknown,
                                       QRectF(50, 50, 100, 20), "Bogus"),
                 MuPdfException)
        << "an unsupported field type must be refused";

    // And the refusal must not leave a half-built widget behind.
    EXPECT_EQ(static_cast<int>(mgr_->getFormFields(page()).size()), before)
        << "a rejected createFormField must not add a widget to the page";
}

// Widget indices come straight from UI selection state, which can go stale
// when a field is deleted or the page changes underneath the designer.
// Out-of-range indices must throw rather than read past the widget list.
//
// MUTATION KILLED: remove the `if (!widget) fz_throw(...)` guards in
// getFormField / setFieldValue / toggleField -> the calls operate on a null
// pdf_annot.
TEST_F(FormsDesignerNegativeTest, OutOfRangeWidgetIndexIsRefused)
{
    const int past = widgetCount_;      // one past the last valid index
    const int wild = widgetCount_ + 1000;

    for (int bad : {past, wild}) {
        EXPECT_THROW(mgr_->getFormField(page(), bad), MuPdfException)
            << "getFormField accepted out-of-range index " << bad;
        EXPECT_THROW(mgr_->setFieldValue(page(), bad, "x"), MuPdfException)
            << "setFieldValue accepted out-of-range index " << bad;
        EXPECT_THROW(mgr_->toggleField(page(), bad), MuPdfException)
            << "toggleField accepted out-of-range index " << bad;
    }
}

// ─────────────────────────────────────────────────────────────────────
// SUSPECTED PRODUCT DEFECT — negative widget index silently addresses the
// FIRST field.
//
//   src/forms/FormManager.cpp:138
//     static pdf_annot* getNthWidget(fz_context* ctx, pdf_page* page, int index) {
//         pdf_annot* widget = pdf_first_widget(ctx, page);
//         for (int i = 0; i < index && widget; i++)
//             widget = pdf_next_widget(ctx, widget);
//         return widget;
//     }
//
// With index = -1 the loop condition `0 < -1` is false immediately, so the
// function returns pdf_first_widget() — a NON-null widget. Every caller then
// takes the success path: the `if (!widget) fz_throw(...)` guard never fires,
// and getFormField / setFieldValue / toggleField / deleteFormField operate on
// WIDGET 0 instead of refusing.
//
// This is reachable from the designer, not theoretical: FormTool uses -1 as
// its "nothing selected" sentinel (`int selectedWidget_ = -1;`,
// src/forms/FormTool.cpp), so any path that reaches FormManager before a
// field has been selected reads — or writes, or toggles, or DELETES — the
// first field on the page.
//
// This test asserts the CORRECT behaviour (refusal). It is expected to fail
// until getNthWidget rejects `index < 0`; the fix is a one-line guard.
// ─────────────────────────────────────────────────────────────────────
TEST_F(FormsDesignerNegativeTest, NegativeWidgetIndexIsRefused)
{
    const std::string firstFieldValue = mgr_->getFormField(page(), 0).value;

    EXPECT_THROW(mgr_->getFormField(page(), -1), MuPdfException)
        << "index -1 is not a widget; it must be refused, not resolved to "
           "widget 0.";
    EXPECT_THROW(mgr_->setFieldValue(page(), -1, "NEGATIVE_INDEX_WRITE"),
                 MuPdfException)
        << "a write at index -1 must be refused.";

    // Whatever the call did, it must not have MUTATED the first field: that
    // is the concrete harm — the designer's "no selection" sentinel editing
    // a real field.
    EXPECT_EQ(mgr_->getFormField(page(), 0).value, firstFieldValue)
        << "a rejected write at index -1 silently modified widget 0.";
}

// setTabOrder promises to accept only a full permutation of the page's
// widget indices; anything else leaves the existing order untouched. The
// tab-order editor hands it whatever the (drag-reorderable) list contains,
// so a corrupt list must not be able to write a corrupt /PDFE_TabOrder that
// makes fields unreachable by keyboard.
//
// MUTATIONS KILLED (FormManager::setTabOrder):
//   * drop the `newOrder.size() != widgetCount` length check
//     -> ShortOrder / LongOrder are stored.
//   * drop the `idx < 0 || idx >= widgetCount` bound check
//     -> OutOfRange is stored (and later indexes past the widget list).
//   * drop the `seen.insert(idx).second` duplicate check
//     -> Duplicate is stored, leaving one field unreachable by Tab.
// The valid-permutation case at the end keeps all of the above honest: a
// setTabOrder that rejected EVERYTHING would otherwise pass them all.
TEST_F(FormsDesignerNegativeTest, SetTabOrderRejectsMalformedPermutations)
{
    const std::vector<int> original = mgr_->getTabOrder(page());
    ASSERT_EQ(static_cast<int>(original.size()), widgetCount_);

    // (a) Too short.
    std::vector<int> tooShort(original.begin(), original.end() - 1);
    mgr_->setTabOrder(page(), tooShort);
    EXPECT_EQ(mgr_->getTabOrder(page()), original)
        << "a short tab order must be rejected and leave the order unchanged";

    // (b) Too long.
    std::vector<int> tooLong = original;
    tooLong.push_back(0);
    mgr_->setTabOrder(page(), tooLong);
    EXPECT_EQ(mgr_->getTabOrder(page()), original)
        << "an over-long tab order must be rejected";

    // (c) Right length, but an index past the end.
    std::vector<int> outOfRange = original;
    outOfRange.back() = widgetCount_;
    mgr_->setTabOrder(page(), outOfRange);
    EXPECT_EQ(mgr_->getTabOrder(page()), original)
        << "a tab order naming a nonexistent widget must be rejected";

    // (d) Right length, but a negative index.
    std::vector<int> negative = original;
    negative.front() = -1;
    mgr_->setTabOrder(page(), negative);
    EXPECT_EQ(mgr_->getTabOrder(page()), original)
        << "a tab order with a negative index must be rejected";

    // (e) Right length and all in range, but a repeated index — which means
    //     some field is missing and can never be reached by Tab.
    std::vector<int> duplicate = original;
    duplicate.back() = duplicate.front();
    mgr_->setTabOrder(page(), duplicate);
    EXPECT_EQ(mgr_->getTabOrder(page()), original)
        << "a tab order that repeats an index (and therefore drops another) "
           "must be rejected";

    // (f) POSITIVE CONTROL — a genuine permutation IS accepted, so the five
    //     rejections above cannot be satisfied by a setTabOrder that does
    //     nothing at all.
    std::vector<int> reversed(original.rbegin(), original.rend());
    ASSERT_NE(reversed, original);
    mgr_->setTabOrder(page(), reversed);
    EXPECT_EQ(mgr_->getTabOrder(page()), reversed)
        << "a valid permutation must be accepted and stored";
}

// The tab-order dialog is constructed with a null FormManager and a null
// Page by the drag-affordance tests above; that is also what the UI hands it
// before a document is open. It must come up EMPTY rather than inventing
// rows, and must survive being shown.
//
// MUTATION KILLED: populate the list without null-checking the manager/page
// -> constructing the dialog dereferences nullptr.
TEST(TabOrderDialogNegativeTest, NullManagerAndPageProduceAnEmptyList)
{
    TabOrderDialog dlg(/*formManager=*/nullptr, /*page=*/nullptr);
    auto* list = dlg.findChild<QListWidget*>("tabOrderFieldList");
    ASSERT_NE(list, nullptr);
    EXPECT_EQ(list->count(), 0)
        << "with no document there are no fields to order; the dialog must "
           "not fabricate rows.";
}

// ─────────────────────────────────────────────────────────────────────
// SUSPECTED DEFECTS — deliberately NOT asserted here.
//
// The following designer inputs are validated NOWHERE in the product
// (FormManager::createFormField, CreateFormFieldCommand and FormTool were
// all read for this): there is no promised behaviour to pin, and writing a
// test around what happens today would freeze the defect rather than expose
// it. Each is reported upstream instead:
//
//   1. DUPLICATE FIELD NAMES. createFormField writes /T verbatim with no
//      uniqueness check. Two fields created with the same name become ONE
//      field per PDF 32000-1 12.7.3.2 (same fully-qualified name => same
//      field), so they must share a type and a value — but PDFE will
//      happily make one a /Tx and the other a /Btn. FDF/XFDF import and
//      export then address an ambiguous name.
//   2. EMPTY FIELD NAME. An empty /T leaves the widget with no
//      fully-qualified name; it cannot be targeted by form data import,
//      JavaScript, or the calculation order (/CO is name-based).
//   3. DEGENERATE RECTS. A zero- or negative-size rect is stored as-is
//      (pdf_set_annot_rect), producing a widget that cannot be clicked or
//      seen. No minimum size is enforced anywhere.
//   4. RECTS OUTSIDE THE PAGE. No clamping to the MediaBox/CropBox, so a
//      field can be created entirely off-page and become unreachable.
//
// ─────────────────────────────────────────────────────────────────────
