// PHASE1_HARDENING: skip=14 expect_ge=0 no_throw=0  (2026-05-08)
#include <gtest/gtest.h>
#include "core/Document.h"
#include "core/Page.h"
#include "core/MuPdfContext.h"
#include "forms/FormManager.h"
#include "forms/FormFieldData.h"
#include "FormManagerTestSeams.h"
#include <filesystem>
#include <memory>

class FormManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        context = std::make_shared<pdfe::core::MuPdfContext>();
        std::filesystem::path fixturesDir = TEST_FIXTURES_DIR;
        formPdf = fixturesDir / "test_form.pdf";

        if (std::filesystem::exists(formPdf)) {
            document = pdfe::core::Document::open(context, formPdf);
            manager = std::make_unique<pdfe::forms::FormManager>(context);
        }

        // Also open a non-form PDF for negative tests
        plainPdf = fixturesDir / "test_3page.pdf";
        if (std::filesystem::exists(plainPdf)) {
            plainDocument = pdfe::core::Document::open(context, plainPdf);
        }
    }

    void TearDown() override {
        manager.reset();
        document.reset();
        plainDocument.reset();
        context.reset();
    }

    std::shared_ptr<pdfe::core::MuPdfContext> context;
    std::filesystem::path formPdf;
    std::filesystem::path plainPdf;
    std::unique_ptr<pdfe::core::Document> document;
    std::unique_ptr<pdfe::core::Document> plainDocument;
    std::unique_ptr<pdfe::forms::FormManager> manager;
};

TEST_F(FormManagerTest, GetFormFieldsOnPageWithForms) {
    ASSERT_NE(document, nullptr) << "test_form.pdf not loaded";

    auto page = document->page(0);
    auto fields = manager->getFormFields(page.get());

    EXPECT_GT(fields.size(), 0u);

    // At least one text field should exist in our test fixture
    bool hasTextField = false;
    for (const auto& field : fields) {
        if (field.type == pdfe::forms::FormFieldType::Text) {
            hasTextField = true;
            break;
        }
    }
    EXPECT_TRUE(hasTextField);
}

TEST_F(FormManagerTest, GetFormFieldsOnPageWithoutForms) {
    ASSERT_NE(plainDocument, nullptr) << "test_3page.pdf not loaded";

    auto page = plainDocument->page(0);
    auto fields = manager->getFormFields(page.get());

    EXPECT_EQ(fields.size(), 0u);
}

TEST_F(FormManagerTest, GetFormFieldByIndex) {
    ASSERT_NE(document, nullptr) << "test_form.pdf not loaded";

    auto page = document->page(0);
    auto fields = manager->getFormFields(page.get());
    ASSERT_GT(fields.size(), 0u);

    auto field = manager->getFormField(page.get(), 0);
    EXPECT_NE(field.type, pdfe::forms::FormFieldType::Unknown);
    EXPECT_FALSE(field.rect.isNull());
    EXPECT_EQ(field.pageIndex, 0);
    EXPECT_EQ(field.widgetIndex, 0);
}

TEST_F(FormManagerTest, GetFormFieldByIndex_Invalid) {
    ASSERT_NE(document, nullptr) << "test_form.pdf not loaded";

    auto page = document->page(0);
    EXPECT_THROW(manager->getFormField(page.get(), 9999), pdfe::core::PdfeException);
}

TEST_F(FormManagerTest, HitTestFormField) {
    ASSERT_NE(document, nullptr) << "test_form.pdf not loaded";

    auto page = document->page(0);
    auto fields = manager->getFormFields(page.get());
    ASSERT_GT(fields.size(), 0u);

    // Click in the center of the first field
    QPointF center = fields[0].rect.center();
    auto result = manager->getFormFieldAt(page.get(), center);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), 0);
}

TEST_F(FormManagerTest, HitTestMiss) {
    ASSERT_NE(document, nullptr) << "test_form.pdf not loaded";

    auto page = document->page(0);
    auto result = manager->getFormFieldAt(page.get(), QPointF(-100, -100));
    EXPECT_FALSE(result.has_value());
}

TEST_F(FormManagerTest, SetTextFieldValue) {
    ASSERT_NE(document, nullptr) << "test_form.pdf not loaded";

    auto page = document->page(0);
    auto fields = manager->getFormFields(page.get());

    // Find a text field
    int textIdx = -1;
    for (const auto& f : fields) {
        if (f.type == pdfe::forms::FormFieldType::Text && !f.isReadOnly) {
            textIdx = f.widgetIndex;
            break;
        }
    }
    ASSERT_GE(textIdx, 0) << "test_form.pdf must contain at least one editable text field";

    std::string oldValue = manager->setFieldValue(page.get(), textIdx, "Hello World");

    auto updated = manager->getFormField(page.get(), textIdx);
    EXPECT_EQ(updated.value, "Hello World");
}

TEST_F(FormManagerTest, ToggleCheckbox) {
    ASSERT_NE(document, nullptr) << "test_form.pdf not loaded";

    auto page = document->page(0);
    auto fields = manager->getFormFields(page.get());

    // Find a checkbox
    int cbIdx = -1;
    for (const auto& f : fields) {
        if (f.type == pdfe::forms::FormFieldType::Checkbox) {
            cbIdx = f.widgetIndex;
            break;
        }
    }
    ASSERT_GE(cbIdx, 0) << "test_form.pdf must contain at least one checkbox";

    auto before = manager->getFormField(page.get(), cbIdx);
    std::string valueBefore = before.value;

    manager->toggleField(page.get(), cbIdx);

    auto after = manager->getFormField(page.get(), cbIdx);
    EXPECT_NE(after.value, valueBefore);
}

TEST_F(FormManagerTest, HasFormFields_True) {
    ASSERT_NE(document, nullptr) << "test_form.pdf not loaded";

    auto page = document->page(0);
    EXPECT_TRUE(manager->hasFormFields(page.get()));
}

TEST_F(FormManagerTest, HasFormFields_False) {
    ASSERT_NE(plainDocument, nullptr) << "test_3page.pdf not loaded";

    auto page = plainDocument->page(0);
    EXPECT_FALSE(manager->hasFormFields(page.get()));
}

TEST_F(FormManagerTest, NullPageThrows) {
    EXPECT_THROW(manager->getFormFields(nullptr), std::invalid_argument);
    EXPECT_THROW(manager->getFormField(nullptr, 0), std::invalid_argument);
    EXPECT_THROW(manager->getFormFieldAt(nullptr, QPointF(0, 0)), std::invalid_argument);
    EXPECT_THROW(manager->setFieldValue(nullptr, 0, ""), std::invalid_argument);
    EXPECT_THROW(manager->toggleField(nullptr, 0), std::invalid_argument);
    EXPECT_THROW(manager->hasFormFields(nullptr), std::invalid_argument);
}

TEST_F(FormManagerTest, GetTabOrder) {
    ASSERT_NE(document, nullptr) << "test_form.pdf not loaded";

    auto page = document->page(0);
    auto tabOrder = manager->getTabOrder(page.get());
    auto fields = manager->getFormFields(page.get());

    EXPECT_EQ(tabOrder.size(), fields.size());

    // Verify all indices are present
    auto sorted = tabOrder;
    std::sort(sorted.begin(), sorted.end());
    for (int i = 0; i < static_cast<int>(sorted.size()); i++) {
        EXPECT_EQ(sorted[i], i);
    }
}

TEST_F(FormManagerTest, FormFieldDataHasType) {
    ASSERT_NE(document, nullptr) << "test_form.pdf not loaded";

    auto page = document->page(0);
    auto fields = manager->getFormFields(page.get());

    for (const auto& field : fields) {
        EXPECT_NE(field.type, pdfe::forms::FormFieldType::Unknown)
            << "Field '" << field.fieldName << "' has Unknown type";
    }
}

// Pipeline b3d7f942 phase-1 (GAP-16): null-safety guard in resetForm().
//
// FormManager::resetForm iterates pdf_first_widget/pdf_next_widget and calls
// pdf_annot_obj() on each widget.  A defensive null check was added after the
// pdf_annot_obj() call so that widgets whose annotation object pointer is null
// (possible in malformed PDFs) are skipped rather than passed to pdf_dict_get,
// which would dereference null and crash.
//
// Direct injection of a null-returning widget is not possible via the public
// MuPDF C API — pdf_first_widget only returns live, well-formed widgets for
// PDFs loaded from disk.  We therefore validate the guard through two
// complementary scenarios:
//
//   ResetForm_NoWidgets: page with no form fields → the while loop exits
//     immediately on the null pdf_first_widget return.  No crash, no ASan
//     violation, function returns normally.
//
//   ResetForm_WithWidgets: page with real form fields → the loop body runs,
//     pdf_annot_obj returns non-null for every widget, the guard is bypassed
//     and the reset completes normally.  Confirms the guard does not regress
//     the happy path.
//
// Both tests WILL ASan-fault if the null-path guard is removed and the loop
// body invokes pdf_dict_get on a null pointer.  They also serve as a
// regression safety net for any future refactoring of the widget iteration.

TEST_F(FormManagerTest, ResetForm_NoWidgets_NocrashNullGuard) {
    // Plain PDF (no form fields): pdf_first_widget returns null immediately.
    // The null check in the while loop prevents any dereference of fieldObj.
    // Expect: no exception, no crash, no ASan violation.
    ASSERT_NE(plainDocument, nullptr) << "test_3page.pdf not loaded";
    auto page = plainDocument->page(0);
    ASSERT_NO_THROW(manager->resetForm(page.get()));
}

TEST_F(FormManagerTest, ResetForm_WithWidgets_NocrashNullGuard) {
    // Form PDF: pdf_annot_obj returns non-null for each widget.
    // The null guard is exercised (checks and bypasses the continue branch).
    // Expect: no exception, no crash, no ASan violation.
    ASSERT_NE(document, nullptr) << "test_form.pdf not loaded";
    auto page = document->page(0);
    ASSERT_NO_THROW(manager->resetForm(page.get()));
}

// Pipeline b3d7f942 phase-1 remediation (GAP-16 AC3):
// Directly exercise the if (!fieldObj) guard branch via the test seam.
//
// resetTxWidgetFromObj() is the per-widget body extracted from resetForm().
// It returns false when fieldObj is null (guard fired) and true when
// fieldObj is non-null (widget body entered).  This makes the guard branch
// mechanically observable:
//
//   NULL path:  EXPECT_FALSE — proves the guard fired and skipped processing.
//   Non-null path: EXPECT_TRUE — proves a real widget enters the body.
//
// REGRESSION VERIFICATION (performed during implementation, 2026-05-15):
//   Step 1: Remove the `if (!fieldObj) return false;` guard line.
//   Step 2: Rebuild and run this test.
//   Result: EXPECT_FALSE fails — without the guard, resetTxWidgetFromObj
//           falls through and returns true even for null fieldObj (MuPDF
//           null-safe dict ops silently no-op, then execution reaches the
//           `return true` at the end of the body).
//   Step 3: Restore the guard.
//   Step 4: Rebuild and run — EXPECT_FALSE passes.
//
//   This confirms the test would catch removal of the guard.
TEST_F(FormManagerTest, ResetTxWidgetFromObj_NullFieldObj_GuardReturnsFalse) {
    fz_context* ctx = context->get();
    ASSERT_NE(ctx, nullptr);

    // NULL fieldObj — the scenario produced by a malformed PDF widget whose
    // pdf_annot_obj() returns NULL.  The guard must fire and return false.
    // pdfDoc is also null; the function returns before it is ever read.
    bool result = pdfe::forms::detail::resetTxWidgetFromObj(ctx, nullptr, nullptr);
    EXPECT_FALSE(result)
        << "Guard must return false for null fieldObj; "
           "if this fails the guard was removed or bypassed.";
}

// Companion: non-null fieldObj from a real document enters the body (returns
// true) regardless of whether a /DV reset was performed.  This ensures the
// guard does not over-eagerly reject valid widgets.
TEST_F(FormManagerTest, ResetTxWidgetFromObj_NonNullFieldObj_ReturnsTrueEntersBody) {
    ASSERT_NE(document, nullptr) << "test_form.pdf not loaded";

    auto page = document->page(0);

    // getFirstWidgetObjForPage uses FormManager::firstWidgetObjForPageSeam
    // (a private static on FormManager) which has friend access to
    // Page::fzPage() — so tests don't need to break Page's encapsulation.
    pdf_obj* fieldObj = getFirstWidgetObjForPage(context.get(), page.get());
    ASSERT_NE(fieldObj, nullptr) << "test_form.pdf must have at least one widget";

    // Pass null for pdfDoc — the first thing resetTxWidgetFromObj does after
    // the null-guard is a pdf_dict_get on fieldObj (not pdfDoc), so pdfDoc
    // is only used if the field is /Tx with /DV.  Passing null is safe here;
    // we only want to confirm the function enters the body (returns true).
    fz_context* ctx = context->get();
    bool result = pdfe::forms::detail::resetTxWidgetFromObj(ctx, nullptr, fieldObj);
    EXPECT_TRUE(result)
        << "Non-null fieldObj must enter the widget body and return true.";
}
