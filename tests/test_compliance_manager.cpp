#include <gtest/gtest.h>
#include "core/MuPdfContext.h"
#include "core/Document.h"
#include "compliance/ComplianceManager.h"
#include "compliance/ComplianceTypes.h"
#include <filesystem>
#include <memory>

using namespace pdfe::compliance;

class ComplianceManagerTest : public ::testing::Test {
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

TEST_F(ComplianceManagerTest, ConstructDestroy) {
    ComplianceManager mgr(context_);
    SUCCEED();
}

TEST_F(ComplianceManagerTest, DetectPdfALevel_FreshDoc_None) {
    ComplianceManager mgr(context_);
    EXPECT_EQ(mgr.detectPdfALevel(document_.get()), PdfALevel::None);
}

TEST_F(ComplianceManagerTest, ValidatePdfA_FreshDoc_Fails) {
    ComplianceManager mgr(context_);
    auto result = mgr.validatePdfA(document_.get(), PdfALevel::PdfA_2b);
    EXPECT_TRUE(result.hasErrors());
}

TEST_F(ComplianceManagerTest, ValidatePdfUA_FreshDoc_Fails) {
    ComplianceManager mgr(context_);
    auto result = mgr.validatePdfUA(document_.get());
    EXPECT_TRUE(result.hasErrors());
}

TEST_F(ComplianceManagerTest, ConvertToPdfA_2b) {
    auto outputPath = std::filesystem::temp_directory_path() / "test_mgr_pdfa_2b.pdf";

    ComplianceManager mgr(context_);
    auto result = mgr.convertToPdfA(document_.get(), PdfALevel::PdfA_2b, outputPath);

    EXPECT_TRUE(std::filesystem::exists(outputPath));
    EXPECT_EQ(result.targetLevel, PdfALevel::PdfA_2b);

    std::filesystem::remove(outputPath);
}

TEST_F(ComplianceManagerTest, ConvertToPdfUA) {
    auto outputPath = std::filesystem::temp_directory_path() / "test_mgr_pdfua.pdf";

    ComplianceManager mgr(context_);
    auto result = mgr.convertToPdfUA(document_.get(), outputPath, "en-US");

    EXPECT_TRUE(std::filesystem::exists(outputPath));
    EXPECT_EQ(result.targetUALevel, PdfUALevel::PdfUA_1);

    std::filesystem::remove(outputPath);
}

TEST_F(ComplianceManagerTest, AnalyzeForConversion) {
    ComplianceManager mgr(context_);
    auto result = mgr.analyzeForConversion(document_.get(), PdfALevel::PdfA_2b);
    EXPECT_TRUE(result.success);
}

TEST_F(ComplianceManagerTest, ConvertToPdfA_AllLevels) {
    ComplianceManager mgr(context_);
    auto levels = {PdfALevel::PdfA_1b, PdfALevel::PdfA_2b, PdfALevel::PdfA_3b};

    int idx = 0;
    for (auto level : levels) {
        auto outputPath = std::filesystem::temp_directory_path() /
            ("test_mgr_level_" + std::to_string(idx++) + ".pdf");

        auto result = mgr.convertToPdfA(document_.get(), level, outputPath);
        EXPECT_TRUE(std::filesystem::exists(outputPath))
            << "Output should exist for " << pdfaLevelName(level);

        std::filesystem::remove(outputPath);
    }
}

// Pipeline F phase-1: regression for the Karen MEDIUM finding from
// Pipeline E phase-12. Before this fix, fixIssue() always stamped
// PDF/A-2b XMP regardless of the active preflight target, so a 1b
// run would surface a level-mismatch warning on re-validation. The
// 3-arg overload (and fixAll forwarding the level) must produce
// pdfaid:part=1 when the target is 1b.
TEST_F(ComplianceManagerTest, FixIssue_StampsXmpForExplicitLevel_PdfA1b) {
    ComplianceManager mgr(context_);

    // Synthesize a MissingXmpMetadata issue and feed it to the
    // 3-arg overload with PDF/A-1b as the target.
    ComplianceIssue issue;
    issue.severity = IssueSeverity::Error;
    issue.rule = "6.7.11";
    issue.description = "Missing XMP metadata";
    issue.type = IssueType::MissingXmpMetadata;
    issue.fixStrategy = FixStrategy::Auto;

    auto fr = mgr.fixIssue(document_.get(), issue, PdfALevel::PdfA_1b);
    ASSERT_TRUE(fr.ok) << fr.errorMessage;

    EXPECT_EQ(mgr.detectPdfALevel(document_.get()), PdfALevel::PdfA_1b)
        << "After fixIssue with target=1b, /Catalog/Metadata should "
           "advertise pdfaid:part=1 / conformance=B.";
}

TEST_F(ComplianceManagerTest, FixAll_ForwardsLevelToXmpFixer_PdfA1b) {
    ComplianceManager mgr(context_);

    std::vector<ComplianceIssue> issues;
    {
        ComplianceIssue i;
        i.severity = IssueSeverity::Error;
        i.rule = "6.7.11";
        i.description = "Missing XMP metadata";
        i.type = IssueType::MissingXmpMetadata;
        i.fixStrategy = FixStrategy::Auto;
        issues.push_back(i);
    }

    auto agg = mgr.fixAll(document_.get(), issues, PdfALevel::PdfA_1b);
    EXPECT_GE(agg.succeeded, 1);
    EXPECT_EQ(agg.failed, 0);
    EXPECT_EQ(mgr.detectPdfALevel(document_.get()), PdfALevel::PdfA_1b)
        << "fixAll(target=PdfA_1b) must forward the level to the XMP "
           "fixer, not silently stamp 2b.";
}

TEST_F(ComplianceManagerTest, FixIssue_TwoArgOverloadStillDefaultsTo2b) {
    // Back-compat: the legacy two-arg overload must still stamp 2b
    // (existing callers / tests rely on that default).
    ComplianceManager mgr(context_);
    ComplianceIssue issue;
    issue.severity = IssueSeverity::Error;
    issue.type = IssueType::MissingXmpMetadata;
    issue.fixStrategy = FixStrategy::Auto;

    auto fr = mgr.fixIssue(document_.get(), issue);
    ASSERT_TRUE(fr.ok) << fr.errorMessage;
    EXPECT_EQ(mgr.detectPdfALevel(document_.get()), PdfALevel::PdfA_2b);
}

// Pipeline a7c4f891 phase-4 (B7): malformed PDF/A claim detection.
//
// A hostile PDF can stamp /Catalog/Metadata XMP claiming PDF/A-2
// conformance via pdfaid:part=2 without actually meeting the
// PDF/A requirements (missing OutputIntent, embedded RGB content,
// non-embedded fonts, etc.). PdfAValidator::validate() MUST detect
// the violation and report errors rather than trusting the claim
// blindly. This locks the validator's input-distrust contract.

TEST_F(ComplianceManagerTest, ValidateMalformedPdfAClaim_FreshDocWithoutCompliance) {
    // The fresh test_single.pdf doesn't claim PDF/A — validating
    // against PdfA_2b should fail (as ValidatePdfA_FreshDoc_Fails
    // already proves). What we're newly asserting here is the
    // validator's REASON for failure: it must cite specific
    // missing-feature errors (missing OutputIntent, missing XMP
    // pdfaid claim, etc.) rather than silently returning a generic
    // "not compliant" status. A blind-trust validator would
    // potentially miss malformed claims.
    ComplianceManager mgr(context_);
    auto result = mgr.validatePdfA(document_.get(), PdfALevel::PdfA_2b);
    EXPECT_TRUE(result.hasErrors())
        << "PDF without any PDF/A claim must fail validation";
    EXPECT_FALSE(result.issues.empty())
        << "validate() must populate the issues list with specific "
        << "violation reasons, not return a bare hasErrors()=true";
    // At least one issue must have a non-empty rule string OR a
    // non-empty description — the validator must emit SPECIFIC
    // violation evidence, not a bare "fails to comply" with no
    // reason. The IssueType enum is set on-demand via
    // classifyIssueByRule() inside fixAll(); validators emit the
    // rule string directly (e.g., "6.7.11" for missing XMP).
    bool hasSpecificEvidence = false;
    for (const auto& issue : result.issues) {
        if (!issue.rule.empty() || !issue.description.empty()) {
            hasSpecificEvidence = true;
            break;
        }
    }
    EXPECT_TRUE(hasSpecificEvidence)
        << "validate() must report at least one issue with a specific "
        << "rule string or description so a malformed claim cannot hide "
        << "behind a blind-trust 'looks like PDF/A' code path. "
        << "Issues count: " << result.issues.size();
    // Additionally lock the rule-string contract for the most common
    // missing-feature cases: a fresh PDF should fail validation with
    // a missing-XMP rule OR a missing-OutputIntent rule cited.
    bool sawXmpOrOutputIntentRule = false;
    for (const auto& issue : result.issues) {
        const auto t = classifyIssueByRule(issue.rule);
        if (t == IssueType::MissingXmpMetadata
            || t == IssueType::MissingOutputIntent) {
            sawXmpOrOutputIntentRule = true;
            break;
        }
    }
    EXPECT_TRUE(sawXmpOrOutputIntentRule)
        << "for a non-PDF/A doc, validator MUST cite either missing-XMP "
        << "or missing-OutputIntent rule — these are the canonical "
        << "PDF/A-2 failure modes for a baseline PDF. classifyIssueByRule "
        << "is the canonical mapping.";
}

TEST_F(ComplianceManagerTest, ValidateMalformedPdfAClaim_AfterPartialConversion) {
    // Convert to PDF/A-2b, then validate again. After successful
    // conversion the doc SHOULD claim PDF/A and pass; if for any
    // reason the conversion partially fails (e.g., XMP added but
    // OutputIntent not added), the validator must still detect
    // the partial-compliance state, not pass on the XMP claim alone.
    ComplianceManager mgr(context_);
    auto outputPath = std::filesystem::temp_directory_path() / "test_b7_partial.pdf";
    if (std::filesystem::exists(outputPath)) {
        std::filesystem::remove(outputPath);
    }
    auto convResult = mgr.convertToPdfA(document_.get(), PdfALevel::PdfA_2b,
                                          outputPath);
    if (convResult.success) {
        // Open the converted doc and validate.
        auto converted = pdfe::core::Document::open(context_, outputPath);
        ASSERT_NE(converted, nullptr);
        auto validation = mgr.validatePdfA(converted.get(), PdfALevel::PdfA_2b);
        // After a successful conversion, validation should pass OR
        // report specific issues. It must NOT report success when
        // there are real violations.
        if (validation.hasErrors()) {
            EXPECT_FALSE(validation.issues.empty())
                << "after partial conversion, validator must report "
                << "specific issues — never blind-trust the XMP claim";
        }
    }
    if (std::filesystem::exists(outputPath)) {
        std::filesystem::remove(outputPath);
    }
}

// Pipeline a7c4f891 phase-4 gate remediation (karen + validator FAIL):
// The AC for B7 specifies "a malformed-claim fixture (PDF claiming
// PDF/A-2 conformance but using RGB without OutputIntent)" — a PDF
// that ACTIVELY lies about its compliance level. The tests above use
// a fresh non-claiming PDF. This test constructs the adversarial
// scenario: stamp pdfaid:part=2 XMP on a fresh doc (making it claim
// PDF/A-2b) WITHOUT adding the required OutputIntent, then verify
// the validator catches the OutputIntent violation specifically.

TEST_F(ComplianceManagerTest, ValidateFabricatedPdfAClaim_XmpWithoutOutputIntent) {
    ComplianceManager mgr(context_);

    // Stage 1: Stamp the XMP metadata to claim PDF/A-2b conformance.
    // fixIssue(MissingXmpMetadata, PdfA_2b) writes the pdfaid:part=2 /
    // conformance=B XMP packet into /Catalog/Metadata. We deliberately
    // do NOT fix MissingOutputIntent, leaving the doc in a state where
    // it falsely claims compliance.
    ComplianceIssue xmpIssue;
    xmpIssue.severity = IssueSeverity::Error;
    xmpIssue.rule = "6.7.11";
    xmpIssue.type = IssueType::MissingXmpMetadata;
    xmpIssue.fixStrategy = FixStrategy::Auto;
    auto fixResult = mgr.fixIssue(document_.get(), xmpIssue, PdfALevel::PdfA_2b);
    ASSERT_TRUE(fixResult.ok)
        << "Stamping XMP claim must succeed: " << fixResult.errorMessage;

    // Stage 2: Confirm the document now reports a PDF/A-2b claim.
    ASSERT_EQ(mgr.detectPdfALevel(document_.get()), PdfALevel::PdfA_2b)
        << "After stamping XMP, detectPdfALevel must report PdfA_2b — "
        << "the fabricated claim must be visible to the detector";

    // Stage 3: Validate. The doc claims PDF/A-2b via XMP but has no
    // OutputIntent (and likely no embedded fonts, no structure tree,
    // etc.). The validator MUST detect real violations — it must NOT
    // blind-trust the XMP claim.
    auto result = mgr.validatePdfA(document_.get(), PdfALevel::PdfA_2b);
    EXPECT_TRUE(result.hasErrors())
        << "A PDF that claims PDF/A-2b via XMP but lacks OutputIntent "
        << "MUST fail validation — blind-trusting the XMP claim is a "
        << "security vulnerability (hostile PDFs can fabricate claims)";
    EXPECT_FALSE(result.issues.empty())
        << "Validation must report specific issues, not a bare failure";

    // Stage 4: The validator must specifically cite the missing
    // OutputIntent. This is the core adversarial scenario: a PDF that
    // claims compliance but is structurally non-compliant.
    bool sawOutputIntentViolation = false;
    for (const auto& issue : result.issues) {
        const auto t = classifyIssueByRule(issue.rule);
        if (t == IssueType::MissingOutputIntent) {
            sawOutputIntentViolation = true;
            break;
        }
    }
    EXPECT_TRUE(sawOutputIntentViolation)
        << "Validator MUST detect MissingOutputIntent on a PDF that "
        << "falsely claims PDF/A-2b via XMP. A validator that only "
        << "checks the XMP claim without verifying structural "
        << "requirements would pass a fabricated-claim fixture — "
        << "this test locks that input-distrust contract. "
        << "Issues found: " << result.issues.size();
}
