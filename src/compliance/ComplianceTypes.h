#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace pdfe::compliance {

/// PDF/A conformance levels.
enum class PdfALevel {
    None,
    PdfA_1b,   ///< ISO 19005-1, conformance B (visual)
    PdfA_1a,   ///< ISO 19005-1, conformance A (accessible)
    PdfA_2b,   ///< ISO 19005-2, conformance B (visual)
    PdfA_2u,   ///< ISO 19005-2, conformance U (Unicode)
    PdfA_2a,   ///< ISO 19005-2, conformance A (Accessible)
    PdfA_3b,   ///< ISO 19005-3, conformance B (with attachments)
    PdfA_3u,   ///< ISO 19005-3, conformance U (Unicode + attachments)
    PdfA_3a    ///< ISO 19005-3, conformance A (accessible + attachments)
};

/// PDF/UA conformance levels.
enum class PdfUALevel {
    None,
    PdfUA_1,   ///< ISO 14289-1
    PdfUA_2    ///< ISO 14289-2
};

/// Severity of a compliance issue.
enum class IssueSeverity {
    Error,     ///< Must fix for compliance
    Warning    ///< Should fix, but not strictly required
};

/// Pipeline E phase-12: typed categorisation of compliance findings,
/// used by ComplianceManager::autoFixableIssues / fixAll.
/// Unknown is the safe default for legacy issues whose rule string
/// hasn't been mapped yet — they will be classified as Manual fix.
enum class IssueType {
    Unknown,
    MissingXmpMetadata,         ///< /Catalog /Metadata absent OR XMP lacks pdfaid
    MissingOutputIntent,        ///< /Catalog /OutputIntents absent or no GTS_PDFA1
    ContainsJavaScript,         ///< /Names /JavaScript or /AA actions
    DocumentEncrypted,          ///< Encryption dict present
    FontsNotEmbedded,           ///< /FontFile / /FontFile2 / /FontFile3 missing
    ContainsTransparency,       ///< /Group /S /Transparency or SMask usage
    ContainsLZWCompression      ///< /Filter /LZWDecode in any stream
};

/// Pipeline E phase-12: per-issue strategy for the Fix-All workflow.
/// Auto means ComplianceManager has a fixer that mutates an open
/// Document in-place; Manual means the user has to act (e.g. supply a
/// password to remove encryption, embed missing fonts).
enum class FixStrategy {
    Auto,
    Manual
};

/// A single compliance issue found during validation.
/// New fields (type, fixStrategy) are placed AFTER the legacy positional
/// fields so that existing brace-initializers in PdfAValidator etc. keep
/// compiling — those callers don't need to set the type explicitly,
/// classifyIssueByRule() resolves it on-demand inside fixAll().
struct ComplianceIssue {
    IssueSeverity severity = IssueSeverity::Error;
    std::string rule;          ///< ISO clause reference, e.g. "6.2.11.3"
    std::string description;   ///< Human-readable description
    int pageIndex = -1;        ///< -1 for document-level issues
    std::string objectRef;     ///< Optional PDF object reference
    IssueType type = IssueType::Unknown;            ///< Pipeline E phase-12
    FixStrategy fixStrategy = FixStrategy::Manual;  ///< Pipeline E phase-12
};

/// Pipeline E phase-12: classify an issue's IssueType from its rule
/// string. Validators that haven't yet been migrated to the typed API
/// can keep emitting rule strings; this function maps them to the
/// canonical type used by the fix-all workflow.
///
/// Uses **exact rule matches** rather than ISO-prefix scans so that
/// adjacent sub-rules don't get falsely routed to the wrong fixer.
/// For example, ProhibitedFeatureScanner emits both `6.6.1`
/// (JavaScript) and `6.6.3` (external Launch/Sound/Movie/GoToR
/// actions) — `6.6.3` MUST NOT route to ContainsJavaScript or the JS
/// stripper would silently report success while leaving the external
/// references untouched. Unknown rules fall through to
/// IssueType::Unknown (which defaults to FixStrategy::Manual) — that
/// is the safe direction for the fix-all workflow.
inline IssueType classifyIssueByRule(const std::string& rule) {
    // PdfAValidator XMP / pdfaid clauses — clause 6.7 is the
    // metadata family; current emitters use 6.7.11 (and 6.7.3 in
    // older paths). Keep both as exact matches.
    if (rule == "6.7" || rule == "6.7.3" || rule == "6.7.11") {
        return IssueType::MissingXmpMetadata;
    }
    if (rule == "6.2.2") {
        return IssueType::MissingOutputIntent;
    }
    // 6.6.1 = JavaScript. 6.6.3 = external refs (NOT a JS issue).
    if (rule == "6.6.1") {
        return IssueType::ContainsJavaScript;
    }
    // 6.1.3 = encryption. 6.1.7 = object streams (different issue,
    // no auto-fixer; leave as Unknown → Manual).
    if (rule == "6.1.3") {
        return IssueType::DocumentEncrypted;
    }
    if (rule == "6.3.3") {
        return IssueType::FontsNotEmbedded;
    }
    if (rule == "6.4") {
        return IssueType::ContainsTransparency;
    }
    if (rule == "6.5") {
        return IssueType::ContainsLZWCompression;
    }
    return IssueType::Unknown;
}

/// Default strategy for an IssueType. Used when validators don't set
/// fixStrategy explicitly.
inline FixStrategy defaultFixStrategy(IssueType t) {
    switch (t) {
    case IssueType::MissingXmpMetadata:
    case IssueType::MissingOutputIntent:
    case IssueType::ContainsJavaScript:
        return FixStrategy::Auto;
    case IssueType::DocumentEncrypted:
    case IssueType::FontsNotEmbedded:
    case IssueType::ContainsTransparency:
    case IssueType::ContainsLZWCompression:
    case IssueType::Unknown:
    default:
        return FixStrategy::Manual;
    }
}

/// Human-readable name for an IssueType (for UI / reports).
inline std::string issueTypeName(IssueType t) {
    switch (t) {
    case IssueType::MissingXmpMetadata:    return "Missing XMP metadata";
    case IssueType::MissingOutputIntent:   return "Missing OutputIntent";
    case IssueType::ContainsJavaScript:    return "Contains JavaScript";
    case IssueType::DocumentEncrypted:     return "Document is encrypted";
    case IssueType::FontsNotEmbedded:      return "Fonts not embedded";
    case IssueType::ContainsTransparency:  return "Contains transparency";
    case IssueType::ContainsLZWCompression:return "Contains LZW compression";
    case IssueType::Unknown:
    default:                                return "Unknown";
    }
}

/// Result of a compliance validation or conversion attempt.
struct ComplianceResult {
    bool success = false;
    PdfALevel targetLevel = PdfALevel::None;
    PdfUALevel targetUALevel = PdfUALevel::None;
    std::vector<ComplianceIssue> issues;
    std::string errorMessage;

    bool hasErrors() const {
        return std::any_of(issues.begin(), issues.end(),
            [](const ComplianceIssue& i) {
                return i.severity == IssueSeverity::Error;
            });
    }

    bool hasWarnings() const {
        return std::any_of(issues.begin(), issues.end(),
            [](const ComplianceIssue& i) {
                return i.severity == IssueSeverity::Warning;
            });
    }

    int errorCount() const {
        return static_cast<int>(std::count_if(
            issues.begin(), issues.end(),
            [](const ComplianceIssue& i) {
                return i.severity == IssueSeverity::Error;
            }));
    }

    int warningCount() const {
        return static_cast<int>(std::count_if(
            issues.begin(), issues.end(),
            [](const ComplianceIssue& i) {
                return i.severity == IssueSeverity::Warning;
            }));
    }
};

/// Font compliance information for a single font.
struct FontComplianceInfo {
    std::string fontName;
    bool isEmbedded = false;
    bool hasGlyphWidths = false;
    bool hasToUnicode = false;       ///< Required for PDF/A-2u and above
    bool isSymbolic = false;
    int pageIndex = -1;
    std::vector<std::string> issues;
};

/// Get the ISO part number for a PDF/A level.
inline int pdfaPartNumber(PdfALevel level) {
    switch (level) {
        case PdfALevel::PdfA_1b:
        case PdfALevel::PdfA_1a: return 1;
        case PdfALevel::PdfA_2b:
        case PdfALevel::PdfA_2u:
        case PdfALevel::PdfA_2a: return 2;
        case PdfALevel::PdfA_3b:
        case PdfALevel::PdfA_3u:
        case PdfALevel::PdfA_3a: return 3;
        default: return 0;
    }
}

/// Get the conformance letter for a PDF/A level.
inline std::string pdfaConformance(PdfALevel level) {
    switch (level) {
        case PdfALevel::PdfA_1b:
        case PdfALevel::PdfA_2b:
        case PdfALevel::PdfA_3b: return "B";
        case PdfALevel::PdfA_2u:
        case PdfALevel::PdfA_3u: return "U";
        case PdfALevel::PdfA_1a:
        case PdfALevel::PdfA_2a:
        case PdfALevel::PdfA_3a: return "A";
        default: return "";
    }
}

/// Get a human-readable name for a PDF/A level.
inline std::string pdfaLevelName(PdfALevel level) {
    switch (level) {
        case PdfALevel::PdfA_1b: return "PDF/A-1b";
        case PdfALevel::PdfA_1a: return "PDF/A-1a";
        case PdfALevel::PdfA_2b: return "PDF/A-2b";
        case PdfALevel::PdfA_2u: return "PDF/A-2u";
        case PdfALevel::PdfA_2a: return "PDF/A-2a";
        case PdfALevel::PdfA_3b: return "PDF/A-3b";
        case PdfALevel::PdfA_3u: return "PDF/A-3u";
        case PdfALevel::PdfA_3a: return "PDF/A-3a";
        default: return "None";
    }
}

/// Does this level require all text to have ToUnicode mappings?
inline bool levelRequiresUnicode(PdfALevel level) {
    return level == PdfALevel::PdfA_2u || level == PdfALevel::PdfA_2a
        || level == PdfALevel::PdfA_3u || level == PdfALevel::PdfA_3a;
}

/// Does this level require a Tagged PDF structure tree?
inline bool levelRequiresTagging(PdfALevel level) {
    return level == PdfALevel::PdfA_1a || level == PdfALevel::PdfA_2a
        || level == PdfALevel::PdfA_3a;
}

/// Does this level allow transparency?
inline bool levelAllowsTransparency(PdfALevel level) {
    return level != PdfALevel::PdfA_1b && level != PdfALevel::PdfA_1a;
}

/// Does this level allow object streams?
inline bool levelAllowsObjStreams(PdfALevel level) {
    return level != PdfALevel::PdfA_1b && level != PdfALevel::PdfA_1a;
}

} // namespace pdfe::compliance
