#include <gtest/gtest.h>
#include "core/MuPdfContext.h"
#include "core/Document.h"
#include "compliance/PdfAValidator.h"
#include "compliance/XmpMetadataWriter.h"
#include "compliance/IccProfileManager.h"
#include "compliance/ComplianceTypes.h"
#include <filesystem>
#include <memory>
#include <algorithm>

using namespace pdfe::compliance;

class PdfAValidatorTest : public ::testing::Test {
protected:
    void SetUp() override {
        context_ = std::make_shared<pdfe::core::MuPdfContext>();
        auto pdfPath = std::filesystem::path(TEST_FIXTURES_DIR) / "test_single.pdf";
        if (!std::filesystem::exists(pdfPath)) {
            GTEST_SKIP() << "test_single.pdf not found";
        }
        document_ = pdfe::core::Document::open(context_, pdfPath);
        ASSERT_NE(document_, nullptr);
    }

    void TearDown() override {
        document_.reset();
        context_.reset();
    }

    std::shared_ptr<pdfe::core::MuPdfContext> context_;
    std::unique_ptr<pdfe::core::Document> document_;
};

TEST_F(PdfAValidatorTest, ConstructDestroy) {
    PdfAValidator validator(context_);
    SUCCEED();
}

TEST_F(PdfAValidatorTest, Validate_None_Fails) {
    PdfAValidator validator(context_);
    auto result = validator.validate(document_.get(), PdfALevel::None);
    EXPECT_FALSE(result.success);
}

TEST_F(PdfAValidatorTest, Validate_NullDoc_Fails) {
    PdfAValidator validator(context_);
    auto result = validator.validate(nullptr, PdfALevel::PdfA_1b);
    EXPECT_FALSE(result.success);
}

TEST_F(PdfAValidatorTest, Validate_FreshDoc_HasErrors) {
    PdfAValidator validator(context_);
    auto result = validator.validate(document_.get(), PdfALevel::PdfA_1b);
    // A fresh doc won't have XMP or OutputIntent
    EXPECT_TRUE(result.hasErrors());
    EXPECT_GT(result.errorCount(), 0);
}

TEST_F(PdfAValidatorTest, Validate_MissingXMP_ReportsError) {
    PdfAValidator validator(context_);
    auto result = validator.validate(document_.get(), PdfALevel::PdfA_2b);

    bool foundXmpIssue = false;
    for (const auto& issue : result.issues) {
        if (issue.rule == "6.7.11") {
            foundXmpIssue = true;
            break;
        }
    }
    EXPECT_TRUE(foundXmpIssue) << "Should report missing XMP metadata";
}

TEST_F(PdfAValidatorTest, Validate_MissingOutputIntent_ReportsError) {
    PdfAValidator validator(context_);
    auto result = validator.validate(document_.get(), PdfALevel::PdfA_2b);

    bool foundOutputIntent = false;
    for (const auto& issue : result.issues) {
        if (issue.rule == "6.2.2") {
            foundOutputIntent = true;
            break;
        }
    }
    EXPECT_TRUE(foundOutputIntent) << "Should report missing OutputIntent";
}

TEST_F(PdfAValidatorTest, Validate_WithXmpAndIntent_FewerErrors) {
    // Add XMP and OutputIntent to reduce errors
    XmpMetadataWriter xmpWriter(context_);
    xmpWriter.writePdfAIdentification(document_.get(), PdfALevel::PdfA_2b);

    IccProfileManager iccMgr(context_);
    iccMgr.addSrgbOutputIntent(document_.get());

    // Save and reopen
    auto tempPath = std::filesystem::temp_directory_path() / "test_validator_partial.pdf";
    document_->saveAs(tempPath);
    auto doc2 = pdfe::core::Document::open(context_, tempPath);

    PdfAValidator validator(context_);
    auto result = validator.validate(doc2.get(), PdfALevel::PdfA_2b);

    // Should NOT have XMP or OutputIntent errors
    bool hasXmpError = false;
    bool hasIntentError = false;
    for (const auto& issue : result.issues) {
        if (issue.rule == "6.7.11" && issue.severity == IssueSeverity::Error) hasXmpError = true;
        if (issue.rule == "6.2.2") hasIntentError = true;
    }
    EXPECT_FALSE(hasXmpError) << "Should not report XMP error after adding it";
    EXPECT_FALSE(hasIntentError) << "Should not report OutputIntent error after adding it";

    doc2.reset();
    std::filesystem::remove(tempPath);
}

TEST_F(PdfAValidatorTest, IsCompliant_FreshDoc_False) {
    PdfAValidator validator(context_);
    EXPECT_FALSE(validator.isCompliant(document_.get(), PdfALevel::PdfA_2b));
}

TEST_F(PdfAValidatorTest, Validate_PdfA2a_RequiresTagging) {
    PdfAValidator validator(context_);
    auto result = validator.validate(document_.get(), PdfALevel::PdfA_2a);

    bool foundTagError = false;
    for (const auto& issue : result.issues) {
        if (issue.rule == "6.8") {
            foundTagError = true;
            break;
        }
    }
    EXPECT_TRUE(foundTagError) << "PDF/A-2a should require tagged PDF";
}

TEST_F(PdfAValidatorTest, Validate_PdfA1b_DoesNotRequireTagging) {
    PdfAValidator validator(context_);
    auto result = validator.validate(document_.get(), PdfALevel::PdfA_1b);

    bool foundTagError = false;
    for (const auto& issue : result.issues) {
        if (issue.rule == "6.8") {
            foundTagError = true;
            break;
        }
    }
    EXPECT_FALSE(foundTagError) << "PDF/A-1b should NOT require tagged PDF";
}

TEST_F(PdfAValidatorTest, Validate_ResultHasTargetLevel) {
    PdfAValidator validator(context_);
    auto result = validator.validate(document_.get(), PdfALevel::PdfA_3b);
    EXPECT_EQ(result.targetLevel, PdfALevel::PdfA_3b);
}

TEST_F(PdfAValidatorTest, Validate_AllLevelsReturnResult) {
    PdfAValidator validator(context_);

    auto levels = {PdfALevel::PdfA_1b, PdfALevel::PdfA_2b,
                   PdfALevel::PdfA_2u, PdfALevel::PdfA_2a,
                   PdfALevel::PdfA_3b};

    for (auto level : levels) {
        auto result = validator.validate(document_.get(), level);
        EXPECT_EQ(result.targetLevel, level) << "Target level should match for " << pdfaLevelName(level);
        EXPECT_TRUE(result.hasErrors()) << "Fresh doc should have errors for " << pdfaLevelName(level);
    }
}
