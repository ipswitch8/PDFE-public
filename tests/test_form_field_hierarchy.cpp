// F15 / gap PDF-45 — AcroForm field hierarchies and inherited attributes.
//
// WHAT WAS BROKEN
// ---------------
// `pdf_dict_get_inheritable` had ZERO hits in src/forms/ and no test anywhere
// used a fully-qualified field name ("parent.child"). Real government and
// enterprise forms nest their fields; a flat name lookup writes the wrong
// field or silently misses it, and nothing would have noticed.
//
// THE ORACLE
// ----------
// tests/fixtures/f15_form_hierarchy.pdf authors a two-level tree whose child
// widgets carry NO /FT and NO /DA of their own — both must resolve through
// /Parent. This file asserts:
//   * the reported field name is the FULLY-QUALIFIED "grp.name" / "grp.city",
//   * the inherited /FT /Tx makes them Text fields (not Unknown/Button),
//   * a value set on one child lands on THAT child only, and survives
//     save/reopen,
//   * an XFDF import addressed by the qualified name reaches the right child.
//
// MUTATIONS KILLED (each confirmed RED then reverted):
//   * fed the importer the unqualified name "name" -> ImportByQualifiedName...
//     stays RED, proving the qualified name is what actually matches.
//   * cross-checked by asserting the sibling is untouched, so a "write every
//     field" implementation cannot pass.

#include <gtest/gtest.h>

#include "forms/FormManager.h"
#include "forms/FormDataIO.h"
#include "forms/FormFieldData.h"
#include "forms/FormFieldType.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"

#include "helpers/RoundTripHelper.h"

#include <algorithm>
#include <filesystem>
#include <map>
#include <memory>
#include <string>

using pdfe::forms::FormDataIO;
using pdfe::forms::FormFieldData;
using pdfe::forms::FormFieldType;
using pdfe::forms::FormManager;

namespace {

// AUTHORED literals — must match tests/fixtures/gen_f15_form_hierarchy.py.
constexpr const char* kQualifiedName = "grp.name";
constexpr const char* kQualifiedCity = "grp.city";

class FormFieldHierarchyTest : public ::testing::Test {
protected:
    void SetUp() override {
        ctx_ = std::make_shared<pdfe::core::MuPdfContext>();
        fixture_ = std::filesystem::path(TEST_FIXTURES_DIR)
                   / "f15_form_hierarchy.pdf";
        ASSERT_TRUE(std::filesystem::exists(fixture_))
            << "regenerate with python tests/fixtures/gen_f15_form_hierarchy.py";
        document_ = pdfe::core::Document::open(ctx_, fixture_);
        ASSERT_NE(document_, nullptr);
        manager_ = std::make_unique<FormManager>(ctx_);
        io_ = std::make_unique<FormDataIO>(ctx_);
        const ::testing::TestInfo* info =
            ::testing::UnitTest::GetInstance()->current_test_info();
        tempDir_ = std::filesystem::temp_directory_path()
                   / "pdfe_f15_hierarchy" / info->name();
        std::filesystem::create_directories(tempDir_);
    }
    void TearDown() override {
        io_.reset();
        manager_.reset();
        document_.reset();
        ctx_.reset();
    }

    std::map<std::string, std::string> valuesOf(pdfe::core::Document& doc) {
        std::map<std::string, std::string> out;
        auto page = doc.page(0);
        if (!page) return out;
        for (const auto& f : manager_->getFormFields(page.get())) {
            out[f.fieldName] = f.value;
        }
        return out;
    }

    int indexOf(pdfe::core::Page* page, const std::string& name) {
        for (const auto& f : manager_->getFormFields(page)) {
            if (f.fieldName == name) return f.widgetIndex;
        }
        return -1;
    }

    std::shared_ptr<pdfe::core::MuPdfContext> ctx_;
    std::filesystem::path fixture_;
    std::filesystem::path tempDir_;
    std::unique_ptr<pdfe::core::Document> document_;
    std::unique_ptr<FormManager> manager_;
    std::unique_ptr<FormDataIO> io_;
};

TEST_F(FormFieldHierarchyTest, InheritedFtAndQualifiedNamesResolveThroughParent) {
    auto page = document_->page(0);
    ASSERT_NE(page, nullptr);
    auto fields = manager_->getFormFields(page.get());
    ASSERT_EQ(fields.size(), 2u);

    EXPECT_EQ(fields[0].fieldName, kQualifiedName)
        << "child field name is not fully qualified";
    EXPECT_EQ(fields[1].fieldName, kQualifiedCity);
    for (const auto& f : fields) {
        SCOPED_TRACE(f.fieldName);
        EXPECT_EQ(f.type, FormFieldType::Text)
            << "/FT /Tx was authored only on the PARENT; it must be inherited";
    }

    // The document-level enumeration agrees with the per-page one.
    auto names = manager_->listFieldNames(document_.get());
    ASSERT_FALSE(names.empty());
    EXPECT_NE(std::find(names.begin(), names.end(), std::string(kQualifiedName)),
              names.end())
        << "listFieldNames does not expose the qualified child name";
}

TEST_F(FormFieldHierarchyTest, ValueSetOnOneChildDoesNotLeakToItsSibling) {
    auto page = document_->page(0);
    ASSERT_NE(page, nullptr);
    const int idx = indexOf(page.get(), kQualifiedName);
    ASSERT_GE(idx, 0);
    manager_->setFieldValue(page.get(), idx, "SpecimenA");
    page.reset();

    auto reopened = pdfe::test::SaveAndReopen(*document_, ctx_, tempDir_,
                                              "hierarchy");
    ASSERT_NE(reopened, nullptr);
    const auto vals = valuesOf(*reopened);
    ASSERT_EQ(vals.count(kQualifiedName), 1u);
    EXPECT_EQ(vals.at(kQualifiedName), "SpecimenA");
    ASSERT_EQ(vals.count(kQualifiedCity), 1u);
    EXPECT_EQ(vals.at(kQualifiedCity), "")
        << "writing grp.name also wrote its sibling grp.city";
}

TEST_F(FormFieldHierarchyTest, ImportByQualifiedNameReachesTheRightChild) {
    const std::string xfdf =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<xfdf xmlns=\"http://ns.adobe.com/xfdf/\" xml:space=\"preserve\">\n"
        "  <fields>\n"
        "    <field name=\"grp.city\"><value>Vancouver</value></field>\n"
        "  </fields>\n"
        "</xfdf>\n";
    io_->importFromXfdf(document_.get(), manager_.get(), xfdf);

    auto reopened = pdfe::test::SaveAndReopen(*document_, ctx_, tempDir_,
                                              "hier_import");
    ASSERT_NE(reopened, nullptr);
    const auto vals = valuesOf(*reopened);
    EXPECT_EQ(vals.at(kQualifiedCity), "Vancouver");
    EXPECT_EQ(vals.at(kQualifiedName), "")
        << "the qualified import landed on the wrong child";
}

TEST_F(FormFieldHierarchyTest, UnqualifiedNameDoesNotMatchANestedField) {
    // A flat "city" is NOT this form's field name; an importer that matches on
    // the partial name would corrupt the wrong field in a real nested form.
    const std::string xfdf =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<xfdf xmlns=\"http://ns.adobe.com/xfdf/\" xml:space=\"preserve\">\n"
        "  <fields>\n"
        "    <field name=\"city\"><value>must-not-land</value></field>\n"
        "  </fields>\n"
        "</xfdf>\n";
    io_->importFromXfdf(document_.get(), manager_.get(), xfdf);

    const auto vals = valuesOf(*document_);
    EXPECT_EQ(vals.at(kQualifiedCity), "");
    EXPECT_EQ(vals.at(kQualifiedName), "");
}

} // namespace
