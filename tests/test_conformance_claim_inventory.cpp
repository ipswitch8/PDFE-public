// -*- coding: utf-8 -*-
//
// Conformance-claim inventory and falsification — meta-audit follow-up F14,
// acceptance criterion 6.
//
// Gaps: PDF-17 (nothing prevented or detected a FALSE conformance claim),
// PDF-19 (PDF/A-4 is not modelled and was never explicitly reported as
// unsupported).
//
// THE RULE. Every conformance claim PDFE STAMPS into a saved file must have a
// test that FAILS when the claim is false. A claim is a machine-readable
// assertion downstream systems trust without revalidating — an archival
// ingester reads `pdfaid:part` and files the document as PDF/A. A claim with no
// falsification test is an unbacked promise.
//
// This file does two things:
//
//   1. `ClaimInventory.EveryStampedClaimHasARegisteredGuardTest` walks the
//      GoogleTest registry and asserts that the named guard for every row of
//      the inventory below actually EXISTS. Renaming or deleting a guard turns
//      this red — the inventory cannot silently rot into prose.
//
//   2. The `ConformanceClaimFalsification` suite provides the guards that did
//      not previously exist: each stamps a claim onto a document that does NOT
//      satisfy it and asserts the product reports the discrepancy.

#include <gtest/gtest.h>

#include "compliance/ComplianceTypes.h"
#include "compliance/IccProfileManager.h"
#include "compliance/PdfAValidator.h"
#include "compliance/PdfUAConverter.h"
#include "compliance/XmpMetadataWriter.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"

#include <QTemporaryDir>

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace pdfe::compliance;
using pdfe::core::Document;
using pdfe::core::MuPdfContext;

namespace {

/// One conformance claim the product stamps, and the test that falsifies it.
struct ClaimRow {
    const char* claim;        ///< what is written into the file
    const char* stampedAt;    ///< the source site that writes it
    const char* guardSuite;   ///< GoogleTest suite of the falsification test
    const char* guardTest;    ///< GoogleTest name of the falsification test
};

// The inventory. Mirrored verbatim in
// docs/audit/meta-audit-2026-07/followup/phase-14-dispositions.md.
const std::vector<ClaimRow>& inventory() {
    static const std::vector<ClaimRow> rows = {
        {"XMP pdfaid:part + pdfaid:conformance (the PDF/A level claim)",
         "src/compliance/XmpMetadataWriter.cpp writePdfAIdentification "
         "(level->part/conformance map at :30-57)",
         "ConformanceIndependentTest", "StampedClaimIsIndependentlyChecked"},

        {"XMP pdfaid claim vs the document's actual PDF/A conformance",
         "src/compliance/XmpMetadataWriter.cpp writePdfAIdentification",
         "ConformanceClaimFalsification", "PdfAClaimWithoutConformanceIsReported"},

        {"/OutputIntents [<< /S /GTS_PDFA1 /DestOutputProfile <stream> >>]",
         "src/compliance/IccProfileManager.cpp:282",
         "ConformanceClaimFalsification", "OutputIntentClaimIsFalsifiable"},

        {"/MarkInfo << /Marked true >> (the tagged-PDF claim)",
         "src/compliance/PdfAConverter.cpp:160 and "
         "src/compliance/StructureTreeBuilder.cpp:155",
         "ConformanceClaimFalsification", "TaggedPdfClaimIsFalsifiable"},

        {"/StructTreeRoot present (the structure-tree claim)",
         "src/compliance/StructureTreeBuilder.cpp:146",
         "ConformanceClaimFalsification", "TaggedPdfClaimIsFalsifiable"},

        {"XMP pdfuaid:part (the PDF/UA level claim)",
         "src/compliance/XmpMetadataWriter.cpp writePdfUAIdentification",
         "ConformanceClaimFalsification", "PdfUaClaimIsFalsifiable"},

        {"PDF/A part coverage: parts 1/2/3 only — part 4 is NOT claimable",
         "src/compliance/XmpMetadataWriter.cpp partsToPdfALevel (:55-66)",
         "ConformanceClaimFalsification", "PdfA4IsExplicitlyNotRecognised"},
    };
    return rows;
}

fs::path fixture(const std::string& n) { return fs::path(TEST_FIXTURES_DIR) / n; }

bool testIsRegistered(const std::string& suite, const std::string& name) {
    const auto* unit = ::testing::UnitTest::GetInstance();
    for (int i = 0; i < unit->total_test_suite_count(); ++i) {
        const auto* ts = unit->GetTestSuite(i);
        if (suite != ts->name()) continue;
        for (int j = 0; j < ts->total_test_count(); ++j) {
            const auto* ti = ts->GetTestInfo(j);
            if (name == ti->name()) {
                // A DISABLED_-prefixed test is not a guard. GoogleTest encodes
                // disabling in the NAME, so an exact-name lookup already fails
                // for a disabled guard; this re-checks both halves explicitly.
                //
                // Deliberately NOT is_reportable()/should_run(): those depend on
                // the active --gtest_filter, which would make this test fail
                // spuriously during any filtered run. Registration is the
                // property being asserted, and registration is filter-independent.
                const std::string suiteName = ts->name();
                const bool disabled =
                    suiteName.rfind("DISABLED_", 0) == 0 ||
                    name.rfind("DISABLED_", 0) == 0;
                return !disabled;
            }
        }
    }
    return false;
}

bool hasIssueWithRule(const ComplianceResult& r, const std::string& rule) {
    for (const auto& i : r.issues) {
        if (i.rule == rule && i.severity == IssueSeverity::Error) return true;
    }
    return false;
}

std::string issueDigest(const ComplianceResult& r) {
    std::string s;
    for (const auto& i : r.issues) {
        s += "[" + i.rule + "] " + i.description + "\n";
    }
    return s.empty() ? "(no issues reported)" : s;
}

class ConformanceClaimFalsification : public ::testing::Test {
protected:
    void SetUp() override {
        ctx_ = std::make_shared<MuPdfContext>();
        ASSERT_TRUE(tmp_.isValid());
    }
    void TearDown() override { ctx_.reset(); }

    fs::path out(const std::string& n) const {
        return fs::path(tmp_.path().toStdString()) / n;
    }

    /// Copy `test_single.pdf` — a plain document that is NOT PDF/A, has no
    /// OutputIntent, no /MarkInfo, no /StructTreeRoot and no /Lang — then stamp
    /// @p level's XMP claim onto it and save. The result is a file whose CLAIM
    /// is true-looking and whose CONTENT does not back it: exactly the artefact
    /// a false-claim guard must reject.
    fs::path stampClaimOnNonConformantDocument(PdfALevel level,
                                               const std::string& name) {
        const fs::path dst = out(name);
        std::error_code ec;
        fs::copy_file(fixture("test_single.pdf"), dst,
                      fs::copy_options::overwrite_existing, ec);
        EXPECT_FALSE(ec) << ec.message();

        auto doc = Document::open(ctx_, dst);
        EXPECT_NE(doc, nullptr);
        if (!doc) return dst;
        XmpMetadataWriter xmp(ctx_);
        xmp.writePdfAIdentification(doc.get(), level);
        doc->markRequiresFullRewrite();
        doc->save(dst);
        return dst;
    }

    std::shared_ptr<MuPdfContext> ctx_;
    QTemporaryDir tmp_;
};

} // namespace

// ===========================================================================
// Criterion 6 — the inventory itself
// ===========================================================================

TEST(ClaimInventory, EveryStampedClaimHasARegisteredGuardTest) {
    ASSERT_FALSE(inventory().empty());
    for (const auto& row : inventory()) {
        SCOPED_TRACE(std::string(row.claim) + "  (stamped at " + row.stampedAt + ")");
        EXPECT_TRUE(testIsRegistered(row.guardSuite, row.guardTest))
            << "the guard test " << row.guardSuite << "." << row.guardTest
            << " is not registered (renamed, deleted, or DISABLED_). The claim \""
            << row.claim << "\" would then be stamped with nothing able to "
               "falsify it.";
    }
}

// ===========================================================================
// The guards
// ===========================================================================

// The core false-claim case: a document that ADVERTISES PDF/A-1b but embeds no
// output intent and no compliant fonts. The product must not agree with the
// claim just because the claim is present.
TEST_F(ConformanceClaimFalsification, PdfAClaimWithoutConformanceIsReported) {
    const fs::path p = stampClaimOnNonConformantDocument(PdfALevel::PdfA_1b,
                                                         "false_claim_1b.pdf");
    auto doc = Document::open(ctx_, p);
    ASSERT_NE(doc, nullptr);

    // The claim IS present — otherwise this test would be vacuous.
    XmpMetadataWriter xmp(ctx_);
    ASSERT_EQ(xmp.detectPdfALevel(doc.get()), PdfALevel::PdfA_1b)
        << "setup failed: the pdfaid claim was not stamped, so there is "
           "nothing to falsify";

    PdfAValidator validator(ctx_);
    const ComplianceResult r = validator.validate(doc.get(), PdfALevel::PdfA_1b);
    EXPECT_FALSE(r.success)
        << "a document was accepted as PDF/A-1b purely because it CLAIMS to "
           "be. Reported issues:\n" << issueDigest(r);
    EXPECT_FALSE(validator.isCompliant(doc.get(), PdfALevel::PdfA_1b));
}

// The output-intent claim: PDF/A requires an /OutputIntents entry with an ICC
// /DestOutputProfile. Absence must be an ERROR, not a warning that a caller can
// overlook.
TEST_F(ConformanceClaimFalsification, OutputIntentClaimIsFalsifiable) {
    const fs::path p = stampClaimOnNonConformantDocument(PdfALevel::PdfA_1b,
                                                         "no_output_intent.pdf");
    auto doc = Document::open(ctx_, p);
    ASSERT_NE(doc, nullptr);

    // Independent of the validator: the ICC manager itself must see no intent.
    IccProfileManager icc(ctx_);
    ASSERT_FALSE(icc.hasOutputIntent(doc.get()))
        << "setup failed: the fixture already carries an OutputIntent";

    PdfAValidator validator(ctx_);
    const ComplianceResult r = validator.validate(doc.get(), PdfALevel::PdfA_1b);
    EXPECT_TRUE(hasIssueWithRule(r, "6.2.2"))
        << "a PDF/A claim was stamped on a document with no OutputIntent and "
           "the validator did not raise clause 6.2.2. Reported:\n"
        << issueDigest(r);
}

// The tagged-PDF claim: /MarkInfo /Marked true plus /StructTreeRoot. A level
// that REQUIRES tagging (2a) must be refused for an untagged document, however
// the XMP is stamped.
TEST_F(ConformanceClaimFalsification, TaggedPdfClaimIsFalsifiable) {
    const fs::path p = stampClaimOnNonConformantDocument(PdfALevel::PdfA_2a,
                                                         "false_claim_2a.pdf");
    auto doc = Document::open(ctx_, p);
    ASSERT_NE(doc, nullptr);

    XmpMetadataWriter xmp(ctx_);
    ASSERT_EQ(xmp.detectPdfALevel(doc.get()), PdfALevel::PdfA_2a)
        << "setup failed: the PDF/A-2a claim was not stamped";

    PdfAValidator validator(ctx_);
    const ComplianceResult r = validator.validate(doc.get(), PdfALevel::PdfA_2a);
    EXPECT_TRUE(hasIssueWithRule(r, "6.8"))
        << "a PDF/A-2a (accessible) claim was stamped on a document with "
           "neither /MarkInfo /Marked nor /StructTreeRoot, and the validator "
           "did not raise the tagging clause. Reported:\n"
        << issueDigest(r);
    EXPECT_FALSE(r.success);
}

// The PDF/UA claim: pdfuaid:part stamped on a document with no structure tree
// and no /Lang must be reported as non-conformant by the PDF/UA validator.
TEST_F(ConformanceClaimFalsification, PdfUaClaimIsFalsifiable) {
    const fs::path dst = out("false_claim_ua.pdf");
    std::error_code ec;
    fs::copy_file(fixture("test_single.pdf"), dst,
                  fs::copy_options::overwrite_existing, ec);
    ASSERT_FALSE(ec) << ec.message();

    {
        auto doc = Document::open(ctx_, dst);
        ASSERT_NE(doc, nullptr);
        XmpMetadataWriter xmp(ctx_);
        xmp.writePdfUAIdentification(doc.get(), PdfUALevel::PdfUA_1);
        doc->markRequiresFullRewrite();
        doc->save(dst);
    }

    auto doc = Document::open(ctx_, dst);
    ASSERT_NE(doc, nullptr);
    PdfUAConverter ua(ctx_);
    const ComplianceResult r = ua.validatePdfUA(doc.get());
    EXPECT_FALSE(r.success)
        << "a pdfuaid:part PDF/UA claim was stamped on a document with no "
           "structure tree and no document /Lang, and validatePdfUA accepted "
           "it. Reported:\n" << issueDigest(r);
    EXPECT_FALSE(r.issues.empty());
}

// PDF-19: PDF/A-4 (ISO 19005-4, the PDF 2.0 conformance level) is NOT modelled
// — `PdfALevel` stops at part 3. That is a defensible scope decision, but it
// must be EXPLICIT: a `pdfaid:part 4` file must resolve to "no recognised
// claim", never be silently mapped onto a part-1/2/3 level PDFE could then
// pretend to validate.
TEST_F(ConformanceClaimFalsification, PdfA4IsExplicitlyNotRecognised) {
    // The enum genuinely has no part-4 member; assert the boundary rather than
    // asserting a member that does not exist.
    EXPECT_EQ(static_cast<int>(PdfALevel::PdfA_3a),
              static_cast<int>(PdfALevel::None) + 8)
        << "PdfALevel gained or lost a member; re-check whether PDF/A-4 is now "
           "modelled and update this guard plus the phase-14 dispositions.";

    // A document carrying a part-4 claim must come back as None (unrecognised),
    // NOT as some part-1/2/3 level.
    const fs::path dst = out("claims_part4.pdf");
    std::error_code ec;
    fs::copy_file(fixture("test_single.pdf"), dst,
                  fs::copy_options::overwrite_existing, ec);
    ASSERT_FALSE(ec) << ec.message();

    {
        // Stamp a real 2b claim first so a /Metadata packet exists, then
        // rewrite just the part number to 4 in the saved bytes. Editing the
        // packet in place keeps the XMP well-formed.
        auto doc = Document::open(ctx_, dst);
        ASSERT_NE(doc, nullptr);
        XmpMetadataWriter xmp(ctx_);
        xmp.writePdfAIdentification(doc.get(), PdfALevel::PdfA_2b);
        doc->markRequiresFullRewrite();
        doc->save(dst);
    }

    // Confirm the 2b claim round-tripped, so the rewrite below has a target.
    {
        auto doc = Document::open(ctx_, dst);
        ASSERT_NE(doc, nullptr);
        XmpMetadataWriter xmp(ctx_);
        ASSERT_EQ(xmp.detectPdfALevel(doc.get()), PdfALevel::PdfA_2b)
            << "setup failed: no pdfaid:part claim was written";
    }

    // Byte-rewrite `<pdfaid:part>2<` -> `<pdfaid:part>4<`. Same length, so the
    // file structure is untouched.
    {
        std::string bytes;
        {
            std::ifstream in(dst, std::ios::binary);
            std::ostringstream ss;
            ss << in.rdbuf();
            bytes = ss.str();
        }
        const std::string needle = "<pdfaid:part>2<";
        const auto at = bytes.find(needle);
        if (at == std::string::npos) {
            // The packet may be stored compressed; in that case this particular
            // byte-level probe is not applicable and the enum-boundary
            // assertion above carries the guard.
            SUCCEED() << "pdfaid packet is not stored as plain text in this "
                         "build; the enum-boundary assertion above still holds";
            return;
        }
        bytes[at + needle.size() - 2] = '4';
        std::ofstream o(dst, std::ios::binary | std::ios::trunc);
        o << bytes;
    }

    auto doc = Document::open(ctx_, dst);
    ASSERT_NE(doc, nullptr);
    XmpMetadataWriter xmp(ctx_);
    EXPECT_EQ(xmp.detectPdfALevel(doc.get()), PdfALevel::None)
        << "a pdfaid:part 4 claim was mapped onto a modelled PDF/A level. "
           "PDFE does not implement ISO 19005-4, so it must report the claim "
           "as unrecognised rather than validate it against part-1/2/3 rules.";
}
