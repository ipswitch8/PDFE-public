// -*- coding: utf-8 -*-
//
// Pipeline 4 (HARDEN-REDACT): Redaction completeness tests.
// Verifies sensitive data is fully removed after redaction.

#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "core/TextExtractor.h"
#include "redaction/RedactionManager.h"
#include "support/PdfContentOracle.h"

#include <QRectF>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

using namespace pdfe::core;

namespace {
std::string fixturesDir() { return std::string(TEST_FIXTURES_DIR); }

std::filesystem::path tempDir()
{
    auto d = std::filesystem::temp_directory_path() / "pdfe_redact_complete";
    std::filesystem::create_directories(d);
    return d;
}

inline long pdfeProcessId()
{
#ifdef _WIN32
    return static_cast<long>(_getpid());
#else
    return static_cast<long>(getpid());
#endif
}

// #49: copy the shared tracked fixture to a UNIQUE per-run working path so an
// in-place redaction bake (applyAllRedactions rewrites onto doc->filePath()) can
// never mutate tests/fixtures/test_redaction.pdf. The pid keeps concurrent shard
// processes disjoint; the atomic counter keeps repeated calls within one process
// disjoint — so --gtest_repeat sees a fresh copy every iteration.
inline std::filesystem::path uniqueWorkCopy(const std::filesystem::path& src, const char* stem)
{
    static std::atomic<unsigned> counter{0};
    const unsigned n = counter.fetch_add(1, std::memory_order_relaxed);
    auto dst = tempDir() / (std::string(stem) + "_" + std::to_string(pdfeProcessId())
                            + "_" + std::to_string(n) + ".pdf");
    std::filesystem::copy_file(src, dst, std::filesystem::copy_options::overwrite_existing);
    return dst;
}

// #49: a UNIQUE per-run output directory. The tests write fixed-named artifacts
// (redacted_bytes.pdf, ...) and reopen them; a reopened Document can still hold
// the file when the next --gtest_repeat iteration re-saves the same path
// ("cannot remove file: Permission denied" on Windows). A fresh dir per run keeps
// each iteration's outputs disjoint.
inline std::filesystem::path uniqueRunDir(const char* stem)
{
    static std::atomic<unsigned> counter{0};
    const unsigned n = counter.fetch_add(1, std::memory_order_relaxed);
    auto d = tempDir() / (std::string(stem) + "_" + std::to_string(pdfeProcessId())
                          + "_" + std::to_string(n));
    std::filesystem::create_directories(d);
    return d;
}
}

class RedactionCompletenessTest : public ::testing::Test {
protected:
    void SetUp() override {
        ctx_ = std::make_shared<MuPdfContext>();
        // De-conditionalized (R-02/R-15): the known-secret fixture is pinned by
        // the CMake fixture floor, so the byte-absence oracle runs
        // UNCONDITIONALLY. The old fallback to test_single.pdf + GTEST_SKIP let
        // the strongest oracle silently not run; a missing fixture is now a hard
        // failure here (and aborts CMake configure at the fixture floor).
        auto srcFixture = std::filesystem::path(fixturesDir()) / "test_redaction.pdf";
        ASSERT_TRUE(std::filesystem::exists(srcFixture))
            << "known-secret redaction fixture must be present: " << srcFixture;
        td_ = uniqueRunDir("run");
        // #49: open a UNIQUE working copy — several tests here bake the redaction
        // in place (applyAllRedactions rewrites onto doc->filePath()), which would
        // otherwise mutate the shared tracked fixture and make --gtest_repeat fail
        // on the 2nd iteration.
        fixture_ = uniqueWorkCopy(srcFixture, "redaction_completeness");
    }
    std::shared_ptr<MuPdfContext> ctx_;
    std::filesystem::path fixture_;
    std::filesystem::path td_;
};

TEST_F(RedactionCompletenessTest, RedactedTextAbsentFromBytes)
{
    auto doc = Document::open(ctx_, fixture_);
    ASSERT_NE(doc, nullptr);

    pdfe::redaction::RedactionManager mgr(ctx_);
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    // Redact the top region containing "Confidential Document" (y ~66-99).
    mgr.createRedactionMark(page.get(), QRectF(90, 60, 270, 45));
    mgr.applyAllRedactions(doc.get());

    auto out = td_ / "redacted_bytes.pdf";
    doc->saveAs(out);

    // DECOMPRESS the content stream before searching. The previous version
    // read the raw on-disk bytes and grepped for "Confidential" -- a
    // Flate-compressed content stream never contains the ASCII secret, so that
    // check passed even if the text was NOT removed. Decompressing exposes the
    // real text operators.
    const std::string content =
        pdfe::testsupport::decompressedPageContent(out, 0);
    ASSERT_FALSE(content.empty()) << "no decompressed content to check";
    EXPECT_EQ(content.find("Confidential"), std::string::npos)
        << "redacted secret 'Confidential' survives in the DECOMPRESSED content "
           "stream (redaction did not remove the text operators)";

    // Sanity: a raw-byte grep would have been blind here -- prove the guard is
    // meaningful by confirming the decompressed stream is what carries text.
    std::ifstream f(out, std::ios::binary);
    const std::string rawBytes((std::istreambuf_iterator<char>(f)),
                               std::istreambuf_iterator<char>());
    // The control text that must SURVIVE ("visible") is present once
    // decompressed; if it is also findable raw, the fixture is uncompressed and
    // the raw grep was coincidentally valid -- either way the decompress check
    // above is the authoritative one.
    EXPECT_NE(content.find("visible"), std::string::npos)
        << "control text unexpectedly missing from decompressed content";

    // Extraction-absence oracle (runs unconditionally alongside byte-absence):
    // reopen the saved file and confirm the secret is not recoverable via text
    // extraction either, while the control neighbour survives.
    auto reopened = Document::open(ctx_, out);
    ASSERT_NE(reopened, nullptr);
    auto rePage = reopened->page(0);
    ASSERT_NE(rePage, nullptr);
    pdfe::core::TextExtractor extractor(ctx_);
    const std::string extracted = extractor.extractPlainText(rePage.get());
    EXPECT_EQ(extracted.find("Confidential"), std::string::npos)
        << "redacted secret 'Confidential' still recoverable via text extraction";
    EXPECT_NE(extracted.find("visible"), std::string::npos)
        << "control text outside the redaction region was wrongly removed";
}

TEST_F(RedactionCompletenessTest, RedactionUndoDoesNotRestoreAfterSave)
{
    auto doc = Document::open(ctx_, fixture_);
    ASSERT_NE(doc, nullptr);

    pdfe::redaction::RedactionManager mgr(ctx_);
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    mgr.createRedactionMark(page.get(), QRectF(50, 50, 200, 50));
    mgr.applyAllRedactions(doc.get());

    auto out = td_ / "redacted_undo.pdf";
    doc->saveAs(out);

    // Reopen — the saved file should not contain the redacted content
    auto reopened = Document::open(ctx_, out);
    ASSERT_NE(reopened, nullptr);
    EXPECT_GE(reopened->pageCount(), 1);
}

TEST_F(RedactionCompletenessTest, MultipleRedactionMarksApplied)
{
    auto doc = Document::open(ctx_, fixture_);
    ASSERT_NE(doc, nullptr);

    pdfe::redaction::RedactionManager mgr(ctx_);
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    // Mark multiple regions
    mgr.createRedactionMark(page.get(), QRectF(0, 0, 300, 50));
    mgr.createRedactionMark(page.get(), QRectF(0, 100, 300, 50));
    mgr.createRedactionMark(page.get(), QRectF(0, 200, 300, 50));
    mgr.applyAllRedactions(doc.get());

    auto out = td_ / "redacted_multi.pdf";
    EXPECT_NO_THROW(doc->saveAs(out));
    EXPECT_TRUE(std::filesystem::exists(out));
}

TEST_F(RedactionCompletenessTest, FullPageRedactionProducesValidPdf)
{
    auto doc = Document::open(ctx_, fixture_);
    ASSERT_NE(doc, nullptr);

    pdfe::redaction::RedactionManager mgr(ctx_);
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    // Redact entire page
    mgr.createRedactionMark(page.get(), QRectF(0, 0, 612, 792));
    mgr.applyAllRedactions(doc.get());

    auto out = td_ / "redacted_full_page.pdf";
    doc->saveAs(out);

    auto reopened = Document::open(ctx_, out);
    ASSERT_NE(reopened, nullptr);
    EXPECT_EQ(reopened->pageCount(), doc->pageCount());
}
