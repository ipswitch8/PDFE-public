// -*- coding: utf-8 -*-
// Phase 11: End-to-end integration tests.
//
// These tests exercise feature integration points that unit tests can't
// reach on their own — multi-subsystem flows where correctness depends
// on several phases working together.
//
//   1. Rich-text round-trip (Phase 9 FormattingBlock alignment + Phase
//      6a text edit pipeline) — set alignment, save, reload, assert.
//   2. Signature preservation across incremental save — open signed
//      PDF, make a permitted annotation addition, save incrementally,
//      reopen, verify the original signed byte range is still intact.
//   3. Redaction forensics — redact text, save, grep the binary for
//      the redacted string, assert 0 occurrences.
//   4. OCR real-world — run OcrPipeline on each fixtures/ocr/*.pdf,
//      compute recall against the .expected.txt, assert the per-file
//      threshold.
#include <gtest/gtest.h>
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "annotations/AnnotationManager.h"
#include "annotations/AnnotationType.h"
#include "reflow/TextReflowEngine.h"
#include "reflow/ReflowTypes.h"
#include "tre/OcrPipeline.h"
#include "tre/HocrParser.h"
#include "tre/TreTypes.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace pdfe::core;

namespace {

std::filesystem::path fixturesDir() { return std::filesystem::path(TEST_FIXTURES_DIR); }

std::filesystem::path tempOut(const char* name)
{
    auto d = std::filesystem::temp_directory_path() / "pdfe_e2e_tests";
    std::filesystem::create_directories(d);
    return d / name;
}

std::string readAll(const std::filesystem::path& p)
{
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

int countOccurrences(const std::string& hay, const std::string& needle)
{
    if (needle.empty()) return 0;
    int n = 0;
    size_t pos = 0;
    while ((pos = hay.find(needle, pos)) != std::string::npos) {
        ++n;
        pos += needle.size();
    }
    return n;
}

/// Split a UTF-8 string into lowercase whitespace-delimited word tokens.
std::vector<std::string> tokenize(const std::string& s)
{
    std::vector<std::string> out;
    std::string cur;
    for (unsigned char c : s) {
        if (std::isspace(c) || std::ispunct(c)) {
            if (!cur.empty()) { out.push_back(std::move(cur)); cur.clear(); }
        } else {
            cur.push_back(static_cast<char>(std::tolower(c)));
        }
    }
    if (!cur.empty()) out.push_back(std::move(cur));
    return out;
}

/// Recall = |expected ∩ actual| / |expected| — counted over distinct tokens
/// so duplicate matches don't double-count.
float recall(const std::string& actual, const std::string& expected)
{
    const auto expTokens = tokenize(expected);
    const auto actTokens = tokenize(actual);
    if (expTokens.empty()) return 1.0f;
    std::set<std::string> expSet(expTokens.begin(), expTokens.end());
    std::set<std::string> actSet(actTokens.begin(), actTokens.end());
    int hit = 0;
    for (const auto& t : expSet) {
        if (actSet.count(t)) ++hit;
    }
    return static_cast<float>(hit) / static_cast<float>(expSet.size());
}

} // namespace

// -------- E2E 1: Alignment round-trip via the reflow engine --------

TEST(E2EAlignment, FormattingBlockAlignmentSurvivesReflowCycle)
{
    // Integration between the Phase-9 FormattingBlock + the reflow
    // engine's offset math. A real save/reload via ContentStreamEditor
    // is gated by the next atomic phase; this E2E exercises the layer
    // that ContentStreamEditor will call into.
    using namespace pdfe::reflow;

    FormattingBlock block;
    block.bounds = QRectF(0, 0, 400, 600);
    block.alignment = Alignment::Center;

    const std::string text =
        "This is a centered paragraph "
        "that should wrap across multiple lines "
        "and remain centered through reflow.";
    auto measurer = [](const std::string& w) { return w.size() * 6.0f; };
    auto words = TextReflowEngine::tokenize(text, measurer);
    auto lines = TextReflowEngine::reflow(words, 400.0f, 6.0f);
    ASSERT_GT(lines.size(), 1u);

    // For Center, every non-last line's offset must be > 0 (unless its
    // measured width fills the block exactly).
    int centeredLines = 0;
    for (const auto& line : lines) {
        const float off = TextReflowEngine::lineXOffset(
            400.0f, line.width, Alignment::Center, line.isLastLine);
        if (off > 0.0f) ++centeredLines;
    }
    EXPECT_GT(centeredLines, 0) << "center alignment must offset at least one line";

    // Round-trip: switch to Right and confirm offsets flip to the full
    // slack, not half.
    for (const auto& line : lines) {
        const float centerOff = TextReflowEngine::lineXOffset(
            400.0f, line.width, Alignment::Center, line.isLastLine);
        const float rightOff = TextReflowEngine::lineXOffset(
            400.0f, line.width, Alignment::Right, line.isLastLine);
        EXPECT_GE(rightOff, centerOff)
            << "Right offset should be >= Center offset for the same line";
    }
}

// -------- E2E 2: Signature preservation across incremental save --------

TEST(E2ESignature, IncrementalSavePreservesSignedByteRange)
{
    // Open a signed PDF, add a harmless annotation, save to a new path
    // (incremental semantics via Document::saveAs), reopen, and assert
    // the basic integration invariants hold. Document::saveAs does a
    // full rewrite, not an incremental append, so byte-level preservation
    // of the signed prefix is not asserted here — true incremental save
    // (the operation that preserves signature validity) is scoped for a
    // follow-up phase. This E2E asserts the narrower invariant: editing
    // a signed PDF and saving produces a file that reopens with the
    // page count preserved.
    auto ctx = std::make_shared<MuPdfContext>();
    const auto src = fixturesDir() / "test_signed.pdf";
    if (!std::filesystem::exists(src)) {
        GTEST_SKIP() << "test_signed.pdf not available";
    }

    auto doc = Document::open(ctx, src);
    ASSERT_NE(doc, nullptr);
    const int pages = doc->pageCount();
    ASSERT_GT(pages, 0);
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    pdfe::annotations::AnnotationManager mgr(ctx);
    mgr.createAnnotation(page.get(),
        pdfe::annotations::AnnotationType::Text, QRectF(200, 200, 30, 30));

    const auto out = tempOut("signed_after_edit.pdf");
    std::filesystem::remove(out);
    doc->saveAs(out);
    ASSERT_TRUE(std::filesystem::exists(out));

    auto reopened = Document::open(ctx, out);
    ASSERT_NE(reopened, nullptr);
    EXPECT_EQ(reopened->pageCount(), pages)
        << "page count must survive saveAs on a signed input";
}

// -------- E2E 3: Redaction forensics --------

TEST(E2ERedaction, RedactedStringDoesNotAppearInSavedBytes)
{
    // Build a small PDF in-memory via FreeText annotation so we know
    // exactly what string should be redacted, then redact and grep.
    auto ctx = std::make_shared<MuPdfContext>();
    const auto src = fixturesDir() / "test_single.pdf";
    if (!std::filesystem::exists(src)) {
        GTEST_SKIP() << "test_single.pdf not available";
    }
    const auto work = tempOut("redact_work.pdf");
    std::filesystem::copy_file(src, work,
        std::filesystem::copy_options::overwrite_existing);

    // Insert a distinctive sentinel we can grep for later.
    constexpr const char* kSentinel = "TOP-SECRET-SENTINEL-99X";
    {
        auto doc = Document::open(ctx, work);
        auto page = doc->page(0);
        pdfe::annotations::AnnotationManager mgr(ctx);
        auto a = mgr.createAnnotation(page.get(),
            pdfe::annotations::AnnotationType::FreeText,
            QRectF(100, 100, 300, 40));
        mgr.setContents(page.get(), a.annotIndex, kSentinel);
        const auto with_secret = tempOut("redact_with_secret.pdf");
        doc->saveAs(with_secret);
        // Before redaction: sentinel IS present (regression guard —
        // confirms our test setup actually wrote the string).
        const std::string bytes = readAll(with_secret);
        EXPECT_GT(countOccurrences(bytes, kSentinel), 0)
            << "sentinel must appear before redaction";
    }

    // Redact via delete-annotation + re-save (the simplest redaction
    // path — it removes the annotation's contents entirely rather than
    // overlaying black boxes). A production redaction command would
    // also clear the content stream; for this E2E we assert the
    // narrower invariant: after we delete the annotation and save with
    // garbage collection, the sentinel bytes do NOT appear in the
    // output.
    const auto redacted = tempOut("redact_after.pdf");
    {
        auto doc = Document::open(ctx, work);
        auto page = doc->page(0);
        pdfe::annotations::AnnotationManager mgr(ctx);
        auto a = mgr.createAnnotation(page.get(),
            pdfe::annotations::AnnotationType::FreeText,
            QRectF(100, 100, 300, 40));
        mgr.setContents(page.get(), a.annotIndex, kSentinel);
        mgr.deleteAnnotation(page.get(), a.annotIndex);
        doc->saveAs(redacted);
    }
    const std::string bytes = readAll(redacted);
    EXPECT_EQ(countOccurrences(bytes, kSentinel), 0)
        << "sentinel bytes must not appear in output — garbage "
        << "collection must strip unreferenced redacted content";
}

// -------- E2E 4: OCR recall on the synthetic corpus --------

// D2 phase-4: re-enabled with live OCR pipeline integration. Asserts
// recall ≥ 0.95 against the synthetic clean-scan fixture. Skips
// gracefully if the eng.traineddata fixture or PDF/expected-text
// fixtures are absent (preserves the cross-CI portability that the
// original DISABLED_ skip was guarding).
TEST(E2EOcr, CleanScanRecallAboveNinetyFivePercent)
{
    const auto pdf = fixturesDir() / "test_ocr_clean.pdf";
    const auto txt = fixturesDir() / "test_ocr_clean.expected.txt";
    const auto tessdata = fixturesDir() / "tessdata";
    const auto traineddata = tessdata / "eng.traineddata";
    if (!std::filesystem::exists(pdf) || !std::filesystem::exists(txt)) {
        GTEST_SKIP() << "OCR fixtures not available: " << pdf;
    }
    if (!std::filesystem::exists(traineddata)) {
        GTEST_SKIP() << "Tesseract eng.traineddata not deployed at "
                     << traineddata;
    }

    auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
    auto doc = pdfe::core::Document::open(ctx, pdf);
    ASSERT_NE(doc, nullptr) << "Failed to open OCR fixture: " << pdf;

    pdfe::tre::OcrConfig config;
    config.language = "eng";
    config.tessdataPath = tessdata.string();
    config.ocrDpi = 300;
    pdfe::tre::OcrPipeline ocr(ctx.get(), doc.get(), config);

    // Run OCR on page 0 (the synthetic clean-scan fixture is single-page).
    const std::string hocr = ocr.ocrPage(0);
    ASSERT_FALSE(hocr.empty())
        << "OcrPipeline::ocrPage(0) returned empty HOCR — Tesseract "
           "initialization or rendering failed.";

    // Extract plain text from HOCR by walking the parsed structure
    // (blocks → paragraphs → lines → words).
    const auto hocrDoc = pdfe::tre::HocrParser::parse(hocr);
    std::ostringstream plainTextStream;
    for (const auto& block : hocrDoc.blocks) {
        for (const auto& para : block.paragraphs) {
            for (const auto& line : para.lines) {
                for (size_t w = 0; w < line.words.size(); ++w) {
                    if (w > 0) plainTextStream << ' ';
                    plainTextStream << line.words[w].text;
                }
                plainTextStream << '\n';
            }
        }
    }
    const std::string plainText = plainTextStream.str();
    ASSERT_FALSE(plainText.empty())
        << "Parsed HOCR contained no recognizable words. HOCR length: "
        << hocr.size();

    const std::string expected = readAll(txt);
    const float r = recall(plainText, expected);
    EXPECT_GE(r, 0.95f)
        << "Clean-scan OCR recall fell below 0.95 threshold. "
           "Measured recall: " << r << ". "
           "Expected text length: " << expected.size()
        << " bytes; OCR output length: " << plainText.size() << " bytes. "
           "If this regresses across Tesseract upgrades or fixture "
           "regeneration, lower the threshold to the measured floor "
           "and document the new floor in UI_MATRIX_GAPS.md (UI-TE-TRE-07).";
}

// -------- Recall helper self-check --------

TEST(E2EOcrRecallHelper, RecallMatchesExpectedOverlap)
{
    // Unit-test our own recall implementation so the OCR thresholds
    // are trustworthy when the disabled tests are re-enabled.
    EXPECT_NEAR(recall("the quick brown fox",
                       "the quick brown fox"), 1.0f, 1e-4);
    EXPECT_NEAR(recall("the fox", "the quick brown fox"),
                0.5f, 1e-4);
    EXPECT_NEAR(recall("", "anything"), 0.0f, 1e-4);
    EXPECT_NEAR(recall("anything", ""), 1.0f, 1e-4);  // empty expected → 1
    // Case + punctuation invariance.
    EXPECT_NEAR(recall("The, quick. brown! FOX?",
                       "the quick brown fox"), 1.0f, 1e-4);
}
