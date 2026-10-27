#pragma once

#include "ComplianceTypes.h"
#include <memory>
#include <filesystem>
#include <string>
#include <vector>

namespace pdfe::core {
class MuPdfContext;
class Document;
}

namespace pdfe::compliance {

class ComplianceManager {
public:
    explicit ComplianceManager(std::shared_ptr<pdfe::core::MuPdfContext> context);
    ~ComplianceManager();
    ComplianceManager(const ComplianceManager&) = delete;
    ComplianceManager& operator=(const ComplianceManager&) = delete;

    ComplianceResult validatePdfA(pdfe::core::Document* doc, PdfALevel level);
    ComplianceResult validatePdfUA(pdfe::core::Document* doc);
    PdfALevel detectPdfALevel(pdfe::core::Document* doc);

    ComplianceResult convertToPdfA(
        pdfe::core::Document* doc,
        PdfALevel level,
        const std::filesystem::path& outputPath
    );

    ComplianceResult convertToPdfUA(
        pdfe::core::Document* doc,
        const std::filesystem::path& outputPath,
        const std::string& language = "en-US"
    );

    ComplianceResult convertToPdfA2aAndUA(
        pdfe::core::Document* doc,
        const std::filesystem::path& outputPath,
        const std::string& language = "en-US"
    );

    ComplianceResult analyzeForConversion(
        pdfe::core::Document* doc,
        PdfALevel level
    );

    // ----- Pipeline E phase-12: Fix-All workflow -----

    /// Result of a fixIssue / fixAll call.
    struct FixResult {
        bool ok = false;
        std::string errorMessage;
    };

    /// Aggregate result of fixAll().
    struct FixAllResult {
        int attempted = 0;
        int succeeded = 0;
        int failed = 0;
        std::vector<ComplianceIssue> stillRemaining; ///< issues after refresh
        std::vector<std::string> errorMessages;
    };

    /// Filter `issues` to only those whose fixStrategy is Auto.
    /// Static — does not require a Document.
    static std::vector<ComplianceIssue> autoFixableIssues(
        const std::vector<ComplianceIssue>& issues);

    /// Apply a single fixer for one issue. Idempotent: a second call
    /// after a successful first call is a no-op (returns ok=true).
    /// Manual issues return ok=false with a "manual fix required"
    /// message. Unknown types return ok=false with "no fixer".
    ///
    /// Two-arg overload defaults the target level to PDF/A-2b for
    /// back-compat. New code should pass an explicit level so issues
    /// like MissingXmpMetadata stamp the correct pdfaid:part.
    FixResult fixIssue(pdfe::core::Document* doc,
                       const ComplianceIssue& issue);
    FixResult fixIssue(pdfe::core::Document* doc,
                       const ComplianceIssue& issue,
                       PdfALevel level);

    /// Iterate every Auto issue in `issues` and apply its fixer.
    /// After all fixers run, re-validates against `level` to populate
    /// stillRemaining. Manual issues are NOT applied but ARE preserved
    /// in stillRemaining so callers can surface them in the UI.
    /// Returns aggregate success/failure counts.
    FixAllResult fixAll(pdfe::core::Document* doc,
                        const std::vector<ComplianceIssue>& issues,
                        PdfALevel level);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pdfe::compliance
