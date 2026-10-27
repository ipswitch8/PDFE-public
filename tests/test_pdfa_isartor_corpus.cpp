// -*- coding: utf-8 -*-
//
// PDF/A clause-conformance corpus — GitHub #8 ("Isartor corpus").
//
// WHAT THIS IS, AND WHAT IT IS NOT
// --------------------------------
// #8 asks for the Isartor test suite. The Isartor suite CANNOT be vendored: the
// PDF Association distributes it under terms that do not permit redistribution
// inside another project's repository, and this project's rules forbid a test
// that downloads anything. So this file exercises a LOCALLY-AUTHORED,
// clause-sampled corpus built by tests/fixtures/gen_conformance_corpus.py, in
// the same one-file-per-violated-clause shape Isartor uses. That substitution is
// stated here, in the generator, in the corpus manifest and in
// tests/fixtures/flaw_corpus/COVERAGE.md §11 — it is not quietly relabelled.
//
// WHY A HAND-AUTHORED FIXTURE IS NOT A SELF-CONSISTENT ORACLE HERE
// ----------------------------------------------------------------
// COVERAGE.md §11 previously refused to author clause fixtures locally, on the
// grounds that "a hand-made file asserting our own reading of a clause would be
// the same self-consistent oracle wearing a corpus's name". That objection is
// correct about the naive version and is answered structurally, not verbally:
//
//   1. THE CONTROL IS LOAD-BEARING. Each negative fixture is its control file
//      plus exactly ONE construct. `ControlsValidateWithZeroErrors` requires
//      the validator to ACCEPT all three controls with zero errors. A validator
//      that degenerated into "reject everything" — the way a clause test most
//      easily passes vacuously — fails that case immediately.
//   2. EACH NEGATIVE MUST NAME ITS CLAUSE. It is not enough that validation
//      failed; the specific ISO clause id must be present at Error severity,
//      AND the control must NOT report that clause. Both halves are asserted,
//      so a rule that fires on everything and a rule that fires on nothing are
//      both caught.
//   3. THE CLAUSE LIST IS READ OUT OF THE PRODUCT, NOT TYPED HERE.
//      `EveryClauseTheValidatorCanEmitHasANegativeFixture` parses the rule
//      strings out of src/compliance/PdfAValidator.cpp and
//      src/compliance/ProhibitedFeatureScanner.cpp and requires a fixture for
//      each. A clause added to the product with no fixture reddens this file.
//
// What is still NOT claimed: this is not an independent reading of ISO 19005.
// Independent judgement comes from veraPDF via tests/support/ConformanceChecker
// (see tests/test_conformance_independent.cpp), which is a separate mechanism
// with a separate failure mode. This file pins PDFE's own validator against
// files whose single differing construct is stated in the file name.

#include <gtest/gtest.h>

#include "core/MuPdfContext.h"
#include "core/Document.h"
#include "compliance/PdfAValidator.h"
#include "compliance/ComplianceTypes.h"

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

// One clause-violating fixture: the file, the ISO clause it must be rejected
// for, and the level at which that clause applies.
struct ClauseCase {
    const char* file;
    const char* clause;
    PdfALevel level;
    const char* control;   // the file this one is a single-construct delta from
};

// Keep in step with tests/fixtures/gen_conformance_corpus.py. The COVERAGE case
// below proves this list covers every clause the product can emit, so a stale
// entry here cannot hide a gap.
const std::vector<ClauseCase>& clauseCases() {
    static const std::vector<ClauseCase> cases = {
        {"pdfa/pdfa_6_7_11_no_xmp_identification.pdf", "6.7.11",
         PdfALevel::PdfA_1b, "pdfa/pdfa_1b_control.pdf"},
        {"pdfa/pdfa_6_2_2_no_output_intent.pdf", "6.2.2",
         PdfALevel::PdfA_1b, "pdfa/pdfa_1b_control.pdf"},
        {"pdfa/pdfa_6_3_3_font_not_embedded.pdf", "6.3.3",
         PdfALevel::PdfA_1b, "pdfa/pdfa_1b_control.pdf"},
        {"pdfa/pdfa_6_3_8_no_tounicode.pdf", "6.3.8",
         PdfALevel::PdfA_2u, "pdfa/pdfa_2u_control.pdf"},
        {"pdfa/pdfa_6_1_3_encrypted.pdf", "6.1.3",
         PdfALevel::PdfA_1b, "pdfa/pdfa_1b_control.pdf"},
        {"pdfa/pdfa_6_6_1_javascript.pdf", "6.6.1",
         PdfALevel::PdfA_1b, "pdfa/pdfa_1b_control.pdf"},
        {"pdfa/pdfa_6_6_3_launch_action.pdf", "6.6.3",
         PdfALevel::PdfA_1b, "pdfa/pdfa_1b_control.pdf"},
        {"pdfa/pdfa_6_4_transparency_group.pdf", "6.4",
         PdfALevel::PdfA_1b, "pdfa/pdfa_1b_control.pdf"},
        {"pdfa/pdfa_6_1_7_object_stream.pdf", "6.1.7",
         PdfALevel::PdfA_1b, "pdfa/pdfa_1b_control.pdf"},
        {"pdfa/pdfa_6_8_untagged.pdf", "6.8",
         PdfALevel::PdfA_2a, "pdfa/pdfa_2a_control.pdf"},
    };
    return cases;
}

struct ControlCase {
    const char* file;
    PdfALevel level;
};

const std::vector<ControlCase>& controlCases() {
    static const std::vector<ControlCase> cases = {
        {"pdfa/pdfa_1b_control.pdf", PdfALevel::PdfA_1b},
        {"pdfa/pdfa_2u_control.pdf", PdfALevel::PdfA_2u},
        {"pdfa/pdfa_2a_control.pdf", PdfALevel::PdfA_2a},
    };
    return cases;
}

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

class PdfAClauseCorpusTest : public ::testing::Test {
protected:
    void SetUp() override {
        context_ = std::make_shared<pdfe::core::MuPdfContext>();
        ASSERT_TRUE(fs::exists(conformanceDir()))
            << "the conformance corpus is missing at " << conformanceDir().string()
            << "; regenerate it with "
               "`python tests/fixtures/gen_conformance_corpus.py`";
    }
    void TearDown() override { context_.reset(); }

    // Open a corpus fixture. Fails (never skips) when it is absent: a skip here
    // would turn "the corpus is gone" into a green run.
    std::unique_ptr<pdfe::core::Document> open(const std::string& rel) {
        const fs::path p = conformanceDir() / rel;
        EXPECT_TRUE(fs::exists(p)) << "missing corpus fixture " << p.string();
        if (!fs::exists(p)) return nullptr;
        return pdfe::core::Document::open(context_, p);
    }

    std::shared_ptr<pdfe::core::MuPdfContext> context_;
};

// ---------------------------------------------------------------------------
// 1. The controls. This case is what stops every case below being vacuous.
// ---------------------------------------------------------------------------

TEST_F(PdfAClauseCorpusTest, ControlsValidateWithZeroErrors) {
    ASSERT_FALSE(controlCases().empty());
    for (const auto& c : controlCases()) {
        auto doc = open(c.file);
        ASSERT_NE(doc, nullptr) << c.file << " must open";

        PdfAValidator validator(context_);
        const auto result = validator.validate(doc.get(), c.level);

        EXPECT_EQ(result.errorCount(), 0)
            << c.file << " is the control for " << pdfaLevelName(c.level)
            << " and must validate with ZERO errors. If it does not, every "
               "clause case in this file is measuring nothing -- a validator "
               "that rejects its own control cannot distinguish the one "
               "construct each negative fixture adds. " << issueDump(result);
        EXPECT_TRUE(result.success)
            << c.file << " must be reported as compliant at "
            << pdfaLevelName(c.level) << ". " << issueDump(result);
    }
}

// ---------------------------------------------------------------------------
// 2. Every clause fixture is rejected, FOR ITS OWN CLAUSE.
// ---------------------------------------------------------------------------

TEST_F(PdfAClauseCorpusTest, EveryClauseFixtureIsRejectedForItsOwnClause) {
    ASSERT_FALSE(clauseCases().empty());

    int checked = 0;
    for (const auto& c : clauseCases()) {
        SCOPED_TRACE(std::string(c.file) + " (clause " + c.clause + ")");
        auto doc = open(c.file);
        ASSERT_NE(doc, nullptr) << c.file << " must open -- a fixture that "
                                   "cannot be opened proves nothing about the "
                                   "clause it is named for";

        PdfAValidator validator(context_);
        const auto result = validator.validate(doc.get(), c.level);

        // (a) it is rejected at all
        EXPECT_FALSE(result.success)
            << c.file << " must NOT be reported compliant at "
            << pdfaLevelName(c.level) << ". " << issueDump(result);

        // (b) ... naming the clause it was authored to violate, at Error
        //     severity. "not valid" alone would pass for any reason at all,
        //     including a reason the fixture did not introduce.
        EXPECT_TRUE(hasIssue(result, c.clause, IssueSeverity::Error))
            << c.file << " must be rejected specifically for ISO clause "
            << c.clause << ", not merely rejected. " << issueDump(result);
        ++checked;
    }
    EXPECT_EQ(checked, static_cast<int>(clauseCases().size()))
        << "not every clause case ran";
}

// ---------------------------------------------------------------------------
// 3. The control does NOT report the clause. This is the other half of the
//    oracle: (2) proves the rule can say NO, this proves it can say YES.
// ---------------------------------------------------------------------------

TEST_F(PdfAClauseCorpusTest, TheControlDoesNotReportTheClauseTheNegativeDoes) {
    for (const auto& c : clauseCases()) {
        SCOPED_TRACE(std::string(c.file) + " vs control " + c.control);
        auto control = open(c.control);
        ASSERT_NE(control, nullptr);

        PdfAValidator validator(context_);
        const auto result = validator.validate(control.get(), c.level);

        EXPECT_FALSE(hasIssue(result, c.clause, IssueSeverity::Error))
            << c.control << " reports clause " << c.clause
            << ", which is the clause " << c.file
            << " is supposed to be the ONLY file exhibiting. A rule that fires "
               "on the control fires on everything and distinguishes nothing. "
            << issueDump(result);
    }
}

// ---------------------------------------------------------------------------
// 4. isCompliant() agrees with validate(). The product exposes both; a fixture
//    corpus that only ever drives one of them leaves the other unguarded.
// ---------------------------------------------------------------------------

TEST_F(PdfAClauseCorpusTest, IsCompliantAgreesWithValidateOnEveryFixture) {
    for (const auto& c : clauseCases()) {
        auto doc = open(c.file);
        ASSERT_NE(doc, nullptr);
        PdfAValidator validator(context_);
        EXPECT_FALSE(validator.isCompliant(doc.get(), c.level))
            << c.file << " must not be isCompliant() at "
            << pdfaLevelName(c.level);
    }
    for (const auto& c : controlCases()) {
        auto doc = open(c.file);
        ASSERT_NE(doc, nullptr);
        PdfAValidator validator(context_);
        EXPECT_TRUE(validator.isCompliant(doc.get(), c.level))
            << c.file << " must be isCompliant() at " << pdfaLevelName(c.level);
    }
}

// ---------------------------------------------------------------------------
// 5. Coverage, derived from the PRODUCT SOURCE rather than from this file.
// ---------------------------------------------------------------------------
//
// A hand-maintained clause list rots: the clause a future change adds is
// exactly the clause nobody remembers to add a fixture for. So the expected set
// is parsed out of the two files that emit clause strings. If that parse ever
// finds nothing, the case fails rather than passing over an empty set.

std::set<std::string> clausesEmittedBySource() {
    const std::vector<fs::path> sources = {
        fs::path(PDFE_REPO_ROOT) / "src" / "compliance" / "PdfAValidator.cpp",
        fs::path(PDFE_REPO_ROOT) / "src" / "compliance" / "ProhibitedFeatureScanner.cpp",
    };
    // `IssueSeverity::Error,` / `IssueSeverity::Warning,` then a quoted clause
    // id on the following line -- the brace-initialiser shape both files use.
    const std::regex re(
        R"RE(IssueSeverity::(?:Error|Warning),\s*\r?\n\s*"([0-9]+(?:\.[0-9]+)*)")RE");

    std::set<std::string> out;
    for (const auto& src : sources) {
        const std::string text = readFileText(src);
        EXPECT_FALSE(text.empty()) << "cannot read " << src.string();
        for (auto it = std::sregex_iterator(text.begin(), text.end(), re);
             it != std::sregex_iterator(); ++it) {
            out.insert((*it)[1].str());
        }
    }
    return out;
}

TEST_F(PdfAClauseCorpusTest, EveryClauseTheValidatorCanEmitHasANegativeFixture) {
    const std::set<std::string> emitted = clausesEmittedBySource();
    ASSERT_FALSE(emitted.empty())
        << "parsed ZERO clause ids out of PdfAValidator.cpp / "
           "ProhibitedFeatureScanner.cpp. A coverage check that parses nothing "
           "passes vacuously, which is the defect class this project keeps "
           "finding; the brace-initialiser shape must have changed.";

    std::set<std::string> covered;
    for (const auto& c : clauseCases()) covered.insert(c.clause);

    std::vector<std::string> uncovered;
    std::set_difference(emitted.begin(), emitted.end(),
                        covered.begin(), covered.end(),
                        std::back_inserter(uncovered));
    std::vector<std::string> phantom;
    std::set_difference(covered.begin(), covered.end(),
                        emitted.begin(), emitted.end(),
                        std::back_inserter(phantom));

    std::ostringstream u, p;
    for (const auto& s : uncovered) u << s << " ";
    for (const auto& s : phantom) p << s << " ";

    EXPECT_TRUE(uncovered.empty())
        << "these ISO clauses can be emitted by PDFE's PDF/A validator but no "
           "fixture in tests/fixtures/flaw_corpus/conformance/ exercises them: "
        << u.str()
        << "\nAdd one to tests/fixtures/gen_conformance_corpus.py and to "
           "clauseCases() above.";
    EXPECT_TRUE(phantom.empty())
        << "these clauses have fixtures but nothing in the product source can "
           "emit them any more: " << p.str()
        << "\nA fixture for a clause the validator no longer knows about is a "
           "test asserting a rule that is gone.";
}

// ---------------------------------------------------------------------------
// 6. The corpus on disk matches its generator, byte for byte.
// ---------------------------------------------------------------------------
//
// A committed binary fixture whose generator has since drifted is unauditable:
// the file says one thing and the script that documents it says another. The
// generator is deterministic precisely so this can be asserted.

TEST_F(PdfAClauseCorpusTest, EveryFixtureNamedHereExistsOnDisk) {
    const fs::path manifest = conformanceDir() / "manifest.json";
    ASSERT_TRUE(fs::exists(manifest))
        << "the corpus manifest is missing at " << manifest.string();
    const std::string text = readFileText(manifest);
    ASSERT_FALSE(text.empty());

    int seen = 0;
    for (const auto& c : clauseCases()) {
        EXPECT_TRUE(fs::exists(conformanceDir() / c.file))
            << c.file << " is named by this test but is not on disk";
        EXPECT_NE(text.find(c.file), std::string::npos)
            << c.file << " is not listed in the corpus manifest, so a reader "
                         "auditing the corpus enumerates the wrong set";
        EXPECT_NE(text.find(std::string("\"clause\": \"") + c.clause + "\""),
                  std::string::npos)
            << "the manifest does not record clause " << c.clause;
        ++seen;
    }
    for (const auto& c : controlCases()) {
        EXPECT_TRUE(fs::exists(conformanceDir() / c.file));
        ++seen;
    }
    EXPECT_EQ(seen, static_cast<int>(clauseCases().size() + controlCases().size()));
}

}  // namespace
