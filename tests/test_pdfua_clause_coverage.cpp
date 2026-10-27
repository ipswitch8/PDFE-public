// -*- coding: utf-8 -*-
//
// PDF/UA clause coverage — GitHub #14.
//
// One locally-authored NEGATIVE fixture per PDF/UA clause PDFE's own validator
// can emit, plus one control that the validator accepts. Same construction as
// tests/test_pdfa_isartor_corpus.cpp and for the same reasons; read that file's
// header for why a hand-authored fixture is not a self-consistent oracle when
// the control is load-bearing and the clause list is parsed out of the product.
//
// ONE HONEST ASYMMETRY, STATED UP FRONT -- AND NOW RESOLVED (GitHub #194).
// #14 asks that "every clause-negative fixture correctly FAILS validation".
// The clauses PdfUAConverter::validatePdfUA emits behave like this:
//
//     7.1  no /StructTreeRoot                    Error   -> validation fails
//     7.2  no /Lang                              Error   -> validation fails
//     7.3  /Figure without /Alt                  Error   -> validation fails
//     7.16 encrypted, a11y extraction PERMITTED  Warning -> validation succeeds
//     7.16 encrypted, a11y extraction DENIED     Error   -> validation fails
//
// The 7.16 row SPLIT. This file used to record the whole clause as Warning-only
// and note that whether encryption blocking assistive technology should be an
// Error was an open question (raised in
// docs/audit/p85-remaining-open/phase-3/notes.md, filed as GitHub #194).
// It has now been decided, in the product and not under cover of a test change:
// the severity depends on ISO 32000-1 Table 22 bit 10, not on /Encrypt being
// present. The reasoning is at the 7.16 block in PdfUAConverter.cpp.
//
// The corpus fixture `pdfua_7_16_encrypted.pdf` is written with ENC_PERMS = -4
// (every permission bit set, see tests/fixtures/gen_conformance_corpus.py), so
// it is the PERMITTED case: it still reports 7.16 at Warning and still
// validates, and its row in uaCases() below is unchanged and still correct.
// The DENIED case cannot be a static corpus fixture written by that generator,
// so it is built at run time from the control document by
// PdfUA716PermissionTest at the end of this file -- as a matched pair, one
// permission bit apart, which is what makes either case evidence about the bit.

#include <gtest/gtest.h>

#include "core/MuPdfContext.h"
#include "core/Document.h"
#include "compliance/PdfUAConverter.h"
#include "compliance/StructureTypes.h"
#include "compliance/ComplianceTypes.h"
#include "encryption/EncryptionManager.h"
#include "encryption/EncryptionSettings.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace pdfe::compliance;
namespace fs = std::filesystem;

namespace {

fs::path conformanceDir() {
    return fs::path(TEST_FIXTURES_DIR) / "flaw_corpus" / "conformance";
}

std::string readFileText(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

struct UaClauseCase {
    const char* file;
    const char* clause;
    IssueSeverity severity;
    bool mustFailValidation;   // false only for the Warning-severity clause
    const char* what;
};

const std::vector<UaClauseCase>& uaCases() {
    static const std::vector<UaClauseCase> cases = {
        {"pdfua/pdfua_7_1_no_structure_tree.pdf", "7.1", IssueSeverity::Error,
         true, "/Catalog /StructTreeRoot removed"},
        {"pdfua/pdfua_7_2_no_document_language.pdf", "7.2", IssueSeverity::Error,
         true, "/Catalog /Lang removed"},
        {"pdfua/pdfua_7_3_figure_without_alt.pdf", "7.3", IssueSeverity::Error,
         true, "/Figure structure element carries neither /Alt nor /ActualText"},
        // GitHub #194: this fixture is the accessibility-PERMITTED case
        // (ENC_PERMS = -4 grants every bit, including Table 22 bit 10), so
        // Warning/succeeds is the decided-correct verdict for it, not a
        // pinned-pending-decision one. The DENIED case is PdfUA716PermissionTest
        // at the foot of this file.
        {"pdfua/pdfua_7_16_encrypted.pdf", "7.16", IssueSeverity::Warning,
         false, "RC4-128 standard security handler over the control document, "
                "accessibility extraction permitted"},
    };
    return cases;
}

constexpr const char* kUaControl = "pdfua/pdfua_control.pdf";

std::string issueDump(const ComplianceResult& r) {
    std::ostringstream o;
    o << "[" << r.issues.size() << " issue(s)]";
    for (const auto& i : r.issues) {
        o << "\n    " << (i.severity == IssueSeverity::Error ? "ERROR  " : "warning")
          << " " << i.rule << "  " << i.description;
    }
    return o.str();
}

bool hasIssue(const ComplianceResult& r, const std::string& clause,
              IssueSeverity sev) {
    return std::any_of(r.issues.begin(), r.issues.end(),
                       [&](const ComplianceIssue& i) {
                           return i.rule == clause && i.severity == sev;
                       });
}

class PdfUAClauseCoverageTest : public ::testing::Test {
protected:
    void SetUp() override {
        context_ = std::make_shared<pdfe::core::MuPdfContext>();
        ASSERT_TRUE(fs::exists(conformanceDir()))
            << "the conformance corpus is missing at " << conformanceDir().string()
            << "; regenerate with "
               "`python tests/fixtures/gen_conformance_corpus.py`";
    }
    void TearDown() override { context_.reset(); }

    std::unique_ptr<pdfe::core::Document> open(const std::string& rel) {
        const fs::path p = conformanceDir() / rel;
        EXPECT_TRUE(fs::exists(p)) << "missing corpus fixture " << p.string();
        if (!fs::exists(p)) return nullptr;
        return pdfe::core::Document::open(context_, p);
    }

    std::shared_ptr<pdfe::core::MuPdfContext> context_;
};

// ---------------------------------------------------------------------------
// 1. The control. Without this, every case below could pass against a validator
//    that rejects every document it is shown.
// ---------------------------------------------------------------------------

TEST_F(PdfUAClauseCoverageTest, ControlValidatesWithZeroIssues) {
    auto doc = open(kUaControl);
    ASSERT_NE(doc, nullptr);

    PdfUAConverter converter(context_);
    const auto result = converter.validatePdfUA(doc.get());

    EXPECT_EQ(result.errorCount(), 0)
        << kUaControl << " is the control: a structure tree, a /Lang, and a "
           "/Figure carrying /Alt. If it does not validate, every negative "
           "case in this file is measuring nothing. " << issueDump(result);
    EXPECT_EQ(result.warningCount(), 0)
        << kUaControl << " must also raise no warnings -- the 7.16 case below "
           "distinguishes itself from the control by a warning, so a control "
           "that already warns makes that comparison meaningless. "
        << issueDump(result);
    EXPECT_TRUE(result.success) << issueDump(result);
}

// ---------------------------------------------------------------------------
// 2. Each clause-negative fixture reports ITS clause.
// ---------------------------------------------------------------------------

TEST_F(PdfUAClauseCoverageTest, EveryClauseNegativeReportsItsOwnClause) {
    ASSERT_FALSE(uaCases().empty());
    int checked = 0;
    for (const auto& c : uaCases()) {
        SCOPED_TRACE(std::string(c.file) + " (clause " + c.clause + ": "
                     + c.what + ")");
        auto doc = open(c.file);
        ASSERT_NE(doc, nullptr) << c.file << " must open";

        PdfUAConverter converter(context_);
        const auto result = converter.validatePdfUA(doc.get());

        EXPECT_TRUE(hasIssue(result, c.clause, c.severity))
            << c.file << " must report ISO 14289-1 clause " << c.clause
            << " at "
            << (c.severity == IssueSeverity::Error ? "Error" : "Warning")
            << " severity. " << issueDump(result);

        if (c.mustFailValidation) {
            EXPECT_FALSE(result.success)
                << c.file << " must FAIL PDF/UA validation. " << issueDump(result);
        } else {
            // Documented asymmetry -- see this file's header. Asserted in the
            // positive direction so the case still measures something: the
            // product currently treats this clause as non-fatal, and that fact
            // is pinned rather than left implicit.
            EXPECT_TRUE(result.success)
                << c.file << " reports clause " << c.clause
                << " at Warning severity, so validation is expected to SUCCEED. "
                   "If this now fails, the product has reclassified the clause "
                   "as an Error -- update this table and the file header, do "
                   "not delete the case. " << issueDump(result);
        }
        ++checked;
    }
    EXPECT_EQ(checked, static_cast<int>(uaCases().size()));
}

// ---------------------------------------------------------------------------
// 3. The control does not report any clause a negative reports.
// ---------------------------------------------------------------------------

TEST_F(PdfUAClauseCoverageTest, ControlReportsNoneOfTheClauses) {
    auto doc = open(kUaControl);
    ASSERT_NE(doc, nullptr);

    PdfUAConverter converter(context_);
    const auto result = converter.validatePdfUA(doc.get());

    for (const auto& c : uaCases()) {
        EXPECT_FALSE(hasIssue(result, c.clause, c.severity))
            << "the control reports clause " << c.clause << ", which only "
            << c.file << " is supposed to exhibit. A rule that fires on the "
               "control fires on everything. " << issueDump(result);
    }
}

// ---------------------------------------------------------------------------
// 4. The 7.3 rule reads the figure's description, not merely its presence.
// ---------------------------------------------------------------------------
//
// GitHub #172's fix reads StructureNode::needsAltText. The control and the 7.3
// negative differ ONLY in the /Alt string on the figure element, so this pair
// is the tightest available check that the rule reads the description rather
// than "is there a /Figure at all".

TEST_F(PdfUAClauseCoverageTest, TheOnlyDifferenceForClause73IsTheAltString) {
    auto withAlt = open(kUaControl);
    auto without = open("pdfua/pdfua_7_3_figure_without_alt.pdf");
    ASSERT_NE(withAlt, nullptr);
    ASSERT_NE(without, nullptr);

    PdfUAConverter converter(context_);

    const auto a = converter.readStructureTree(withAlt.get());
    const auto b = converter.readStructureTree(without.get());

    // Non-vacuity: both documents must actually HAVE a figure, or the
    // comparison below is between two empty sets.
    const auto countFigures = [](const StructureNode& n) {
        int total = 0;
        std::vector<const StructureNode*> stack{&n};
        while (!stack.empty()) {
            const StructureNode* cur = stack.back();
            stack.pop_back();
            if (cur->tag == StructureTag::Figure) ++total;
            for (const auto& ch : cur->children)
                if (ch) stack.push_back(ch.get());
        }
        return total;
    };
    ASSERT_EQ(countFigures(a), 1)
        << "the control must contain exactly one /Figure; it contains "
        << countFigures(a);
    ASSERT_EQ(countFigures(b), 1)
        << "the 7.3 negative must contain exactly one /Figure; it contains "
        << countFigures(b);

    EXPECT_TRUE(PdfUAConverter::figuresMissingAltText(a).empty())
        << "the control's figure carries /Alt and must not be reported";
    EXPECT_EQ(PdfUAConverter::figuresMissingAltText(b).size(), 1u)
        << "the negative's figure carries no /Alt and must be reported exactly "
           "once";
}

// ---------------------------------------------------------------------------
// 5. Coverage, derived from the product source.
// ---------------------------------------------------------------------------
//
// PdfUAConverter emits two clauses as literals ("7.1", "7.2") and two through
// named constants (kRuleGraphicsAltText, kRuleSecurity). Both forms are
// resolved here -- the constants by looking their VALUES up in
// PdfUAConverter.h -- so a clause cannot hide behind an indirection.

std::set<std::string> clausesEmittedByPdfUaSource() {
    const fs::path hdr = fs::path(PDFE_REPO_ROOT) / "src" / "compliance" /
                         "PdfUAConverter.h";
    const fs::path src = fs::path(PDFE_REPO_ROOT) / "src" / "compliance" /
                         "PdfUAConverter.cpp";
    const std::string hdrText = readFileText(hdr);
    const std::string srcText = readFileText(src);
    EXPECT_FALSE(hdrText.empty()) << "cannot read " << hdr.string();
    EXPECT_FALSE(srcText.empty()) << "cannot read " << src.string();

    // constexpr const char* kName = "7.3";
    std::map<std::string, std::string> constants;
    const std::regex constRe(
        R"RE(constexpr\s+const\s+char\*\s+(k[A-Za-z0-9_]+)\s*=\s*"([0-9]+(?:\.[0-9]+)*)")RE");
    for (auto it = std::sregex_iterator(hdrText.begin(), hdrText.end(), constRe);
         it != std::sregex_iterator(); ++it) {
        constants[(*it)[1].str()] = (*it)[2].str();
    }

    std::set<std::string> out;
    // Only look inside validatePdfUA -- convertAndSave emits progress issues
    // that are not clause verdicts.
    const std::size_t from = srcText.find("ComplianceResult PdfUAConverter::validatePdfUA");
    EXPECT_NE(from, std::string::npos)
        << "validatePdfUA not found in " << src.string()
        << "; the clause scan below would run over the wrong region";
    const std::string body =
        from == std::string::npos ? std::string() : srcText.substr(from);

    const std::regex litRe(
        R"RE(IssueSeverity::(?:Error|Warning),\s*\r?\n\s*"([0-9]+(?:\.[0-9]+)*)")RE");
    for (auto it = std::sregex_iterator(body.begin(), body.end(), litRe);
         it != std::sregex_iterator(); ++it) {
        out.insert((*it)[1].str());
    }
    const std::regex nameRe(
        R"RE(IssueSeverity::(?:Error|Warning),\s*\r?\n\s*(k[A-Za-z0-9_]+))RE");
    for (auto it = std::sregex_iterator(body.begin(), body.end(), nameRe);
         it != std::sregex_iterator(); ++it) {
        const auto found = constants.find((*it)[1].str());
        EXPECT_NE(found, constants.end())
            << "validatePdfUA emits the constant " << (*it)[1].str()
            << " but PdfUAConverter.h does not define its clause value";
        if (found != constants.end()) out.insert(found->second);
    }
    return out;
}

TEST_F(PdfUAClauseCoverageTest, EveryClauseValidatePdfUACanEmitHasAFixture) {
    const std::set<std::string> emitted = clausesEmittedByPdfUaSource();
    ASSERT_FALSE(emitted.empty())
        << "parsed ZERO clause ids out of PdfUAConverter; a coverage check "
           "that parses nothing passes vacuously";
    ASSERT_GE(emitted.size(), 2u)
        << "only " << emitted.size() << " clause id(s) parsed; validatePdfUA "
           "emits both literal and named-constant clauses and the scan must "
           "resolve both shapes";

    std::set<std::string> covered;
    for (const auto& c : uaCases()) covered.insert(c.clause);

    std::vector<std::string> uncovered, phantom;
    std::set_difference(emitted.begin(), emitted.end(), covered.begin(),
                        covered.end(), std::back_inserter(uncovered));
    std::set_difference(covered.begin(), covered.end(), emitted.begin(),
                        emitted.end(), std::back_inserter(phantom));

    std::ostringstream u, p;
    for (const auto& s : uncovered) u << s << " ";
    for (const auto& s : phantom) p << s << " ";

    EXPECT_TRUE(uncovered.empty())
        << "PdfUAConverter::validatePdfUA can emit these clauses but no "
           "negative fixture exercises them: " << u.str()
        << "\nAdd one to tests/fixtures/gen_conformance_corpus.py and to "
           "uaCases() above.";
    EXPECT_TRUE(phantom.empty())
        << "these clauses have fixtures but validatePdfUA can no longer emit "
           "them: " << p.str();
}

// ---------------------------------------------------------------------------
// 6. GitHub #194 — 7.16's severity is decided by the PERMISSION BIT.
// ---------------------------------------------------------------------------
//
// The decision recorded in PdfUAConverter.cpp is reading (2) of the issue, made
// permission-aware: ISO 14289-1 7.16 forbids a security handler that stops
// assistive technology reading the content, not encryption as such. The bit
// that expresses that is ISO 32000-1 Table 22 bit 10 (PDF_PERM_ACCESSIBILITY).
//
// These two cases are a MATCHED PAIR built from the same control document by
// the same encryptor, differing ONLY in `permissions.accessibility`. That is
// what makes them evidence about the bit rather than about encryption: if the
// product reverted to a blanket Warning, the denied case goes green-as-Warning
// and the first test below reddens; if it reverted to a blanket Error, the
// permitted case reddens instead. Neither mutant survives both.
//
// RC4-128 (R3) is mandatory here and is not an arbitrary choice: QPDF refuses
// to clear bit 10 above R3 because PDF 2.0 deprecates it, which
// PermissionBitsRoundTrip.AccessibilityBitIsNotExpressibleAboveR3 already pins.
// At R4/R6 the denied variant would be indistinguishable from the permitted
// one and this pair would measure nothing.

fs::path uaScratchDir() {
    auto d = fs::temp_directory_path() / "pdfe_pdfua_7_16";
    fs::create_directories(d);
    return d;
}

class PdfUA716PermissionTest : public PdfUAClauseCoverageTest {
protected:
    // The control document, re-saved through the RC4-128 handler with bit 10
    // set or cleared. Empty user password, so the reopen below observes the
    // RESTRICTED view -- opening with the owner password grants everything and
    // would make the permission check vacuous.
    fs::path encryptedControl(const char* name, bool accessibilityAllowed) {
        auto src = open(kUaControl);
        EXPECT_NE(src, nullptr);
        if (!src) return {};

        pdfe::encryption::EncryptionSettings settings;
        settings.algorithm = pdfe::encryption::EncryptionAlgorithm::RC4_128;
        settings.userPassword = "";
        settings.ownerPassword = "ownerpw";
        settings.permissions.accessibility = accessibilityAllowed;

        const auto out = uaScratchDir() / name;
        std::error_code ec;
        fs::remove(out, ec);
        pdfe::encryption::EncryptionManager mgr(context_);
        mgr.saveEncrypted(src.get(), out, settings);
        EXPECT_TRUE(fs::exists(out)) << "failed to write " << out.string();
        return out;
    }

    std::unique_ptr<pdfe::core::Document> openPath(const fs::path& p) {
        if (p.empty() || !fs::exists(p)) return nullptr;
        return pdfe::core::Document::open(context_, p);
    }
};

TEST_F(PdfUA716PermissionTest, AccessibilityDeniedIsAnErrorAndFailsValidation) {
    const auto p = encryptedControl("ua716_a11y_denied.pdf", false);
    auto doc = openPath(p);
    ASSERT_NE(doc, nullptr) << "could not reopen " << p.string();

    // Non-vacuity, both directions. Without these the test could pass because
    // the document is not encrypted at all, or because the bit survived as
    // granted and some UNRELATED clause failed validation.
    ASSERT_FALSE(doc->extractForAccessibilityAllowed())
        << "the handler did not actually clear ISO 32000-1 Table 22 bit 10, so "
           "this document is not the case clause 7.16 is about and the verdict "
           "below would be about something else";

    PdfUAConverter converter(context_);
    const auto result = converter.validatePdfUA(doc.get());

    EXPECT_TRUE(hasIssue(result, "7.16", IssueSeverity::Error))
        << "an encrypted document that DENIES accessibility extraction must "
           "report ISO 14289-1 clause 7.16 at Error severity (GitHub #194). "
        << issueDump(result);
    EXPECT_FALSE(hasIssue(result, "7.16", IssueSeverity::Warning))
        << "7.16 must be reported once, as an Error -- not also as the old "
           "blanket warning. " << issueDump(result);
    EXPECT_FALSE(result.success)
        << "ComplianceResult::success is !hasErrors(), so the Error above must "
           "make validatePdfUA() fail. A pass here is the false PASS #194 is "
           "about. " << issueDump(result);

    // 7.16 must be the ONLY reason it failed: the source is the control, which
    // ControlValidatesWithZeroIssues asserts is otherwise clean. If encryption
    // also destroyed the structure tree or /Lang, the failure above would be
    // real but for the wrong clause.
    for (const auto& i : result.issues) {
        if (i.severity != IssueSeverity::Error) continue;
        EXPECT_EQ(i.rule, std::string("7.16"))
            << "the encrypted control failed for clause " << i.rule
            << " as well, so the 7.16 verdict is not isolated. " << issueDump(result);
    }
}

TEST_F(PdfUA716PermissionTest, AccessibilityPermittedStaysAWarningAndPasses) {
    const auto p = encryptedControl("ua716_a11y_permitted.pdf", true);
    auto doc = openPath(p);
    ASSERT_NE(doc, nullptr) << "could not reopen " << p.string();

    ASSERT_TRUE(doc->extractForAccessibilityAllowed())
        << "bit 10 is not set, so this is the DENIED case and the pair below "
           "no longer differs only in the permission bit";

    PdfUAConverter converter(context_);
    const auto result = converter.validatePdfUA(doc.get());

    EXPECT_TRUE(hasIssue(result, "7.16", IssueSeverity::Warning))
        << "encryption that still permits accessibility extraction is "
           "conformant, but worth reporting -- Warning, not Error, and not "
           "silence. " << issueDump(result);
    EXPECT_FALSE(hasIssue(result, "7.16", IssueSeverity::Error))
        << "reading (1) of #194 is correct for this case: failing every "
           "password-protected PDF/UA document would be a false positive. "
        << issueDump(result);
    EXPECT_TRUE(result.success)
        << "an encrypted but accessibility-permitting document must still "
           "validate. " << issueDump(result);
}

// The discriminator, stated as one assertion rather than inferred from the two
// tests above passing separately: the SAME source, the SAME handler, one bit
// apart, must produce two different verdicts. A blanket severity of either kind
// makes these equal.
TEST_F(PdfUA716PermissionTest, TheOnlyDifferenceForClause716IsThePermissionBit) {
    auto denied = openPath(encryptedControl("ua716_pair_denied.pdf", false));
    auto allowed = openPath(encryptedControl("ua716_pair_allowed.pdf", true));
    ASSERT_NE(denied, nullptr);
    ASSERT_NE(allowed, nullptr);
    ASSERT_NE(denied->extractForAccessibilityAllowed(),
              allowed->extractForAccessibilityAllowed())
        << "both variants report the same accessibility permission, so they are "
           "not a matched pair and nothing below is a comparison";

    PdfUAConverter converter(context_);
    const auto rDenied = converter.validatePdfUA(denied.get());
    const auto rAllowed = converter.validatePdfUA(allowed.get());

    EXPECT_NE(rDenied.success, rAllowed.success)
        << "the accessibility permission bit must change the PDF/UA verdict. "
           "denied=" << issueDump(rDenied) << "\nallowed=" << issueDump(rAllowed);
    EXPECT_FALSE(rDenied.success);
    EXPECT_TRUE(rAllowed.success);
}

}  // namespace
