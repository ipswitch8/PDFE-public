// -*- coding: utf-8 -*-
//
// R3-09 — Form-JavaScript calculation CORRECTNESS (not just "no crash").
//
// The gap (register.md R3-09): calculateAll / fireCalculateEvent / fireFormatEvent
// are smoke-tested for no-throw / empty-or-non-empty in test_javascript_engine.cpp;
// none asserts that field3 = field1 + field2 yields the correct SUM. Hostile form
// JS is well covered — the hole is purely calculation correctness.
//
// Fixture: tests/fixtures/form_calc_sum.pdf (gen_form_calc_sum.py)
//   field1, field2, field3 (all FT=Tx). field3 carries an /AA /C (calculate)
//   JavaScript action:
//     var a=this.getField("field1").value; var b=this.getField("field2").value;
//     event.value = (a==""?0:Number(a))+(b==""?0:Number(b));
//   AcroForm /CO = [field3], /NeedAppearances true.
//
// Oracle: seed field1=2, field2=3, run the document calculation, and assert the
// engine actually computed field3 == "5". This is the correctness assertion the
// current smoke tests lack; it kills a mutation where the calculate event fires
// but computes nothing (value stays empty / stale).

#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/Page.h"
#include "core/MuPdfContext.h"
#include "forms/FormManager.h"
#include "forms/FormFieldData.h"
#include "forms/JavaScriptEngine.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

using namespace pdfe::core;
using namespace pdfe::forms;

namespace {

class FidelityR3_09_FormCalc : public ::testing::Test {
protected:
    void SetUp() override {
        context = std::make_shared<MuPdfContext>();
        std::filesystem::path dir = TEST_FIXTURES_DIR;
        fixture = dir / "form_calc_sum.pdf";
        if (!std::filesystem::exists(fixture)) {
            GTEST_SKIP() << "form_calc_sum.pdf fixture not available";
        }
        document = Document::open(context, fixture);
        if (!document) GTEST_SKIP() << "could not open form_calc_sum.pdf";
        page = document->page(0);
        if (!page) GTEST_SKIP() << "no page 0 in form_calc_sum.pdf";
    }

    // Return the widget index of the field whose fieldName equals `name`.
    std::optional<int> widgetIndexOf(FormManager& fm, const std::string& name) {
        auto fields = fm.getFormFields(page.get());
        for (const auto& f : fields) {
            if (f.fieldName == name) return f.widgetIndex;
        }
        return std::nullopt;
    }

    std::shared_ptr<MuPdfContext> context;
    std::filesystem::path fixture;
    std::unique_ptr<Document> document;
    std::unique_ptr<Page> page;
};

TEST_F(FidelityR3_09_FormCalc, CalculatedFieldEqualsSumOfInputs) {
    FormManager fm(context);
    ASSERT_TRUE(fm.hasFormFields(page.get()))
        << "fixture has no form fields";

    auto i1 = widgetIndexOf(fm, "field1");
    auto i2 = widgetIndexOf(fm, "field2");
    auto i3 = widgetIndexOf(fm, "field3");
    ASSERT_TRUE(i1 && i2 && i3)
        << "expected field1/field2/field3 in the AcroForm";

    // Seed the two input fields.
    fm.setFieldValue(page.get(), *i1, "2");
    fm.setFieldValue(page.get(), *i2, "3");

    // Run the document's calculation scripts.
    JavaScriptEngine js(context);
    js.enable(page.get());
    ASSERT_TRUE(js.isEnabled())
        << "JavaScript engine did not enable for the document";
    js.calculateAll(page.get());

    // Correctness assertion: field3 must now hold the sum "5".
    FormFieldData result = fm.getFormField(page.get(), *i3);
    EXPECT_EQ(result.value, "5")
        << "calculate script did not compute field1+field2; got '"
        << result.value << "' (expected '5'). The engine fired the event but "
           "produced the wrong/empty value.";
}

TEST_F(FidelityR3_09_FormCalc, RecalculationReflectsChangedInputs) {
    FormManager fm(context);
    auto i1 = widgetIndexOf(fm, "field1");
    auto i2 = widgetIndexOf(fm, "field2");
    auto i3 = widgetIndexOf(fm, "field3");
    ASSERT_TRUE(i1 && i2 && i3);

    JavaScriptEngine js(context);
    js.enable(page.get());
    ASSERT_TRUE(js.isEnabled());

    fm.setFieldValue(page.get(), *i1, "10");
    fm.setFieldValue(page.get(), *i2, "7");
    js.calculateAll(page.get());
    EXPECT_EQ(fm.getFormField(page.get(), *i3).value, "17")
        << "recalculation did not track changed inputs (10+7)";
}

} // namespace
