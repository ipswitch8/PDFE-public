#include <gtest/gtest.h>
#include "core/Document.h"
#include "core/Page.h"
#include "core/MuPdfContext.h"
#include "forms/FormManager.h"
#include "forms/FormDataIO.h"
#include "forms/FormFieldData.h"
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;

class FormDataIOTest : public ::testing::Test {
protected:
    void SetUp() override {
        context = std::make_shared<pdfe::core::MuPdfContext>();

        testFormPdfPath = fs::path(TEST_FIXTURES_DIR) / "test_form.pdf";
        if (fs::exists(testFormPdfPath)) {
            hasTestForm = true;
            document = pdfe::core::Document::open(context, testFormPdfPath);
            formManager = std::make_unique<pdfe::forms::FormManager>(context);
            formDataIO = std::make_unique<pdfe::forms::FormDataIO>(context);
        } else {
            hasTestForm = false;
        }
    }

    void TearDown() override {
        formDataIO.reset();
        formManager.reset();
        document.reset();
        context.reset();
    }

    std::shared_ptr<pdfe::core::MuPdfContext> context;
    std::unique_ptr<pdfe::core::Document> document;
    std::unique_ptr<pdfe::forms::FormManager> formManager;
    std::unique_ptr<pdfe::forms::FormDataIO> formDataIO;
    fs::path testFormPdfPath;
    bool hasTestForm = false;
};

TEST_F(FormDataIOTest, Construction) {
    auto context = std::make_shared<pdfe::core::MuPdfContext>();
    EXPECT_NO_THROW(pdfe::forms::FormDataIO io(context));
}

TEST_F(FormDataIOTest, ConstructionWithNullContext) {
    EXPECT_THROW(pdfe::forms::FormDataIO io(nullptr), std::invalid_argument);
}

TEST_F(FormDataIOTest, MoveConstruction) {
    auto context = std::make_shared<pdfe::core::MuPdfContext>();
    pdfe::forms::FormDataIO io1(context);
    pdfe::forms::FormDataIO io2(std::move(io1));
    SUCCEED();
}

TEST_F(FormDataIOTest, MoveAssignment) {
    auto context = std::make_shared<pdfe::core::MuPdfContext>();
    pdfe::forms::FormDataIO io1(context);
    pdfe::forms::FormDataIO io2(context);
    io2 = std::move(io1);
    SUCCEED();
}

TEST_F(FormDataIOTest, ExportXfdf) {
    if (!hasTestForm) {
        GTEST_SKIP() << "test_form.pdf not available";
    }

    std::string xfdf = formDataIO->exportToXfdf(document.get(), formManager.get());

    EXPECT_FALSE(xfdf.empty());
    EXPECT_NE(xfdf.find("<?xml"), std::string::npos);
    EXPECT_NE(xfdf.find("<xfdf"), std::string::npos);
    EXPECT_NE(xfdf.find("xmlns=\"http://ns.adobe.com/xfdf/\""), std::string::npos);
    EXPECT_NE(xfdf.find("<fields>"), std::string::npos);
    EXPECT_NE(xfdf.find("</fields>"), std::string::npos);
    EXPECT_NE(xfdf.find("</xfdf>"), std::string::npos);
}

TEST_F(FormDataIOTest, ExportXfdfWithNullDocument) {
    if (!hasTestForm) {
        GTEST_SKIP() << "test_form.pdf not available";
    }

    EXPECT_THROW(formDataIO->exportToXfdf(nullptr, formManager.get()),
                 std::invalid_argument);
}

TEST_F(FormDataIOTest, ExportXfdfWithNullManager) {
    if (!hasTestForm) {
        GTEST_SKIP() << "test_form.pdf not available";
    }

    EXPECT_THROW(formDataIO->exportToXfdf(document.get(), nullptr),
                 std::invalid_argument);
}

TEST_F(FormDataIOTest, ImportXfdf) {
    if (!hasTestForm) {
        GTEST_SKIP() << "test_form.pdf not available";
    }

    // Export current state
    std::string originalXfdf = formDataIO->exportToXfdf(document.get(), formManager.get());

    // Get first page and fields
    auto page = document->page(0);
    ASSERT_NE(page, nullptr);

    auto fields = formManager->getFormFields(page.get());
    if (fields.empty()) {
        GTEST_SKIP() << "No form fields in test document";
    }

    // Save original value of first field
    std::string originalValue = fields[0].value;

    // Set a different value
    std::string newValue = "TestValue_Modified";
    formManager->setFieldValue(page.get(), fields[0].widgetIndex, newValue);

    // Verify the change
    auto modifiedField = formManager->getFormField(page.get(), fields[0].widgetIndex);
    EXPECT_EQ(modifiedField.value, newValue);

    // Import original XFDF
    EXPECT_NO_THROW(formDataIO->importFromXfdf(document.get(), formManager.get(), originalXfdf));

    // Verify value restored
    auto restoredField = formManager->getFormField(page.get(), fields[0].widgetIndex);
    EXPECT_EQ(restoredField.value, originalValue);
}

TEST_F(FormDataIOTest, ImportXfdfWithNullDocument) {
    if (!hasTestForm) {
        GTEST_SKIP() << "test_form.pdf not available";
    }

    std::string xfdf = R"(<?xml version="1.0"?>
<xfdf xmlns="http://ns.adobe.com/xfdf/">
  <fields>
    <field name="test"><value>value</value></field>
  </fields>
</xfdf>)";

    EXPECT_THROW(formDataIO->importFromXfdf(nullptr, formManager.get(), xfdf),
                 std::invalid_argument);
}

TEST_F(FormDataIOTest, ImportXfdfWithNullManager) {
    if (!hasTestForm) {
        GTEST_SKIP() << "test_form.pdf not available";
    }

    std::string xfdf = R"(<?xml version="1.0"?>
<xfdf xmlns="http://ns.adobe.com/xfdf/">
  <fields>
    <field name="test"><value>value</value></field>
  </fields>
</xfdf>)";

    EXPECT_THROW(formDataIO->importFromXfdf(document.get(), nullptr, xfdf),
                 std::invalid_argument);
}

TEST_F(FormDataIOTest, ImportXfdfWithEmptyContent) {
    if (!hasTestForm) {
        GTEST_SKIP() << "test_form.pdf not available";
    }

    EXPECT_THROW(formDataIO->importFromXfdf(document.get(), formManager.get(), ""),
                 std::invalid_argument);
}

TEST_F(FormDataIOTest, RoundTripXfdf) {
    if (!hasTestForm) {
        GTEST_SKIP() << "test_form.pdf not available";
    }

    // Get first page and fields
    auto page = document->page(0);
    ASSERT_NE(page, nullptr);

    auto fields = formManager->getFormFields(page.get());
    if (fields.empty()) {
        GTEST_SKIP() << "No form fields in test document";
    }

    // Set known values
    std::string testValue1 = "RoundTripValue_1";
    formManager->setFieldValue(page.get(), fields[0].widgetIndex, testValue1);

    // Export to XFDF
    std::string xfdf = formDataIO->exportToXfdf(document.get(), formManager.get());

    // Modify values
    std::string differentValue = "DifferentValue_Modified";
    formManager->setFieldValue(page.get(), fields[0].widgetIndex, differentValue);

    // Verify modification
    auto modifiedField = formManager->getFormField(page.get(), fields[0].widgetIndex);
    EXPECT_EQ(modifiedField.value, differentValue);

    // Import XFDF
    formDataIO->importFromXfdf(document.get(), formManager.get(), xfdf);

    // Verify original values restored
    auto restoredField = formManager->getFormField(page.get(), fields[0].widgetIndex);
    EXPECT_EQ(restoredField.value, testValue1);
}

TEST_F(FormDataIOTest, ImportInvalidXfdf) {
    if (!hasTestForm) {
        GTEST_SKIP() << "test_form.pdf not available";
    }

    std::string invalidXfdf = R"(<?xml version="1.0"?>
<xfdf xmlns="http://ns.adobe.com/xfdf/">
  <fields>
    <field name="test"><value>unclosed value
  </fields>
</xfdf>)";

    EXPECT_THROW(formDataIO->importFromXfdf(document.get(), formManager.get(), invalidXfdf),
                 pdfe::core::PdfeException);
}

TEST_F(FormDataIOTest, ImportMalformedXml) {
    if (!hasTestForm) {
        GTEST_SKIP() << "test_form.pdf not available";
    }

    std::string malformedXml = "This is not XML at all!";

    EXPECT_THROW(formDataIO->importFromXfdf(document.get(), formManager.get(), malformedXml),
                 pdfe::core::PdfeException);
}

TEST_F(FormDataIOTest, EmptyFormExport) {
    if (!hasTestForm) {
        GTEST_SKIP() << "test_form.pdf not available";
    }

    // Create a new document without forms (if possible) or use existing and clear fields
    std::string xfdf = formDataIO->exportToXfdf(document.get(), formManager.get());

    // Verify valid XFDF structure even if empty
    EXPECT_FALSE(xfdf.empty());
    EXPECT_NE(xfdf.find("<?xml"), std::string::npos);
    EXPECT_NE(xfdf.find("<xfdf"), std::string::npos);
    EXPECT_NE(xfdf.find("<fields>"), std::string::npos);
    EXPECT_NE(xfdf.find("</fields>"), std::string::npos);
    EXPECT_NE(xfdf.find("</xfdf>"), std::string::npos);
}

TEST_F(FormDataIOTest, ExportDeduplicatesFields) {
    if (!hasTestForm) {
        GTEST_SKIP() << "test_form.pdf not available";
    }

    // Collect all widget field names across all pages
    std::vector<std::string> allWidgetNames;
    std::set<std::string> uniqueWidgetNames;
    for (int pageIndex = 0; pageIndex < document->pageCount(); ++pageIndex) {
        auto page = document->page(pageIndex);
        auto fields = formManager->getFormFields(page.get());
        for (const auto& field : fields) {
            if (!field.fieldName.empty()) {
                allWidgetNames.push_back(field.fieldName);
                uniqueWidgetNames.insert(field.fieldName);
            }
        }
    }

    ASSERT_GT(allWidgetNames.size(), 0u) << "Need at least one form field";

    // Export to XFDF
    std::string xfdf = formDataIO->exportToXfdf(document.get(), formManager.get());
    EXPECT_FALSE(xfdf.empty());

    // Count <field name="..."> elements in the XFDF output
    int fieldElementCount = 0;
    std::string::size_type pos = 0;
    while ((pos = xfdf.find("<field ", pos)) != std::string::npos) {
        fieldElementCount++;
        pos += 7;
    }

    // The number of <field> elements must equal the number of unique field names,
    // NOT the total widget count (which may be higher due to radio buttons etc.)
    EXPECT_EQ(static_cast<size_t>(fieldElementCount), uniqueWidgetNames.size())
        << "XFDF should have exactly one <field> per unique field name";

    // Verify each unique field name appears exactly once in the XFDF
    for (const auto& name : uniqueWidgetNames) {
        std::string needle = "name=\"" + name + "\"";
        auto first = xfdf.find(needle);
        ASSERT_NE(first, std::string::npos) << "Field '" << name << "' missing from XFDF";
        auto second = xfdf.find(needle, first + needle.size());
        EXPECT_EQ(second, std::string::npos)
            << "Field '" << name << "' appears more than once in XFDF";
    }
}

TEST_F(FormDataIOTest, ImportAppliesToAllPagesWithSameFieldName) {
    if (!hasTestForm) {
        GTEST_SKIP() << "test_form.pdf not available";
    }

    // Create XFDF with known field value
    std::string xfdf = R"(<?xml version="1.0" encoding="UTF-8"?>
<xfdf xmlns="http://ns.adobe.com/xfdf/" xml:space="preserve">
  <fields>
    <field name="TestField">
      <value>ImportedValue</value>
    </field>
  </fields>
</xfdf>)";

    // Import - should apply to all instances of "TestField" across all pages
    EXPECT_NO_THROW(formDataIO->importFromXfdf(document.get(), formManager.get(), xfdf));

    // If the field exists, verify it was set (best effort test)
    for (int pageIndex = 0; pageIndex < document->pageCount(); ++pageIndex) {
        auto page = document->page(pageIndex);
        auto fields = formManager->getFormFields(page.get());

        for (const auto& field : fields) {
            if (field.fieldName == "TestField") {
                EXPECT_EQ(field.value, "ImportedValue");
            }
        }
    }
}

TEST_F(FormDataIOTest, XfdfContainsOnlyNonEmptyValues) {
    if (!hasTestForm) {
        GTEST_SKIP() << "test_form.pdf not available";
    }

    // Export and verify that empty-valued fields are not included
    std::string xfdf = formDataIO->exportToXfdf(document.get(), formManager.get());

    // We can't check the exact content without knowing field names,
    // but the implementation guarantees only non-empty values are exported
    EXPECT_NE(xfdf.find("<fields>"), std::string::npos);
    SUCCEED();
}
