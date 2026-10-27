// -*- coding: utf-8 -*-
//
// Finding R3-08: OCR / TRE recognition-accuracy fidelity oracle.
//
// The existing test_ocr_pipeline.cpp only asserts that OcrPipeline::ocrPage()
// returns a NON-EMPTY HOCR string with a well-formed structure (blocks > 0,
// page width/height > 0). It never compares the recognized TEXT to a known
// ground truth. That means a mutation which makes Tesseract emit wrong /
// garbage / transposed words — but still non-empty, still structurally valid
// HOCR — sails through the current suite undetected. The E2E test
// (test_e2e_integration.cpp, E2EOcr) does compare against ground truth, but
// only via token-set RECALL, which is order-insensitive and ignores
// insertions: an engine that emits every expected word PLUS a flood of
// hallucinated tokens, or that scrambles word order, still scores recall 1.0.
//
// This file adds the missing accuracy oracle: WORD ERROR RATE (WER) and
// CHARACTER ERROR RATE (CER), both computed as an ordered Levenshtein edit
// distance between the OCR output and the committed ground-truth sidecar,
// normalized by the ground-truth length. WER/CER are the standard OCR/ASR
// accuracy metrics and — unlike recall — they PENALIZE insertions,
// deletions, substitutions AND order. A garbage-emitting mutation drives WER
// toward (or above) 1.0 and fails the bound.
//
// Fixtures (committed, with ground-truth sidecars):
//   test_ocr_clean.pdf     + test_ocr_clean.expected.txt   (pangrams; clean scan)
//   test_ocr_skewed.pdf    + test_ocr_skewed.expected.txt  (rotated/noisy scan)
//
// Ground truth (from the sidecars):
//   clean : "The quick brown fox jumps over the lazy dog. Pack my box with
//            five dozen liquor jugs. Sphinx of black quartz, judge my vow."
//   skewed: "Rotated and noisy page. Tesseract should still extract most of
//            this. Line two continues the content."
//
// Skips (never a silent pass) when tessdata or a fixture/sidecar is absent.
// OCR is slow, so every case runs a SINGLE page at the same 300 DPI the
// existing OCR tests use, matching test_e2e_integration.cpp's config.
#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "tre/HocrParser.h"
#include "tre/OcrPipeline.h"
#include "tre/TreTypes.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace pdfe::core;
using namespace pdfe::tre;

namespace {

std::filesystem::path fixturesDir() { return std::filesystem::path(TEST_FIXTURES_DIR); }

std::string readAll(const std::filesystem::path& p)
{
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Normalize + split a UTF-8 string into lowercase word tokens with all ASCII
// punctuation stripped and whitespace collapsed. Byte-level normalization is
// sufficient for the Latin-script fixtures used here (matches the tokenizer
// style in test_e2e_integration.cpp).
std::vector<std::string> normalizeWords(const std::string& s)
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

// Normalized character stream (lowercase, punctuation dropped, whitespace
// collapsed to single spaces) for CER.
std::string normalizeChars(const std::string& s)
{
    const auto words = normalizeWords(s);
    std::string joined;
    for (size_t i = 0; i < words.size(); ++i) {
        if (i > 0) joined.push_back(' ');
        joined += words[i];
    }
    return joined;
}

// Generic Levenshtein (edit) distance over any sequence with == comparable
// elements. Substitution/insertion/deletion each cost 1. Ordered — unlike a
// set-intersection recall, transpositions and insertions are penalized.
template <typename Seq>
size_t levenshtein(const Seq& a, const Seq& b)
{
    const size_t n = a.size();
    const size_t m = b.size();
    if (n == 0) return m;
    if (m == 0) return n;
    std::vector<size_t> prev(m + 1), curr(m + 1);
    for (size_t j = 0; j <= m; ++j) prev[j] = j;
    for (size_t i = 1; i <= n; ++i) {
        curr[0] = i;
        for (size_t j = 1; j <= m; ++j) {
            const size_t cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            curr[j] = std::min({prev[j] + 1,        // deletion
                                curr[j - 1] + 1,    // insertion
                                prev[j - 1] + cost}); // substitution
        }
        std::swap(prev, curr);
    }
    return prev[m];
}

// Word Error Rate = edit_distance(words) / |reference words|.
// Can exceed 1.0 when the hypothesis has more errors than reference words
// (e.g. garbage output with many insertions) — that is the intended,
// mutation-killing behavior.
double wordErrorRate(const std::string& hypothesis, const std::string& reference)
{
    const auto refWords = normalizeWords(reference);
    const auto hypWords = normalizeWords(hypothesis);
    if (refWords.empty()) return hypWords.empty() ? 0.0 : 1.0;
    const size_t dist = levenshtein(hypWords, refWords);
    return static_cast<double>(dist) / static_cast<double>(refWords.size());
}

// Character Error Rate = edit_distance(chars) / |reference chars|.
double charErrorRate(const std::string& hypothesis, const std::string& reference)
{
    const std::string refChars = normalizeChars(reference);
    const std::string hypChars = normalizeChars(hypothesis);
    if (refChars.empty()) return hypChars.empty() ? 0.0 : 1.0;
    const size_t dist = levenshtein(hypChars, refChars);
    return static_cast<double>(dist) / static_cast<double>(refChars.size());
}

// Walk parsed HOCR (blocks -> paragraphs -> lines -> words) into a flat,
// reading-order plain-text string. Mirrors the extraction in
// test_e2e_integration.cpp so the two suites agree on what "OCR output" means.
std::string hocrToPlainText(const std::string& hocr)
{
    const auto doc = HocrParser::parse(hocr);
    std::ostringstream out;
    for (const auto& block : doc.blocks) {
        for (const auto& para : block.paragraphs) {
            for (const auto& line : para.lines) {
                for (size_t w = 0; w < line.words.size(); ++w) {
                    if (w > 0) out << ' ';
                    out << line.words[w].text;
                }
                out << '\n';
            }
        }
    }
    return out.str();
}

// Shared fixture guard: returns false (and records a GTEST_SKIP reason via
// the out-param) when tessdata or the requested fixture/sidecar is missing.
// Never lets the caller silently pass.
struct OcrFixture {
    std::filesystem::path pdf;
    std::filesystem::path txt;
    std::filesystem::path tessdata;
    std::string groundTruth;
    bool ready = false;
    std::string skipReason;
};

OcrFixture loadFixture(const char* pdfName, const char* txtName)
{
    OcrFixture f;
    f.pdf = fixturesDir() / pdfName;
    f.txt = fixturesDir() / txtName;
    f.tessdata = fixturesDir() / "tessdata";
    const auto traineddata = f.tessdata / "eng.traineddata";

    if (!std::filesystem::exists(f.pdf) || !std::filesystem::exists(f.txt)) {
        f.skipReason = "OCR fixture or ground-truth sidecar missing: " + f.pdf.string();
        return f;
    }
    if (!std::filesystem::exists(traineddata)) {
        f.skipReason = "Tesseract eng.traineddata not deployed at " + traineddata.string();
        return f;
    }
    f.groundTruth = readAll(f.txt);
    if (f.groundTruth.empty()) {
        f.skipReason = "Ground-truth sidecar is empty: " + f.txt.string();
        return f;
    }
    f.ready = true;
    return f;
}

// Run OCR on page 0 of a fixture at 300 DPI and return the flattened text.
std::string runOcrPlainText(const OcrFixture& f)
{
    auto ctx = std::make_shared<MuPdfContext>();
    auto doc = Document::open(ctx, f.pdf);
    EXPECT_NE(doc, nullptr) << "Failed to open OCR fixture: " << f.pdf;
    if (!doc) return {};

    OcrConfig config;
    config.language = "eng";
    config.tessdataPath = f.tessdata.string();
    config.ocrDpi = 300;   // matches test_ocr_pipeline / test_e2e_integration

    OcrPipeline ocr(ctx.get(), doc.get(), config);
    const std::string hocr = ocr.ocrPage(0);
    EXPECT_FALSE(hocr.empty())
        << "OcrPipeline::ocrPage(0) returned empty HOCR — Tesseract init or "
           "rendering failed for " << f.pdf;
    if (hocr.empty()) return {};
    return hocrToPlainText(hocr);
}

} // namespace

// --------------------------------------------------------------------------
// Self-checks for the WER/CER oracle so the thresholds below are trustworthy.
// --------------------------------------------------------------------------

TEST(FidelityR3_08_OcrWer, WerIsZeroOnExactMatch)
{
    const std::string ref = "The quick brown fox jumps over the lazy dog.";
    EXPECT_DOUBLE_EQ(wordErrorRate(ref, ref), 0.0);
    EXPECT_DOUBLE_EQ(charErrorRate(ref, ref), 0.0);
    // Normalization must make case/punctuation irrelevant.
    EXPECT_DOUBLE_EQ(wordErrorRate("the QUICK brown fox, jumps over the lazy dog",
                                   ref), 0.0);
}

TEST(FidelityR3_08_OcrWer, WerPenalizesSubstitutionInsertionDeletion)
{
    const std::string ref = "the quick brown fox";  // 4 words
    // One substitution (brown -> green): distance 1 / 4 = 0.25
    EXPECT_NEAR(wordErrorRate("the quick green fox", ref), 0.25, 1e-9);
    // One deletion (drop "quick"): distance 1 / 4 = 0.25
    EXPECT_NEAR(wordErrorRate("the brown fox", ref), 0.25, 1e-9);
    // One insertion: distance 1 / 4 = 0.25
    EXPECT_NEAR(wordErrorRate("the quick brown red fox", ref), 0.25, 1e-9);
}

TEST(FidelityR3_08_OcrWer, WerCatchesGarbageThatRecallWouldMiss)
{
    // The mutation-kill rationale: a hypothesis that contains every reference
    // word PLUS a flood of hallucinated tokens scores perfect recall (1.0),
    // but WER exposes it because insertions are penalized. This is exactly
    // the accuracy failure the existing non-empty-HOCR assertion can't see.
    const std::string ref = "the quick brown fox";
    const std::string garbage =
        "the xyz quick zzz brown qqq fox aaa bbb ccc ddd eee fff ggg";
    EXPECT_GT(wordErrorRate(garbage, ref), 0.5)
        << "WER must penalize hallucinated insertions that recall ignores";

    // Fully wrong output: WER at or above 1.0.
    EXPECT_GE(wordErrorRate("lorem ipsum dolor sit", ref), 1.0);
}

// --------------------------------------------------------------------------
// The accuracy oracle the current suite lacks: OCR output vs. ground truth.
// --------------------------------------------------------------------------

// Clean synthetic scan — three pangrams. A healthy eng.traineddata run should
// reproduce this near-perfectly. Bound: WER <= 0.15 (allows a couple of
// per-word slips across ~24 words for Tesseract-version drift while still
// failing hard on garbage/transposed output). CER is reported and bounded
// looser as a secondary signal.
TEST(FidelityR3_08_OcrWer, CleanScanWordErrorRateWithinBound)
{
    OcrFixture f = loadFixture("test_ocr_clean.pdf", "test_ocr_clean.expected.txt");
    if (!f.ready) {
        GTEST_SKIP() << f.skipReason;
    }

    const std::string plain = runOcrPlainText(f);
    ASSERT_FALSE(plain.empty())
        << "Parsed HOCR produced no words for the clean-scan fixture.";

    const double wer = wordErrorRate(plain, f.groundTruth);
    const double cer = charErrorRate(plain, f.groundTruth);

    EXPECT_LE(wer, 0.15)
        << "Clean-scan OCR word error rate exceeded 0.15.\n"
           "  WER = " << wer << ", CER = " << cer << "\n"
           "  Ground truth: " << f.groundTruth << "\n"
           "  OCR output  : " << plain << "\n"
           "If this regresses only slightly after a Tesseract upgrade or a\n"
           "fixture regeneration, raise the bound to the measured floor and\n"
           "document it — do NOT weaken it to accommodate a genuine accuracy\n"
           "regression (that is the defect this oracle exists to catch).";

    EXPECT_LE(cer, 0.10)
        << "Clean-scan OCR character error rate exceeded 0.10 (CER = " << cer << ").";
}

// Skewed / noisy synthetic scan — same ground-truth-comparison oracle but a
// deliberately looser bound, since rotation + noise legitimately degrade
// recognition. WER <= 0.40 still fails on garbage while tolerating the
// expected accuracy hit from skew. This is the separate looser-bound case the
// finding calls for.
TEST(FidelityR3_08_OcrWer, SkewedScanWordErrorRateWithinLooserBound)
{
    OcrFixture f = loadFixture("test_ocr_skewed.pdf", "test_ocr_skewed.expected.txt");
    if (!f.ready) {
        GTEST_SKIP() << f.skipReason;
    }

    const std::string plain = runOcrPlainText(f);
    ASSERT_FALSE(plain.empty())
        << "Parsed HOCR produced no words for the skewed-scan fixture.";

    const double wer = wordErrorRate(plain, f.groundTruth);
    const double cer = charErrorRate(plain, f.groundTruth);

    EXPECT_LE(wer, 0.40)
        << "Skewed-scan OCR word error rate exceeded the looser 0.40 bound.\n"
           "  WER = " << wer << ", CER = " << cer << "\n"
           "  Ground truth: " << f.groundTruth << "\n"
           "  OCR output  : " << plain << "\n"
           "This bound is intentionally loose (rotation + noise degrade OCR);\n"
           "a value near or above 1.0 means recognition is effectively broken.";
}
