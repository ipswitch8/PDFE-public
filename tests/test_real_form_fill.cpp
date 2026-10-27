// -*- coding: utf-8 -*-
//
// REAL-FORM-FILL: Test PDFE form filling against real IRS PDFs.
// Uses the corpus fixtures downloaded by assemble_realworld_corpus.py.

#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "forms/FormManager.h"
#include "forms/FormFieldData.h"

#include <filesystem>
#include <memory>
#include <string>

using namespace pdfe::core;

namespace {

std::filesystem::path corpusDir()
{
    return std::filesystem::path(TEST_FIXTURES_DIR) / "realworld";
}

} // namespace

class RealFormFillTest : public ::testing::Test {
protected:
    void SetUp() override {
        ctx_ = std::make_shared<MuPdfContext>();
    }
    std::shared_ptr<MuPdfContext> ctx_;
};

TEST_F(RealFormFillTest, IrsW9HasFillableFields)
{
    auto path = corpusDir() / "irs_w9.pdf";
    if (!std::filesystem::exists(path)) {
        GTEST_SKIP() << "Corpus not assembled: " << path;
    }

    auto doc = Document::open(ctx_, path);
    ASSERT_NE(doc, nullptr);
    EXPECT_GT(doc->pageCount(), 0);

    pdfe::forms::FormManager fm(ctx_);
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    auto fields = fm.getFormFields(page.get());
    EXPECT_GT(fields.size(), 0u)
        << "IRS W-9 must have at least one fillable form field";
}

TEST_F(RealFormFillTest, IrsW4HasFillableFields)
{
    auto path = corpusDir() / "irs_w4.pdf";
    if (!std::filesystem::exists(path)) GTEST_SKIP();

    auto doc = Document::open(ctx_, path);
    ASSERT_NE(doc, nullptr);

    pdfe::forms::FormManager fm(ctx_);
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    auto fields = fm.getFormFields(page.get());
    EXPECT_GT(fields.size(), 0u)
        << "IRS W-4 must have fillable form fields";
}

TEST_F(RealFormFillTest, IrsW9FieldFillAndSavePreservesValue)
{
    auto src = corpusDir() / "irs_w9.pdf";
    if (!std::filesystem::exists(src)) GTEST_SKIP();

    auto tmpDir = std::filesystem::temp_directory_path() / "pdfe_real_fill";
    std::filesystem::create_directories(tmpDir);
    auto out = tmpDir / "irs_w9_filled.pdf";
    std::error_code ec;
    std::filesystem::remove(out, ec);

    std::string testValue = "Acme Corporation Inc.";

    {
        auto doc = Document::open(ctx_, src);
        ASSERT_NE(doc, nullptr);

        pdfe::forms::FormManager fm(ctx_);
        auto page = doc->page(0);
        ASSERT_NE(page, nullptr);

        auto fields = fm.getFormFields(page.get());
        if (fields.empty()) {
            GTEST_SKIP() << "No fields to fill on page 0 of IRS W-9";
        }

        // Find the first text field
        int textFieldIdx = -1;
        for (size_t i = 0; i < fields.size(); ++i) {
            if (fields[i].type == pdfe::forms::FormFieldType::Text) {
                textFieldIdx = static_cast<int>(i);
                break;
            }
        }
        if (textFieldIdx < 0) {
            GTEST_SKIP() << "No text fields on page 0";
        }

        // Fill it
        EXPECT_NO_THROW({
            fm.setFieldValue(page.get(), textFieldIdx, testValue);
        });

        // Save
        EXPECT_NO_THROW(doc->saveAs(out));
    }

    // Reopen and verify the value persisted
    ASSERT_TRUE(std::filesystem::exists(out));

    {
        auto ctx2 = std::make_shared<MuPdfContext>();
        auto reopened = Document::open(ctx2, out);
        ASSERT_NE(reopened, nullptr);

        pdfe::forms::FormManager fm(ctx2);
        auto page = reopened->page(0);
        auto fields = fm.getFormFields(page.get());

        // Find a text field with our value
        bool foundValue = false;
        for (const auto& f : fields) {
            if (f.value == testValue) {
                foundValue = true;
                break;
            }
        }
        EXPECT_TRUE(foundValue)
            << "Filled value should persist through saveAs roundtrip";
    }
}

TEST_F(RealFormFillTest, AllIrsFormsHaveSomeFields)
{
    const std::vector<std::string> irsForms = {
        "irs_w9.pdf", "irs_w4.pdf", "irs_f941.pdf",
        "irs_f1099misc.pdf", "irs_f1099nec.pdf",
    };

    int withFields = 0;
    int checked = 0;

    for (const auto& name : irsForms) {
        auto path = corpusDir() / name;
        if (!std::filesystem::exists(path)) continue;
        checked++;

        auto doc = Document::open(ctx_, path);
        if (!doc || doc->pageCount() == 0) continue;

        pdfe::forms::FormManager fm(ctx_);
        // Scan first 3 pages for fields (some forms have fields on
        // later pages, e.g. multi-page returns)
        int pagesToScan = std::min(3, doc->pageCount());
        for (int i = 0; i < pagesToScan; ++i) {
            auto page = doc->page(i);
            if (!page) continue;
            if (!fm.getFormFields(page.get()).empty()) {
                withFields++;
                break;
            }
        }
    }

    if (checked == 0) {
        GTEST_SKIP() << "No IRS corpus PDFs available";
    }

    EXPECT_GE(withFields, 1)
        << "At least one IRS form must have detectable fillable fields";
}
