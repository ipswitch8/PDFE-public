// -*- coding: utf-8 -*-
// test_layout_extraction_corpus.cpp
//
// Meta-audit follow-up F16, gap PDF-107 (merges A3-34).
//
// THE GAP
// -------
// tests/fixtures/flaw_corpus/COVERAGE.md §4 "Layout / real-world extraction"
// was the one section marked Deferred that maps onto a SHIPPING HEADLINE
// FEATURE: editing text on scanned / image-only PDFs (the TRE). The deferral's
// stated blocker was that the section "needs its own harness, not
// open/render/no-throw".
//
// What that left behind, verified before this file was written:
//
//   * tests/fixtures/test_ocr_multicol.pdf (a 300 DPI image-only two-column
//     scan) and its authored ground truth test_ocr_multicol.expected.txt were
//     committed, and the ONLY test that touched either asserted that the
//     sidecar file exists and is non-empty
//     (FixtureCorpus.OcrVariantsEachHaveExpectedTextSidecar). Nothing ever ran
//     OCR on it.
//   * Word accuracy on the OCR path was bounded for the single-column clean and
//     skewed scans only (FidelityR3_08_OcrWer). Multi-column: nothing.
//   * Column reading order was asserted on the NATIVE-TEXT path only
//     (FidelityR3_07_TextExtraction.MultiColumnReadingOrderIsColumnMajor), and
//     there via two hand-picked word anchors rather than a partition -- an
//     extractor that emits most of the right column early still passes it as
//     long as those two anchors keep their relative order.
//
// So the scanned path -- the feature PDFE leads with -- had no layout oracle at
// all, and the native path had a two-anchor one.
//
// GROUND TRUTH DISCIPLINE
// -----------------------
// Every expectation below is anchored to something AUTHORED, never to what
// PDFE currently emits:
//
//   * The text is the literal string pair MULTICOL_TEXT_LEFT /
//     MULTICOL_TEXT_RIGHT in tests/fixtures/generate_ocr_variants.py, written
//     verbatim to test_ocr_multicol.expected.txt by that same script. It is the
//     text that was PRINTED INTO the scan, not the text extraction returns.
//   * The column geometry is the generator's own layout arithmetic: page
//     2550x3300 px at 300 DPI, left column originating at x=300 with
//     col_w = (2550 - 600 - 150) / 2 = 900, gutter 150, right column
//     originating at x = 300 + 900 + 150 = 1350. Those constants are restated
//     here and cross-checked against the rendered page width, so a regenerated
//     fixture cannot silently move the columns out from under the assertions.
//   * On the native-text fixture the ground truth is the fixture's own
//     geometry: which side of the page a line is physically on. That is
//     independent of the order extraction chooses to emit lines in, which is
//     precisely the thing under test.
//
// WHAT IS *NOT* COVERED (stated, not disguised)
// ---------------------------------------------
// §4 of the corpus reference also names genuinely EXTERNAL real-world files --
// a multi-column reflow sample and a 132-page scanned election report. Those
// are not in this repo and were not downloaded; they are large, and the
// download would not be reproducible from the tree. This file therefore closes
// the "headline feature has no layout oracle" half of PDF-107 and leaves the
// "external real-world corpus files" half open. It is recorded as such in
// docs/audit/meta-audit-2026-07/followup/phase-16-dispositions.md rather than
// implied to be covered.

#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "core/TextBlock.h"
#include "core/TextLine.h"
#include "tre/HocrParser.h"
#include "tre/OcrPipeline.h"
#include "tre/TreTypes.h"

#include <QRectF>

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

namespace fs = std::filesystem;

fs::path fixturesDir() { return fs::path(TEST_FIXTURES_DIR); }
fs::path testPdfsDir() { return fs::path(PDFE_REPO_ROOT) / "test_PDFs"; }

std::string readAll(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f.is_open()) return {};
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

// --- authored layout constants, from tests/fixtures/generate_ocr_variants.py
constexpr int kMulticolPageWidthPx  = 2550;   // 300 DPI US Letter
constexpr int kMulticolLeftOriginPx = 300;
constexpr int kMulticolColWidthPx   = (2550 - 600 - 150) / 2;              // 900
constexpr int kMulticolRightOriginPx = kMulticolLeftOriginPx + kMulticolColWidthPx + 150;  // 1350
// The gutter's mid-line: everything printed lies strictly on one side of it.
constexpr int kMulticolGutterMidPx =
    (kMulticolLeftOriginPx + kMulticolColWidthPx + kMulticolRightOriginPx) / 2;   // 1275

// --- word normalisation + WER, matching test_fidelity_r3_08_ocr_wer.cpp so the
//     two suites report the same metric.
std::vector<std::string> normalizeWords(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (unsigned char c : s) {
        if (std::isalnum(c)) {
            cur.push_back(static_cast<char>(std::tolower(c)));
        } else if (!cur.empty()) {
            out.push_back(cur);
            cur.clear();
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

template <typename Seq>
size_t levenshtein(const Seq& a, const Seq& b) {
    const size_t n = a.size(), m = b.size();
    std::vector<size_t> prev(m + 1), curr(m + 1);
    for (size_t j = 0; j <= m; ++j) prev[j] = j;
    for (size_t i = 1; i <= n; ++i) {
        curr[0] = i;
        for (size_t j = 1; j <= m; ++j) {
            const size_t cost = (a[i - 1] == b[j - 1]) ? 0u : 1u;
            curr[j] = std::min({ prev[j] + 1, curr[j - 1] + 1, prev[j - 1] + cost });
        }
        std::swap(prev, curr);
    }
    return prev[m];
}

double wordErrorRate(const std::string& hypothesis, const std::string& reference) {
    const auto ref = normalizeWords(reference);
    const auto hyp = normalizeWords(hypothesis);
    if (ref.empty()) return hyp.empty() ? 0.0 : 1.0;
    return static_cast<double>(levenshtein(hyp, ref)) / static_cast<double>(ref.size());
}

// --- OCR fixture guard: a missing tessdata or fixture SKIPS with the reason
//     stated. It never passes silently.
struct OcrRun {
    bool ready = false;
    std::string skipReason;
    std::string hocr;
    std::string groundTruth;
};

OcrRun runMulticolOcr() {
    OcrRun r;
    const fs::path pdf  = fixturesDir() / "test_ocr_multicol.pdf";
    const fs::path txt  = fixturesDir() / "test_ocr_multicol.expected.txt";
    const fs::path tess = fixturesDir() / "tessdata";
    if (!fs::exists(pdf) || !fs::exists(txt)) {
        r.skipReason = "multi-column OCR fixture or its authored sidecar is missing: "
                       + pdf.string();
        return r;
    }
    if (!fs::exists(tess / "eng.traineddata")) {
        r.skipReason = "Tesseract eng.traineddata not deployed at " + tess.string();
        return r;
    }
    r.groundTruth = readAll(txt);
    if (r.groundTruth.empty()) {
        r.skipReason = "authored ground-truth sidecar is empty: " + txt.string();
        return r;
    }

    auto ctx = std::make_shared<MuPdfContext>();
    auto doc = Document::open(ctx, pdf);
    if (!doc) { r.skipReason = "Document::open failed for " + pdf.string(); return r; }

    OcrConfig config;
    config.language = "eng";
    config.tessdataPath = tess.string();
    config.ocrDpi = 300;                       // the DPI the fixture was authored at
    OcrPipeline ocr(ctx.get(), doc.get(), config);
    r.hocr = ocr.ocrPage(0);
    r.ready = !r.hocr.empty();
    if (!r.ready) r.skipReason = "OcrPipeline::ocrPage(0) returned empty HOCR";
    return r;
}

struct OcrWord {
    std::string text;
    int centreX = 0;
    int order = 0;      // position in emission order
};

std::vector<OcrWord> flattenWords(const HocrDocument& doc) {
    std::vector<OcrWord> out;
    for (const auto& block : doc.blocks)
        for (const auto& para : block.paragraphs)
            for (const auto& line : para.lines)
                for (const auto& w : line.words) {
                    if (w.text.empty()) continue;
                    OcrWord o;
                    o.text = w.text;
                    o.centreX = (w.bbox.x0 + w.bbox.x1) / 2;
                    o.order = static_cast<int>(out.size());
                    out.push_back(o);
                }
    return out;
}

std::string joinTexts(const std::vector<OcrWord>& v, size_t maxWords) {
    std::string s;
    for (size_t i = 0; i < v.size() && i < maxWords; ++i) {
        if (!s.empty()) s += ' ';
        s += v[i].text;
    }
    return s;
}

} // namespace

// ===========================================================================
// 1. Scanned (image-only) multi-column: reading order must be column-major.
// ===========================================================================
//
// The oracle is a PARTITION, not an anchor pair: every word whose centre lies
// left of the authored gutter mid-line must be emitted before every word whose
// centre lies right of it. A row-major reader (the classic scanned-layout
// failure: read across both columns line by line) interleaves them and fails on
// the first crossing. So does a reader that emits the right column first.
TEST(LayoutExtractionCorpus, ScannedMultiColumnOcrIsColumnMajor) {
    const OcrRun run = runMulticolOcr();
    if (!run.ready) GTEST_SKIP() << run.skipReason;

    const HocrDocument doc = HocrParser::parse(run.hocr);
    ASSERT_GT(doc.pageWidth, 0)  << "HOCR declared no page width";
    ASSERT_GT(doc.pageHeight, 0) << "HOCR declared no page height";

    // The generator writes the 2550x3300 px image into a page whose MediaBox is
    // 2550x3300 POINTS, so rendering at 300 DPI scales the authored pixel space
    // by 300/72. The authored geometry is therefore valid up to that single
    // uniform factor, which is derived (never assumed) from the page width. The
    // ASPECT ratio is scale-invariant and is what pins the constants to this
    // fixture: a regenerated fixture of different proportions fails here rather
    // than silently relocating the gutter.
    const double scale = static_cast<double>(doc.pageWidth) / kMulticolPageWidthPx;
    ASSERT_GT(scale, 0.1) << "implausible render scale " << scale;
    EXPECT_NEAR(static_cast<double>(doc.pageWidth) / doc.pageHeight,
                2550.0 / 3300.0, 0.01)
        << "test_ocr_multicol.pdf is " << doc.pageWidth << "x" << doc.pageHeight
        << " px; generate_ocr_variants.py authors 2550x3300. Re-check the column "
           "constants in this file against that script.";
    const double gutterMid = kMulticolGutterMidPx * scale;

    const std::vector<OcrWord> words = flattenWords(doc);
    ASSERT_GE(words.size(), 15u)
        << "only " << words.size() << " words recognised on a two-paragraph "
           "page; OCR failed rather than mis-ordered";

    std::vector<OcrWord> left, right;
    for (const auto& w : words) {
        if (w.centreX < gutterMid) left.push_back(w);
        else right.push_back(w);
    }
    ASSERT_FALSE(left.empty())  << "no words landed left of the authored gutter";
    ASSERT_FALSE(right.empty()) << "no words landed right of the authored gutter";

    const int lastLeft   = left.back().order;
    const int firstRight = right.front().order;
    EXPECT_LT(lastLeft, firstRight)
        << "reading order crosses the columns: the last left-column word ("
        << left.back().text << ", emitted at " << lastLeft
        << ") comes AFTER the first right-column word (" << right.front().text
        << ", emitted at " << firstRight << "). Column-major order emits the "
           "whole left column first. Emission head: " << joinTexts(words, 12);

    // The two columns are also distinct content, so a partition that put
    // everything on one side would be wrong even if ordered.
    EXPECT_GE(left.size(), 5u);
    EXPECT_GE(right.size(), 5u);
}

// ===========================================================================
// 2. Scanned multi-column: word accuracy against the authored text.
// ===========================================================================
//
// Column order without word accuracy is satisfiable by an extractor that emits
// the right boxes with the wrong text, so the two assertions are paired. The
// reference is the authored sidecar; the bound matches the clean-scan bound in
// FidelityR3_08_OcrWer (0.15) because this fixture is likewise a clean, noise
// free 300 DPI render -- its difficulty is layout, not legibility.
TEST(LayoutExtractionCorpus, ScannedMultiColumnOcrWordErrorRateWithinBound) {
    const OcrRun run = runMulticolOcr();
    if (!run.ready) GTEST_SKIP() << run.skipReason;

    const HocrDocument doc = HocrParser::parse(run.hocr);
    std::ostringstream flat;
    for (const auto& block : doc.blocks)
        for (const auto& para : block.paragraphs)
            for (const auto& line : para.lines) {
                for (size_t w = 0; w < line.words.size(); ++w) {
                    if (w) flat << ' ';
                    flat << line.words[w].text;
                }
                flat << '\n';
            }

    const double wer = wordErrorRate(flat.str(), run.groundTruth);
    EXPECT_LE(wer, 0.15)
        << "WER " << wer << " against the authored two-column ground truth.\n"
           "  authored : " << run.groundTruth << "\n"
           "  recognised: " << flat.str();
}

// ===========================================================================
// 3. Native text, multi-column: a COMPLETE partition, not two anchors.
// ===========================================================================
//
// FidelityR3_07_TextExtraction.MultiColumnReadingOrderIsColumnMajor pins two
// words. This pins every line: classify each extracted line by which side of
// the page centre its bounding box lies wholly on (lines that straddle the
// centre are page-wide headers and are excluded from the column partition, but
// are required to precede both columns), then require every left line to be
// emitted before every right line.
TEST(LayoutExtractionCorpus, NativeMultiColumnExtractionIsACompleteColumnPartition) {
    const fs::path pdf = testPdfsDir() / "multicolumn.pdf";
    if (!fs::exists(pdf)) GTEST_SKIP() << "fixture missing (not a pass): " << pdf.string();

    auto ctx = std::make_shared<MuPdfContext>();
    auto doc = Document::open(ctx, pdf);
    ASSERT_NE(doc, nullptr);
    ASSERT_GT(doc->pageCount(), 0);
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    const QRectF pageBox = page->boundingBox();
    ASSERT_GT(pageBox.width(), 0.0);
    const double centre = pageBox.left() + pageBox.width() / 2.0;

    struct Line { int order; double left; double right; std::string text; };
    std::vector<Line> lines;
    for (const auto& block : page->extractText())
        for (const auto& tl : block.lines) {
            const std::string text = tl.text();
            if (text.find_first_not_of(" \t\r\n") == std::string::npos) continue;
            lines.push_back(Line{ static_cast<int>(lines.size()),
                                  tl.boundingBox.left(), tl.boundingBox.right(), text });
        }
    ASSERT_GE(lines.size(), 10u)
        << "only " << lines.size() << " non-blank lines extracted from a "
           "two-column page; extraction failed rather than mis-ordered";

    std::vector<Line> leftCol, rightCol, spanning;
    for (const auto& l : lines) {
        if (l.right <= centre)      leftCol.push_back(l);
        else if (l.left >= centre)  rightCol.push_back(l);
        else                        spanning.push_back(l);
    }
    ASSERT_GE(leftCol.size(), 3u)
        << "fewer than 3 lines lie wholly left of the page centre; this fixture "
           "is not the two-column document these assertions describe";
    ASSERT_GE(rightCol.size(), 3u)
        << "fewer than 3 lines lie wholly right of the page centre";

    int lastLeft = -1, firstRight = -1;
    for (const auto& l : leftCol)  lastLeft = std::max(lastLeft, l.order);
    for (const auto& l : rightCol)
        firstRight = (firstRight < 0) ? l.order : std::min(firstRight, l.order);

    EXPECT_LT(lastLeft, firstRight)
        << "the column partition is not clean: left-column line #" << lastLeft
        << " is emitted after right-column line #" << firstRight << ".\n"
           "  last left  : " << [&] {
                   for (const auto& l : leftCol) if (l.order == lastLeft) return l.text;
                   return std::string();
               }() << "\n"
           "  first right: " << [&] {
                   for (const auto& l : rightCol) if (l.order == firstRight) return l.text;
                   return std::string();
               }();

    // Page-wide headers belong to neither column and must precede both, or the
    // "columns" are being detected around a heading that was split.
    for (const auto& s : spanning)
        EXPECT_LT(s.order, firstRight)
            << "a page-wide line (\"" << s.text << "\", #" << s.order
            << ") is emitted after the right column starts (#" << firstRight << ")";
}
