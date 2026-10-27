#pragma once

#include "ComplianceTypes.h"
#include "StructureTypes.h"
#include <memory>
#include <filesystem>
#include <string>
#include <vector>

namespace pdfe::core {
class MuPdfContext;
class Document;
}

namespace pdfe::compliance {

class PdfUAConverter {
public:
    explicit PdfUAConverter(std::shared_ptr<pdfe::core::MuPdfContext> context);
    ~PdfUAConverter();
    PdfUAConverter(const PdfUAConverter&) = delete;
    PdfUAConverter& operator=(const PdfUAConverter&) = delete;

    ComplianceResult convertAndSave(
        pdfe::core::Document* doc,
        const std::filesystem::path& outputPath,
        const std::string& language = "en-US"
    );

    ComplianceResult validatePdfUA(pdfe::core::Document* doc);

    // ── GitHub #172 ──────────────────────────────────────────────────────
    //
    // Rule id for "a /Figure carries no alternative text". PDF/UA-1
    // (ISO 14289-1) clause 7.3 is *Graphics*, and 7.3 is the clause that
    // requires every figure to have an /Alt (or /ActualText) — so this rule
    // takes "7.3". The encryption warning that used to squat on "7.3" moves to
    // "7.16" (*Security*), which is the clause it actually belongs to. See
    // docs/audit/p83-outstanding/phase-3/notes.md for the check that nothing
    // outside the display path keys on either string.
    static constexpr const char* kRuleGraphicsAltText = "7.3";
    static constexpr const char* kRuleSecurity = "7.16";

    /// Read the document's /StructTreeRoot back into StructureNodes. A /Figure
    /// with no usable /Alt (or /ActualText) comes back with `needsAltText` set,
    /// which is the SAME flag AutoTagger raises when it cannot derive a
    /// description — so one predicate serves both the pre-save tree and the
    /// post-save document. Returns a Document-tagged root with no children when
    /// the document has no structure tree.
    StructureNode readStructureTree(pdfe::core::Document* doc);

    /// Every /Figure in `root` that is flagged as needing alternative text and
    /// has none. Reads `StructureNode::needsAltText`, which before this change
    /// was written by AutoTagger and read by nothing at all.
    static std::vector<const StructureNode*> figuresMissingAltText(
        const StructureNode& root);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pdfe::compliance
