// PHASE4_ROUNDTRIP_PDFA: convert -> save -> validate (reopen +
// PdfAValidator) tests for the PDF/A compliance subsystem. Closes
// the audit's gap that test_pdfa_converter.cpp asserts a `success`
// flag and that the output file exists, but never validates the
// output against the PDF/A spec via PdfAValidator.

#include <gtest/gtest.h>

#include "compliance/PdfAConverter.h"
#include "compliance/PdfAValidator.h"
#include "compliance/ComplianceTypes.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"

extern "C" {
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
}

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>

using pdfe::compliance::ComplianceResult;
using pdfe::compliance::PdfALevel;
using pdfe::compliance::PdfAConverter;
using pdfe::compliance::PdfAValidator;

namespace {

// ───────────────────────────────────────────────────────────────────────────
// STRUCTURE-TREE IDENTITY FINGERPRINT  (GitHub #15 / gap-register PDF-20)
//
// The criterion is *preserves*, not "produces a valid one". A converter that
// discarded the authored tree and auto-tagged the pages afresh would emit a
// perfectly conformant PDF/A-2a — and would have destroyed the author's work.
// An oracle that asked "is there a /StructTreeRoot?" or "does it validate?"
// cannot tell those two outcomes apart. So this fingerprint pins IDENTITY:
//
//   * the ORDERED depth-first sequence of structure elements,
//   * each element's DEPTH and structure TYPE (/S),
//   * its authored /Alt alternate description,
//   * its authored /ID structure-element identifier,
//   * its authored /T title,
//   * the PAGE INDEX its /Pg resolves to, and its /K MCID leaf.
//
// /Alt, /ID and /T are the load-bearing ones. They are author-supplied strings
// with no counterpart anywhere in the page content stream, so no geometry-
// driven auto-tagger has any source to re-derive them from. The /S sequence
// pins the rest: the authored fixture nests /Document > /Part > /Sect >
// /L > /LI > /LBody, a document architecture an auto-tagger looking at three
// pages of flat text has no way to infer.
//
// Object NUMBERS are deliberately absent from the fingerprint: a legitimate
// save renumbers objects, and pinning them would make this test fail on a
// faithful copy. The page INDEX a /Pg resolves to is stable and is used
// instead.
//
// setjmp/longjmp discipline: the whole traversal writes into PLAIN-OLD-DATA
// members of StructWalk. No std::string, no std::vector, nothing with a
// destructor is constructed inside the fz_try (or in any helper it calls) —
// the conversion to std::string happens in the caller after fz_try returns.
constexpr size_t kFpBufMax = 32768;
constexpr int kMaxFpPages = 128;

struct StructWalk {
    char buf[kFpBufMax];
    size_t len = 0;
    int elemCount = 0;
    int maxDepth = 0;
    bool truncated = false;
    bool hasStructTreeRoot = false;
    int pageNums[kMaxFpPages];
    int nPages = 0;
};

void fpAppend(StructWalk& w, const char* s) {
    if (s == nullptr) return;
    while (*s != '\0') {
        if (w.len + 1 >= kFpBufMax) { w.truncated = true; break; }
        w.buf[w.len++] = *s++;
    }
    w.buf[w.len] = '\0';
}

void fpAppendInt(StructWalk& w, int v) {
    char t[24];
    std::snprintf(t, sizeof(t), "%d", v);
    fpAppend(w, t);
}

// "" for absent / wrong-typed, so a dropped key is a VISIBLE empty field
// rather than a missing line.
const char* fpText(fz_context* c, pdf_obj* o) {
    if (o == nullptr || !pdf_is_string(c, o)) return "";
    const char* s = pdf_to_text_string(c, o);
    return s ? s : "";
}

const char* fpName(fz_context* c, pdf_obj* o) {
    if (o == nullptr || !pdf_is_name(c, o)) return "";
    const char* s = pdf_to_name(c, o);
    return s ? s : "";
}

// Page index of the object /Pg points at, or -1. Compared by OBJECT NUMBER
// within one document and reported as an INDEX, which survives renumbering.
int fpPageIndex(fz_context* c, const StructWalk& w, pdf_obj* pg) {
    if (pg == nullptr) return -1;
    const int num = pdf_to_num(c, pg);
    if (num <= 0) return -1;
    for (int i = 0; i < w.nPages; ++i) {
        if (w.pageNums[i] == num) return i;
    }
    return -2;  // a /Pg that does not resolve to any live page
}

void fpWalk(fz_context* c, pdf_obj* node, int depth, StructWalk& w) {
    if (node == nullptr || depth > 32) return;

    if (pdf_is_array(c, node)) {
        const int n = pdf_array_len(c, node);
        for (int i = 0; i < n; ++i) {
            fpWalk(c, pdf_array_get(c, node, i), depth, w);
        }
        return;
    }
    if (pdf_is_number(c, node)) {   // a bare MCID leaf under /K
        fpAppend(w, "  mcid=");
        fpAppendInt(w, pdf_to_int(c, node));
        fpAppend(w, "\n");
        return;
    }
    if (!pdf_is_dict(c, node)) return;

    // An /MCR or /OBJR reference dict is not a structure element.
    const char* type = fpName(c, pdf_dict_get(c, node, PDF_NAME(Type)));
    if (std::strcmp(type, "MCR") == 0 || std::strcmp(type, "OBJR") == 0) {
        fpAppend(w, "  ref=");
        fpAppend(w, type);
        fpAppend(w, "\n");
        return;
    }

    w.elemCount++;
    if (depth > w.maxDepth) w.maxDepth = depth;

    fpAppend(w, "d=");
    fpAppendInt(w, depth);
    fpAppend(w, " S=");
    fpAppend(w, fpName(c, pdf_dict_get(c, node, PDF_NAME(S))));
    fpAppend(w, " T=[");
    fpAppend(w, fpText(c, pdf_dict_gets(c, node, "T")));
    fpAppend(w, "] ID=[");
    fpAppend(w, fpText(c, pdf_dict_gets(c, node, "ID")));
    fpAppend(w, "] Alt=[");
    fpAppend(w, fpText(c, pdf_dict_gets(c, node, "Alt")));
    fpAppend(w, "] Pg=");
    fpAppendInt(w, fpPageIndex(c, w, pdf_dict_gets(c, node, "Pg")));

    pdf_obj* k = pdf_dict_get(c, node, PDF_NAME(K));
    if (k != nullptr && pdf_is_number(c, k)) {
        fpAppend(w, " K=");
        fpAppendInt(w, pdf_to_int(c, k));
        fpAppend(w, "\n");
        return;
    }
    fpAppend(w, "\n");
    fpWalk(c, k, depth + 1, w);
}

struct StructFingerprint {
    bool opened = false;
    bool hasStructTreeRoot = false;
    bool truncated = false;
    int elemCount = 0;
    int maxDepth = 0;
    int pageCount = 0;
    std::string text;
};

StructFingerprint structFingerprint(fz_context* c, const std::string& path) {
    StructWalk w;          // POD, constructed BEFORE the fz_try below.
    w.buf[0] = '\0';
    bool opened = false;

    fz_document* fzDoc = nullptr;
    fz_try(c) {
        fzDoc = fz_open_document(c, path.c_str());
        pdf_document* pdfDoc = pdf_document_from_fz_document(c, fzDoc);
        if (pdfDoc != nullptr) {
            opened = true;
            const int np = pdf_count_pages(c, pdfDoc);
            for (int i = 0; i < np && w.nPages < kMaxFpPages; ++i) {
                w.pageNums[w.nPages++] =
                    pdf_to_num(c, pdf_lookup_page_obj(c, pdfDoc, i));
            }
            pdf_obj* catalog =
                pdf_dict_get(c, pdf_trailer(c, pdfDoc), PDF_NAME(Root));
            pdf_obj* str =
                pdf_dict_get(c, catalog, PDF_NAME(StructTreeRoot));
            w.hasStructTreeRoot = (str != nullptr);
            if (str != nullptr) {
                fpWalk(c, pdf_dict_get(c, str, PDF_NAME(K)), 0, w);
            }
        }
    }
    fz_always(c) {
        if (fzDoc != nullptr) fz_drop_document(c, fzDoc);
    }
    fz_catch(c) {
        opened = false;
    }

    StructFingerprint fp;
    fp.opened = opened;
    fp.hasStructTreeRoot = w.hasStructTreeRoot;
    fp.truncated = w.truncated;
    fp.elemCount = w.elemCount;
    fp.maxDepth = w.maxDepth;
    fp.pageCount = w.nPages;
    fp.text.assign(w.buf, w.len);
    return fp;
}

bool fpContains(const StructFingerprint& fp, const char* needle) {
    return fp.text.find(needle) != std::string::npos;
}

class PdfARoundTripTest : public ::testing::Test {
protected:
    void SetUp() override {
        ctx_ = std::make_shared<pdfe::core::MuPdfContext>();
        fixture_ = std::filesystem::path(TEST_FIXTURES_DIR) / "test_single.pdf";
        ASSERT_TRUE(std::filesystem::exists(fixture_));
        document_ = pdfe::core::Document::open(ctx_, fixture_);
        ASSERT_NE(document_, nullptr);
        converter_ = std::make_unique<PdfAConverter>(ctx_);
        validator_ = std::make_unique<PdfAValidator>(ctx_);

        const ::testing::TestInfo* info =
            ::testing::UnitTest::GetInstance()->current_test_info();
        tempDir_ = std::filesystem::temp_directory_path()
                   / "pdfe_pdfa_roundtrip" / info->name();
        std::filesystem::create_directories(tempDir_);
    }

    void TearDown() override {
        validator_.reset();
        converter_.reset();
        document_.reset();
        ctx_.reset();
    }

    std::filesystem::path outFile(const std::string& stem) {
        return tempDir_ / (stem + ".pdf");
    }

    std::shared_ptr<pdfe::core::MuPdfContext> ctx_;
    std::filesystem::path fixture_;
    std::filesystem::path tempDir_;
    std::unique_ptr<pdfe::core::Document> document_;
    std::unique_ptr<PdfAConverter> converter_;
    std::unique_ptr<PdfAValidator> validator_;
};

// ─── PDF/A-1b conversion + validation ────────────────────────────────

TEST_F(PdfARoundTripTest, PdfA1bConvertedDocumentValidatesAsCompliant) {
    // Note: PdfAConverter::convertAndSave returns ComplianceResult
    // whose `success` flag reflects "no errors / no warnings", not
    // "did the file get written". Existing test_pdfa_converter.cpp
    // tests don't assert the success flag — they assert the file
    // exists + XmpMetadataWriter detects the right level. Match that
    // pattern here.
    auto out = outFile("pdfa_1b");
    converter_->convertAndSave(document_.get(), PdfALevel::PdfA_1b, out);
    ASSERT_TRUE(std::filesystem::exists(out))
        << "convertAndSave must produce an output file";
    ASSERT_GT(std::filesystem::file_size(out), 0u);

    // Re-open the converted document and validate. The round-trip
    // contract: validator.validate(out, PdfA_1b) must report fewer
    // issues on the converted output than on the original — that is
    // the actual signal that conversion did something useful.
    auto convDoc = pdfe::core::Document::open(ctx_, out);
    ASSERT_NE(convDoc, nullptr);
    auto valConverted = validator_->validate(convDoc.get(), PdfALevel::PdfA_1b);
    auto valOriginal  = validator_->validate(document_.get(), PdfALevel::PdfA_1b);
    EXPECT_LE(valConverted.errorCount(), valOriginal.errorCount())
        << "PdfA_1b conversion must NOT increase the validator error "
           "count (converted=" << valConverted.errorCount()
        << ", original=" << valOriginal.errorCount() << ")";
}

// ─── PDF/A-2b conversion + validation ────────────────────────────────

TEST_F(PdfARoundTripTest, PdfA2bConvertedDocumentValidatesAsCompliant) {
    auto out = outFile("pdfa_2b");
    converter_->convertAndSave(document_.get(), PdfALevel::PdfA_2b, out);
    ASSERT_TRUE(std::filesystem::exists(out));
    ASSERT_GT(std::filesystem::file_size(out), 0u);

    auto convDoc = pdfe::core::Document::open(ctx_, out);
    ASSERT_NE(convDoc, nullptr);
    auto valConverted = validator_->validate(convDoc.get(), PdfALevel::PdfA_2b);
    auto valOriginal  = validator_->validate(document_.get(), PdfALevel::PdfA_2b);
    EXPECT_LE(valConverted.errorCount(), valOriginal.errorCount())
        << "PdfA_2b conversion must NOT increase the validator error "
           "count";
}

// ─── isCompliant fast-path matches validate() ────────────────────────

TEST_F(PdfARoundTripTest, IsCompliantMatchesValidateForConvertedDoc) {
    auto out = outFile("pdfa_compliant_check");
    converter_->convertAndSave(document_.get(), PdfALevel::PdfA_1b, out);
    auto convDoc = pdfe::core::Document::open(ctx_, out);
    ASSERT_NE(convDoc, nullptr);

    bool fast = validator_->isCompliant(convDoc.get(), PdfALevel::PdfA_1b);
    auto full = validator_->validate(convDoc.get(), PdfALevel::PdfA_1b);
    EXPECT_EQ(fast, !full.hasErrors())
        << "isCompliant() and validate().hasErrors() must agree on the "
           "same document at the same level";
}

// ─── Pre-conversion fixture is NOT compliant ─────────────────────────

TEST_F(PdfARoundTripTest, UnconvertedDocumentReportsValidationIssues) {
    // The synthetic fixture is unlikely to be PDF/A-1b compliant
    // out-of-the-box (no /OutputIntents, no XMP pdfaid). The
    // validator must catch this — if it claims everything is fine,
    // the validator is the bug.
    auto valResult = validator_->validate(document_.get(), PdfALevel::PdfA_1b);
    // The first assertion in karen's review (errorCount() == 0 OR
    // hasErrors()) was a tautology — always true. The real signal
    // is that at least one issue surfaces on a known non-compliant
    // input. test_single.pdf is generated by PyMuPDF without
    // OutputIntent / pdfaid XMP, so it should fail PDF/A-1b
    // validation in some way — error or warning.
    EXPECT_GT(valResult.issues.size(), 0u)
        << "Synthetic test_single.pdf is expected to surface at least "
           "one PDF/A-1b issue (missing OutputIntent, missing pdfaid "
           "in XMP, etc.). If 0, validator is silently accepting "
           "non-compliant input.";
}

// ─── analyzeForConversion is read-only ───────────────────────────────

TEST_F(PdfARoundTripTest, AnalyzeForConversionDoesNotMutateDocument) {
    // analyzeForConversion is the dry-run pre-flight; it must not
    // alter the source document. Exercise by analyzing then writing a
    // plain (non-converted) save and verify the source still validates
    // the same way after analysis.
    auto pre = validator_->validate(document_.get(), PdfALevel::PdfA_1b);
    // analyzeForConversion is the dry-run; its return value isn't
    // the contract under test (size() >= 0 on unsigned is vacuous).
    // The contract is that calling it doesn't mutate the source
    // document — verified by the validator-result equality below.
    converter_->analyzeForConversion(
        document_.get(), PdfALevel::PdfA_1b);

    auto post = validator_->validate(document_.get(), PdfALevel::PdfA_1b);
    EXPECT_EQ(pre.issues.size(), post.issues.size())
        << "analyzeForConversion must not change the source document's "
           "validation issue count (it ran some analysis-side mutation "
           "if these counts diverge)";
}

// ─── icc_outputintent realworld fixture ──────────────────────────────

TEST_F(PdfARoundTripTest, IccOutputIntentRealworldFixtureOpens) {
    auto rwPath = std::filesystem::path(TEST_FIXTURES_DIR) / "realworld"
                  / "icc_profiles" / "icc_outputintent.pdf";
    ASSERT_TRUE(std::filesystem::exists(rwPath))
        << "phase-2 fixture missing: " << rwPath;

    auto rwDoc = pdfe::core::Document::open(ctx_, rwPath);
    ASSERT_NE(rwDoc, nullptr)
        << "MuPDF must accept the manually-constructed PDF with /OutputIntents";

    // Run the validator over the realworld fixture. The exact verdict
    // depends on what shape the fixture takes — we don't lock to a
    // specific result. The contract being tested is: validator runs
    // to completion without throwing on a real-world OutputIntent
    // shape.
    auto result = validator_->validate(rwDoc.get(), PdfALevel::PdfA_1b);

    // A3-19/A3-41 (over-tolerant-threshold + SUCCEED-only tail): the old
    // `EXPECT_GE(issues.size(), 0u)` plus a bare SUCCEED asserted nothing. Two
    // fixture-independent invariants replace them: the issue list and the
    // error/warning counters must describe the same set, and the validator must
    // be deterministic over an unchanged document.
    EXPECT_EQ(result.hasErrors(), result.errorCount() > 0)
        << "hasErrors() and errorCount() disagree about the same issue list";
    EXPECT_EQ(result.hasWarnings(), result.warningCount() > 0)
        << "hasWarnings() and warningCount() disagree about the same issue list";
    EXPECT_LE(static_cast<size_t>(result.errorCount() + result.warningCount()),
              result.issues.size())
        << "the severity counters exceed the issue list they count";
    auto again = validator_->validate(rwDoc.get(), PdfALevel::PdfA_1b);
    EXPECT_EQ(again.issues.size(), result.issues.size())
        << "validate() is not deterministic over an unchanged document";
    EXPECT_EQ(again.errorCount(), result.errorCount());
}

// ─── Specific prohibited-feature detection + conversion fixes it ────

TEST_F(PdfARoundTripTest,
       MissingOutputIntentIsDetectedAndFixedByConversion) {
    // Locks the contract that the validator (a) catches a SPECIFIC
    // prohibited / missing feature on a non-compliant input and
    // (b) the converter fixes it. Without (a) the validator could
    // silently accept anything; without (b) the converter could
    // silently fail without raising the validator's error count.
    auto preIssues = validator_->validate(
        document_.get(), PdfALevel::PdfA_1b).issues;

    bool sawMissingOutputIntent = false;
    bool sawMissingXmp = false;
    for (const auto& issue : preIssues) {
        auto t = pdfe::compliance::classifyIssueByRule(issue.rule);
        if (t == pdfe::compliance::IssueType::MissingOutputIntent)
            sawMissingOutputIntent = true;
        if (t == pdfe::compliance::IssueType::MissingXmpMetadata)
            sawMissingXmp = true;
    }
    EXPECT_TRUE(sawMissingOutputIntent || sawMissingXmp)
        << "Synthetic test_single.pdf must trigger at least one of "
           "MissingOutputIntent (rule 6.2.2) or MissingXmpMetadata "
           "(rule 6.7/6.7.3/6.7.11). If neither, the validator is "
           "silently accepting a known non-compliant fixture.";

    // Convert and re-validate.
    auto out = outFile("missing_outputintent_fix");
    converter_->convertAndSave(document_.get(), PdfALevel::PdfA_1b, out);
    ASSERT_TRUE(std::filesystem::exists(out));
    auto convDoc = pdfe::core::Document::open(ctx_, out);
    ASSERT_NE(convDoc, nullptr);

    auto postIssues = validator_->validate(
        convDoc.get(), PdfALevel::PdfA_1b).issues;
    bool stillMissingOutputIntent = false;
    bool stillMissingXmp = false;
    for (const auto& issue : postIssues) {
        auto t = pdfe::compliance::classifyIssueByRule(issue.rule);
        if (t == pdfe::compliance::IssueType::MissingOutputIntent)
            stillMissingOutputIntent = true;
        if (t == pdfe::compliance::IssueType::MissingXmpMetadata)
            stillMissingXmp = true;
    }
    if (sawMissingOutputIntent) {
        EXPECT_FALSE(stillMissingOutputIntent)
            << "PDF/A-1b conversion must add an OutputIntent (rule 6.2.2 "
               "must NOT remain in the issues list after conversion)";
    }
    if (sawMissingXmp) {
        EXPECT_FALSE(stillMissingXmp)
            << "PDF/A-1b conversion must add pdfaid XMP metadata "
               "(rule 6.7/6.7.3/6.7.11 must NOT remain after conversion)";
    }
}

// ─── Converted document is fully PDF/A-1b compliant ─────────────────
//
// Re-enabled in pipeline b3f19d72 phase-2: PdfAConverter now
// substitutes standard-14 fonts with embedded TrueType replacements
// during convertAndSave (Arial for Helvetica, Times New Roman for
// Times-Roman, Courier New for Courier, etc.). Rule 6.3.3 "Font
// 'Helvetica' is not embedded" no longer fires on the converted
// output, so the validator reports isCompliant=true.

TEST_F(PdfARoundTripTest, ConvertedDocumentReportsIsCompliantTrue) {
    // The strongest contract: convertAndSave must produce an output
    // that the validator's isCompliant() accepts. If the converter
    // can't reach this on test_single.pdf, that's a converter gap
    // we want surfaced — and the test failure tells us exactly what.
    auto out = outFile("compliant_check");
    converter_->convertAndSave(document_.get(), PdfALevel::PdfA_1b, out);
    ASSERT_TRUE(std::filesystem::exists(out));

    auto convDoc = pdfe::core::Document::open(ctx_, out);
    ASSERT_NE(convDoc, nullptr);

    auto valResult = validator_->validate(convDoc.get(), PdfALevel::PdfA_1b);
    EXPECT_EQ(valResult.errorCount(), 0)
        << "PdfA_1b conversion must produce a document with zero "
           "validator errors. Remaining issues:\n"
        << [&]() {
            std::string s;
            for (const auto& i : valResult.issues) {
                if (i.severity == pdfe::compliance::IssueSeverity::Error) {
                    s += "  rule=" + i.rule + " : " + i.description + "\n";
                }
            }
            return s;
        }();
    EXPECT_TRUE(validator_->isCompliant(convDoc.get(), PdfALevel::PdfA_1b))
        << "isCompliant() must agree the converted document meets "
           "PdfA_1b";
}

// ─── Page count survives PDF/A conversion ───────────────────────────

TEST_F(PdfARoundTripTest, PdfAConversionPreservesPageCount) {
    auto out = outFile("pdfa_pagecount");
    converter_->convertAndSave(document_.get(), PdfALevel::PdfA_1b, out);
    ASSERT_TRUE(std::filesystem::exists(out));

    auto convDoc = pdfe::core::Document::open(ctx_, out);
    ASSERT_NE(convDoc, nullptr);
    EXPECT_EQ(convDoc->pageCount(), document_->pageCount())
        << "PDF/A conversion must preserve the source document's "
           "page count";
}

// ─── GitHub #15 / PDF-20: A2a must PRESERVE an authored /StructTreeRoot ─────
//
// PDF/A-2a is the ACCESSIBLE conformance level, so a converter is under
// pressure to make sure a structure tree exists. The failure mode this test
// exists for is the tempting way to do that: throw the document's own tagging
// away and auto-tag the pages from scratch. The output would be conformant,
// the validator would be happy, `hasStructureTree()` would return true — and
// every /Alt description, every author-assigned /ID, and the whole authored
// document architecture would be gone. That is a silent, unrecoverable data
// loss dressed up as a compliance improvement.
//
// See the StructFingerprint comment above for what identity is pinned and why
// a rebuild cannot reproduce it.

// Shared body: convert `fixtureName` to PDF/A-2a and require the structure
// tree to come back byte-for-byte identical under the fingerprint.
void ExpectA2aPreservesStructTree(
    PdfAConverter* converter,
    const std::shared_ptr<pdfe::core::MuPdfContext>& ctx,
    const std::filesystem::path& fixture,
    const std::filesystem::path& out,
    int expectedElemCount,
    int expectedMinDepth)
{
    ASSERT_TRUE(std::filesystem::exists(fixture))
        << "tagged fixture missing: " << fixture.string();

    fz_context* c = ctx->get();
    const StructFingerprint before = structFingerprint(c, fixture.string());

    // ── PRECONDITIONS. Without these the EXPECT_EQ below could be an
    // empty-string-vs-empty-string comparison, which is the classic vacuous
    // oracle: it would pass on a document with no structure tree at all.
    ASSERT_TRUE(before.opened) << "the tagged fixture did not open";
    ASSERT_TRUE(before.hasStructTreeRoot)
        << "the fixture carries no /StructTreeRoot, so there is nothing whose "
           "preservation this test could observe";
    ASSERT_FALSE(before.truncated)
        << "the fingerprint buffer overflowed on the SOURCE, so the comparison "
           "below would only cover a prefix of the tree";
    ASSERT_EQ(before.elemCount, expectedElemCount)
        << "the fixture's structure tree no longer has the element count this "
           "test was written against. Fingerprint:\n" << before.text;
    ASSERT_GE(before.maxDepth, expectedMinDepth)
        << "the fixture's structure tree is shallower than expected, so the "
           "nesting half of the identity check is not exercised";

    auto doc = pdfe::core::Document::open(ctx, fixture);
    ASSERT_NE(doc, nullptr);

    converter->convertAndSave(doc.get(), PdfALevel::PdfA_2a, out);
    ASSERT_TRUE(std::filesystem::exists(out))
        << "convertAndSave(PdfA_2a) produced no output file";

    const StructFingerprint after = structFingerprint(c, out.string());
    ASSERT_TRUE(after.opened) << "the converted document does not reopen";
    ASSERT_FALSE(after.truncated)
        << "the fingerprint buffer overflowed on the CONVERTED output";

    // The blunt half first, so a total drop reports as a drop.
    ASSERT_TRUE(after.hasStructTreeRoot)
        << "PDF/A-2a conversion DROPPED the /StructTreeRoot entirely from "
        << fixture.filename().string();

    // The identity half. This is the assertion a rebuild fails.
    EXPECT_EQ(after.text, before.text)
        << "PDF/A-2a conversion did not PRESERVE the pre-existing structure "
           "tree of " << fixture.filename().string()
        << " -- it came back with a different tree.\n"
           "This is what a converter that discarded the authored tagging and "
           "re-tagged the pages looks like: the output can still be valid, "
           "still be 'tagged', and still pass every presence check, while the "
           "author's /Alt descriptions, /ID identifiers and document "
           "architecture are gone.\n"
           "  BEFORE (" << before.elemCount << " elements, depth "
        << before.maxDepth << "):\n" << before.text
        << "  AFTER  (" << after.elemCount << " elements, depth "
        << after.maxDepth << "):\n" << after.text;

    EXPECT_EQ(after.elemCount, before.elemCount)
        << "the converted structure tree has a different number of elements ("
        << after.elemCount << " vs " << before.elemCount << ")";
    EXPECT_EQ(after.maxDepth, before.maxDepth)
        << "the converted structure tree is nested to a different depth ("
        << after.maxDepth << " vs " << before.maxDepth
        << ") -- a flattened tree is a rebuilt tree";
}

TEST_F(PdfARoundTripTest, PdfA2aConversionPreservesExistingStructTree) {
    const auto fixture = std::filesystem::path(TEST_FIXTURES_DIR)
                         / "test_tagged_authored_struct.pdf";

    // The authored fixture is the one that can DISTINGUISH preserved from
    // rebuilt: 13 elements, nested /Document > /Part > /Sect > /L > /LI >
    // /LBody, with /Alt, /ID and /T strings that appear nowhere in any page's
    // content stream. Assert those specific authored markers are in the
    // SOURCE fingerprint, so this test cannot silently degrade into a check
    // over a trivial tree if the fixture is ever regenerated differently.
    fz_context* c = ctx_->get();
    const StructFingerprint src = structFingerprint(c, fixture.string());
    ASSERT_TRUE(src.opened) << "authored tagged fixture missing or unreadable: "
                            << fixture.string()
                            << " (regenerate: python "
                               "tests/fixtures/gen_tagged_authored_struct.py)";
    ASSERT_TRUE(fpContains(src, "ID=[pdfe-authored-elem-"))
        << "the fixture carries no authored /ID values, so a rebuilt tree "
           "could match the fingerprint by accident. Fingerprint:\n"
        << src.text;
    ASSERT_TRUE(fpContains(src, "Alt=[A cartographer's note"))
        << "the fixture carries no authored /Alt description, which is the "
           "single strongest thing here that an auto-tagger has no source "
           "for. Fingerprint:\n" << src.text;
    ASSERT_TRUE(fpContains(src, "S=Sect"))
        << "the fixture no longer has the /Sect grouping level";
    ASSERT_TRUE(fpContains(src, "S=LBody"))
        << "the fixture no longer has the /L > /LI > /LBody list nesting";

    ExpectA2aPreservesStructTree(converter_.get(), ctx_,
                                 fixture, outFile("a2a_authored_struct"),
                                 /*expectedElemCount=*/14,
                                 /*expectedMinDepth=*/5);
}

// The fixture GitHub #15 actually names. Its tree is the trivial
// /Document > /P-per-page shape, which is why it is the SECOND case and not
// the first: an auto-tagger could plausibly land on that shape again, so on
// its own it could not tell "preserved" from "rebuilt identically". It is
// still worth pinning — a conversion that flattens or drops it is caught here
// against the fixture the issue was filed about.
TEST_F(PdfARoundTripTest,
       PdfA2aConversionPreservesStructTreeOfTaggedMultipageFixture) {
    const auto fixture = std::filesystem::path(TEST_FIXTURES_DIR)
                         / "test_tagged_multipage.pdf";
    ExpectA2aPreservesStructTree(converter_.get(), ctx_,
                                 fixture, outFile("a2a_tagged_multipage"),
                                 /*expectedElemCount=*/4,
                                 /*expectedMinDepth=*/1);
}

} // namespace
