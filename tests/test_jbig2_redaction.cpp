// -*- coding: utf-8 -*-
//
// A1-88 — JBIG2 decode + REDACTION path.
//
// docs/audit/meta-audit-2026-07/status-register.md row A1-88 (from
// docs/audit/input-domain-audit.md:126,150) records this as OPEN with the
// evidence: "grep -rni jbig2 tests/*.cpp -> only tests/test_cve_replay.cpp
// (CVE-2020-15920 replay); no JBIG2 image fixture; no redaction-over-JBIG2
// test" — i.e. ZERO fixtures for the decode/redact path.
//
// That is the worst kind of gap for a redaction feature: JBIG2 is the standard
// encoding for SCANNED bi-level documents, which is exactly the material users
// redact. If the redaction pipeline mishandled a JBIG2 XObject — left it in the
// file, or removed only the drawing operator while the coded bitmap survived as
// an orphan object — the "redacted" PDF would still carry the confidential
// scan, recoverable with a text editor.
//
// Fixture: tests/fixtures/jbig2_redaction.pdf, built by
// tests/fixtures/gen_jbig2_redaction.py, which is the AUTHORED GROUND TRUTH.
// The 64x64 1-bit image (/Filter /JBIG2Decode, MMR generic region) is placed
// over user-space x 50..250, y 50..250 of a 300x300 page and is four quadrants:
//
//     +----------------+----------------+
//     |  TL: all BLACK |  TR: all WHITE |   device y  50..150
//     +----------------+----------------+
//     |  BL: speckle   |  BR: all BLACK |   device y 150..250
//     +----------------+----------------+
//        device x         device x
//         50..150          150..250
//
// plus a "KEEPVISIBLE" text line at user (50, 270) — OUTSIDE the redaction —
// as the must-survive control.
//
// MUTATIONS KILLED (per-test detail above each TEST_F):
//   * JBIG2 decode broken / polarity inverted / region placed at the wrong
//     offset (DecodesAuthoredBitmap)
//   * redaction that leaves the image drawn, or that only paints a cover box
//     over it while the pixels remain (RedactionRemovesAuthoredPattern)
//   * redaction that removes the /Do operator but leaves the coded JBIG2
//     stream in the saved file as a recoverable orphan object — i.e. setting
//     rewriteDocument = false, or dropping the image-XObject garbage-collection
//     (OriginalJbig2BytesAreNotRecoverable)
//   * over-broad redaction that destroys unrelated page content
//     (UnredactedContentSurvives)

#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/Jbig2SecurityGate.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "redaction/RedactionData.h"
#include "redaction/RedactionManager.h"
#include "support/RasterOracle.h"
#include "support/VisualOracle.h"

#include <QRectF>

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

using namespace pdfe::core;
using pdfe::redaction::RedactionApplyOptions;
using pdfe::redaction::RedactionImageMethod;
using pdfe::redaction::RedactionManager;
using pdfe::testsupport::darkFractionInBlock;
using pdfe::testsupport::Raster;
using pdfe::testsupport::renderPageRaster;

namespace {

// ---------------------------------------------------------------------------
// AUTHORED ground truth — mirrors tests/fixtures/gen_jbig2_redaction.py.
// ---------------------------------------------------------------------------
constexpr int kPageW = 300;
constexpr int kPageH = 300;
constexpr int kImgX = 50;       // user-space left edge
constexpr int kImgY = 50;       // user-space bottom edge
constexpr int kImgDrawW = 200;  // user-space width
constexpr int kImgDrawH = 200;  // user-space height
constexpr int kQuadHalf = 20;   // sampling half-side; each quadrant is 100 px

// Byte offset of the MMR-coded data inside the JBIG2 embedded stream, derived
// from the fixed segment layout the generator emits:
//     11  page-information segment header  (4 num + 1 flags + 1 refs
//                                           + 1 page-assoc + 4 length)
//   + 19  page-information segment data
//   + 11  generic-region segment header
//   + 17  region segment information field
//   +  1  generic-region flags byte (MMR = 1, so no AT pixel bytes follow)
//   = 59
constexpr size_t kMmrDataOffset = 59;

// Length of the verbatim run of ORIGINAL coded bytes searched for in the saved
// file. The generator's bottom-left quadrant is a deterministic 50%-density
// speckle precisely so this run is high-entropy and cannot collide by chance.
constexpr size_t kNeedleLen = 32;

std::filesystem::path srcFixture() {
    return std::filesystem::path(TEST_FIXTURES_DIR) / "jbig2_redaction.pdf";
}

std::filesystem::path tempRoot() {
    auto d = std::filesystem::temp_directory_path() / "pdfe_jbig2_redaction";
    std::filesystem::create_directories(d);
    return d;
}

long pdfeProcessId() {
#ifdef _WIN32
    return static_cast<long>(_getpid());
#else
    return static_cast<long>(getpid());
#endif
}

// #49 repeat-safety convention (see tests/test_redaction_completeness.cpp):
// applyAllRedactions() bakes in place onto doc->filePath(), and a reopened
// Document can still hold an output file when the next --gtest_repeat iteration
// re-saves the same path. So every run gets a UNIQUE fixture COPY and a UNIQUE
// output directory: the pid keeps concurrent shard processes disjoint, the
// atomic counter keeps repeated calls within one process disjoint.
std::filesystem::path uniqueWorkCopy(const std::filesystem::path& src,
                                     const char* stem) {
    static std::atomic<unsigned> counter{0};
    const unsigned n = counter.fetch_add(1, std::memory_order_relaxed);
    auto dst = tempRoot() / (std::string(stem) + "_" +
                             std::to_string(pdfeProcessId()) + "_" +
                             std::to_string(n) + ".pdf");
    std::filesystem::copy_file(src, dst,
                               std::filesystem::copy_options::overwrite_existing);
    return dst;
}

std::filesystem::path uniqueRunDir(const char* stem) {
    static std::atomic<unsigned> counter{0};
    const unsigned n = counter.fetch_add(1, std::memory_order_relaxed);
    auto d = tempRoot() / (std::string(stem) + "_" +
                           std::to_string(pdfeProcessId()) + "_" +
                           std::to_string(n));
    std::filesystem::create_directories(d);
    return d;
}

std::string readAllBytes(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)),
                       std::istreambuf_iterator<char>());
}

// Pull the raw (still JBIG2-coded) image stream out of the fixture bytes.
// The generator writes the image XObject uncompressed apart from the JBIG2
// coding itself, with the dictionary order
//   ... /Filter /JBIG2Decode /Length N >>\nstream\n <N bytes> \nendstream
// so /Length is the first one following the /JBIG2Decode name.
std::string extractJbig2Stream(const std::string& pdfBytes) {
    const size_t filterPos = pdfBytes.find("/JBIG2Decode");
    if (filterPos == std::string::npos) return {};
    const size_t lenPos = pdfBytes.find("/Length", filterPos);
    if (lenPos == std::string::npos) return {};
    const long len = std::strtol(pdfBytes.c_str() + lenPos + 7, nullptr, 10);
    if (len <= 0) return {};
    size_t streamPos = pdfBytes.find("stream", lenPos);
    if (streamPos == std::string::npos) return {};
    streamPos += 6;  // past "stream"
    if (streamPos < pdfBytes.size() && pdfBytes[streamPos] == '\r') ++streamPos;
    if (streamPos < pdfBytes.size() && pdfBytes[streamPos] == '\n') ++streamPos;
    if (streamPos + static_cast<size_t>(len) > pdfBytes.size()) return {};
    return pdfBytes.substr(streamPos, static_cast<size_t>(len));
}

// Device-space centres of the four authored quadrants. Device space is y-down
// from the top of the page and the fixture's MediaBox origin is (0,0), so
// devY = pageHeight - userY.
double quadCentreX(bool right) {
    return kImgX + kImgDrawW * (right ? 0.75 : 0.25);
}
double quadCentreDevY(bool bottom) {
    const double userY = kImgY + kImgDrawH * (bottom ? 0.25 : 0.75);
    return kPageH - userY;
}

std::string pageText(const std::filesystem::path& pdf) {
    std::string s;
    for (const auto& g : pdfe::testsupport::readGlyphs(pdf, 0))
        if (g.c < 128) s.push_back(static_cast<char>(g.c));
    return s;
}

class Jbig2RedactionTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(std::filesystem::exists(srcFixture()))
            << "fixture missing (regenerate with: python "
               "tests/fixtures/gen_jbig2_redaction.py): "
            << srcFixture();
        work_ = uniqueWorkCopy(srcFixture(), "jbig2");
        outDir_ = uniqueRunDir("run");
        ctx_ = std::make_shared<MuPdfContext>();

        // SEC-56 (#56): JBIG2 decoding now ships DISABLED — the /JBIG2Decode
        // filter is replaced by a blank placeholder stream unless the user opts
        // in. THIS suite is about the decode + redaction pipeline over an
        // AUTHORED, trusted fixture, so it opts in explicitly, exactly as a
        // user would. Without this the raster oracles below would read a blank
        // image and the quadrant assertions would fail for a reason that has
        // nothing to do with redaction.
        jbig2AtEntry_ = pdfe::core::isJbig2DecodingEnabled();
        pdfe::core::setJbig2DecodingEnabled(true);
    }

    void TearDown() override {
        pdfe::core::setJbig2DecodingEnabled(jbig2AtEntry_);
    }

    std::shared_ptr<MuPdfContext> ctx_;
    std::filesystem::path work_;
    std::filesystem::path outDir_;
    bool jbig2AtEntry_ = false;
};

}  // namespace

// The JBIG2 image decodes and reproduces the AUTHORED bitmap at the AUTHORED
// page coordinates.
//
// Kills: a broken/absent JBIG2Decode filter (nothing is drawn, so the authored
// black quadrants read white); an inverted 1-bit polarity (every quadrant
// assertion flips); a region-segment x/y offset that is not honoured (the
// quadrants land in the wrong places, so TL and TR swap).
TEST_F(Jbig2RedactionTest, DecodesAuthoredBitmap) {
    std::string err;
    const Raster r = renderPageRaster(work_, 0, 1.0f, &err);
    ASSERT_TRUE(err.empty()) << "page 0 failed to render: " << err;
    ASSERT_TRUE(r.valid());
    EXPECT_EQ(r.w, kPageW);
    EXPECT_EQ(r.h, kPageH);

    const double tl = darkFractionInBlock(r, quadCentreX(false),
                                          quadCentreDevY(false), kQuadHalf);
    const double tr = darkFractionInBlock(r, quadCentreX(true),
                                          quadCentreDevY(false), kQuadHalf);
    const double br = darkFractionInBlock(r, quadCentreX(true),
                                          quadCentreDevY(true), kQuadHalf);

    EXPECT_GT(tl, 0.95) << "authored TOP-LEFT quadrant is solid BLACK but only "
                        << tl * 100.0 << "% of its pixels are dark — the JBIG2 "
                                         "image did not decode as authored";
    EXPECT_GT(br, 0.95) << "authored BOTTOM-RIGHT quadrant is solid BLACK but "
                           "only "
                        << br * 100.0 << "% of its pixels are dark";
    EXPECT_LT(tr, 0.05) << "authored TOP-RIGHT quadrant is WHITE but "
                        << tr * 100.0
                        << "% of its pixels are dark — polarity inverted?";
}

// Applying a redaction over the TOP-LEFT quadrant must make the authored
// pattern stop rendering.
//
// blackBoxes is turned OFF deliberately: the authored pattern is BLACK, so a
// black cover box would make "pattern gone" and "pattern hidden behind a box"
// pixel-identical and the oracle would be vacuous. With no box, surviving ink
// can only be the original bitmap.
//
// The BOTTOM-RIGHT quadrant is the sharper assertion: it lies entirely OUTSIDE
// the redaction rectangle, so it can only go white if the image XObject was
// genuinely removed (RedactionImageMethod::Remove, the secure default) rather
// than merely occluded.
//
// Kills: a redaction that does not touch image XObjects at all; one that paints
// a cover box while leaving the image drawn; one that clips the image instead
// of removing it.
TEST_F(Jbig2RedactionTest, RedactionRemovesAuthoredPattern) {
    auto doc = Document::open(ctx_, work_);
    ASSERT_NE(doc, nullptr);
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    RedactionManager mgr(ctx_);
    // Top-left quadrant, in MuPDF page space (y-down), matching the convention
    // used by tests/test_redaction_partial_overlap.cpp.
    mgr.createRedactionMark(page.get(), QRectF(kImgX, kPageH - kImgY - kImgDrawH,
                                               kImgDrawW / 2.0, kImgDrawH / 2.0));

    RedactionApplyOptions opts;
    opts.blackBoxes = false;
    opts.imageMethod = RedactionImageMethod::Remove;
    const auto result = mgr.applyAllRedactions(doc.get(), opts);
    ASSERT_TRUE(result.success) << "applyAllRedactions failed: "
                                << result.errorMessage;
    EXPECT_GE(result.marksApplied, 1);

    const auto out = outDir_ / "jbig2_redacted.pdf";
    doc->saveAs(out);
    doc.reset();

    std::string err;
    const Raster r = renderPageRaster(out, 0, 1.0f, &err);
    ASSERT_TRUE(err.empty()) << "redacted page failed to render: " << err;
    ASSERT_TRUE(r.valid());

    const double tl = darkFractionInBlock(r, quadCentreX(false),
                                          quadCentreDevY(false), kQuadHalf);
    const double br = darkFractionInBlock(r, quadCentreX(true),
                                          quadCentreDevY(true), kQuadHalf);

    EXPECT_LT(tl, 0.05)
        << "the authored BLACK patch under the redaction still renders ("
        << tl * 100.0 << "% dark) — the JBIG2 image survived redaction";
    EXPECT_LT(br, 0.05)
        << "the authored BLACK patch OUTSIDE the redaction rectangle still "
           "renders ("
        << br * 100.0
        << "% dark) — RedactionImageMethod::Remove did not remove the "
           "overlapping JBIG2 image XObject";
}

// Confidentiality: after redaction, the ORIGINAL coded JBIG2 bytes must not be
// recoverable from the saved file.
//
// This is the assertion that a "remove the drawing operator only" redaction
// fails: the page stops showing the scan, every pixel oracle above passes, and
// the confidential bitmap is still sitting in the file as an orphan object.
//
// Kills: rewriteDocument = false (the pre-redaction revision, and with it the
// coded stream, stays in the file); dropping the image-XObject garbage
// collection; any change that keeps the redacted image alive from another
// reference (e.g. a resource dictionary that is never pruned).
TEST_F(Jbig2RedactionTest, OriginalJbig2BytesAreNotRecoverable) {
    const std::string originalBytes = readAllBytes(work_);
    const std::string jbig2 = extractJbig2Stream(originalBytes);
    ASSERT_GT(jbig2.size(), kMmrDataOffset + kNeedleLen)
        << "could not extract the authored JBIG2 stream from the fixture";

    // A run taken from the MMR-CODED payload, not from the fixed segment
    // headers (those are low-entropy and could plausibly recur).
    const std::string needle = jbig2.substr(kMmrDataOffset, kNeedleLen);

    // Positive control: the needle really is in the un-redacted file, so a
    // later "absent" result means removal and not a broken search.
    ASSERT_NE(originalBytes.find(needle), std::string::npos)
        << "the coded-byte needle is not present in the fixture itself — the "
           "byte scan would be vacuous";

    auto doc = Document::open(ctx_, work_);
    ASSERT_NE(doc, nullptr);
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    RedactionManager mgr(ctx_);
    mgr.createRedactionMark(page.get(), QRectF(kImgX, kPageH - kImgY - kImgDrawH,
                                               kImgDrawW / 2.0, kImgDrawH / 2.0));

    RedactionApplyOptions opts;
    opts.imageMethod = RedactionImageMethod::Remove;
    const auto result = mgr.applyAllRedactions(doc.get(), opts);
    ASSERT_TRUE(result.success) << "applyAllRedactions failed: "
                                << result.errorMessage;

    const auto out = outDir_ / "jbig2_scrubbed.pdf";
    doc->saveAs(out);
    doc.reset();

    const std::string savedBytes = readAllBytes(out);
    ASSERT_FALSE(savedBytes.empty()) << "saved file is empty";

    EXPECT_EQ(savedBytes.find(needle), std::string::npos)
        << "a " << kNeedleLen
        << "-byte verbatim run of the ORIGINAL JBIG2-coded bitmap is still "
           "present in the redacted file — the confidential scan is recoverable";

    // The filter name surviving is not by itself proof of a leak, but combined
    // with the run above it localises the failure, so report it too.
    EXPECT_EQ(savedBytes.find("/JBIG2Decode"), std::string::npos)
        << "the redacted file still declares a /JBIG2Decode stream although the "
           "only JBIG2 image on the page was redacted away";
}

// The redaction must not be over-broad: content outside the redaction
// rectangle has to survive.
//
// Kills: a redaction that clears the whole page content stream, or that drops
// every resource rather than only the ones it removed. Without this the three
// assertions above could all be satisfied by simply deleting everything.
TEST_F(Jbig2RedactionTest, UnredactedContentSurvives) {
    ASSERT_NE(pageText(work_).find("KEEPVISIBLE"), std::string::npos)
        << "control text missing from the fixture itself";

    auto doc = Document::open(ctx_, work_);
    ASSERT_NE(doc, nullptr);
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    RedactionManager mgr(ctx_);
    mgr.createRedactionMark(page.get(), QRectF(kImgX, kPageH - kImgY - kImgDrawH,
                                               kImgDrawW / 2.0, kImgDrawH / 2.0));

    RedactionApplyOptions opts;
    opts.blackBoxes = false;
    opts.imageMethod = RedactionImageMethod::Remove;
    ASSERT_TRUE(mgr.applyAllRedactions(doc.get(), opts).success);

    const auto out = outDir_ / "jbig2_control.pdf";
    doc->saveAs(out);
    doc.reset();

    EXPECT_NE(pageText(out).find("KEEPVISIBLE"), std::string::npos)
        << "text well outside the redaction rectangle was destroyed — the "
           "redaction is over-broad";
}
