// -*- coding: utf-8 -*-
//
// Incremental-update integrity — meta-audit follow-up F14.
//
// Gaps: PDF-03 (no test asserted that saveIncremental leaves ALL prior
// revisions byte-identical), PDF-81 (the revision chain was only ever two deep),
// PDF-09 / PDF-02 (object-stream documents were never joined to the incremental
// path).
//
// THE PROPERTY. An incremental update is defined by what it does NOT do: every
// byte of every prior revision stays exactly where it was, and the new revision
// is appended. If the prefix moves by one byte, every signature over that file
// is void — the /ByteRange offsets no longer point at the bytes that were
// signed. `tests/test_incremental_save.cpp` already compares a prefix for the
// single-revision case; this file makes the comparison exact (reporting the
// first differing offset), extends it to a deep revision chain, and covers the
// object-stream shape.

#include <gtest/gtest.h>

#include "annotations/AnnotationManager.h"
#include "annotations/AnnotationType.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"

#include <QTemporaryDir>

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

namespace fs = std::filesystem;
using namespace pdfe::core;

namespace {

std::string readAll(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

/// Index of the first differing byte, or npos when @p prefix is a true prefix
/// of @p whole. Explicit rather than a bare `compare()` so a failure reports
/// WHERE the file diverged.
size_t firstPrefixMismatch(const std::string& prefix, const std::string& whole) {
    if (whole.size() < prefix.size()) return 0;
    for (size_t i = 0; i < prefix.size(); ++i) {
        if (prefix[i] != whole[i]) return i;
    }
    return std::string::npos;
}

std::string hexAround(const std::string& s, size_t at, size_t span = 16) {
    std::ostringstream o;
    const size_t begin = at > span ? at - span : 0;
    const size_t end = std::min(s.size(), at + span);
    o << std::hex;
    for (size_t i = begin; i < end; ++i) {
        o << (i == at ? "[" : "") << (int)(unsigned char)s[i] << (i == at ? "]" : "") << " ";
    }
    return o.str();
}

class IncrementalUpdateIntegrityTest : public ::testing::Test {
protected:
    void SetUp() override {
        ctx_ = std::make_shared<MuPdfContext>();
        ASSERT_TRUE(tmp_.isValid());
    }
    void TearDown() override { ctx_.reset(); }

    /// Copy a fixture into this test's private temp dir and return the copy.
    fs::path workCopy(const char* fixture, const char* workName) {
        const fs::path dst = fs::path(tmp_.path().toStdString()) / workName;
        std::error_code ec;
        fs::copy_file(fs::path(TEST_FIXTURES_DIR) / fixture, dst,
                      fs::copy_options::overwrite_existing, ec);
        EXPECT_FALSE(ec) << ec.message();
        return dst;
    }

    /// One incremental-safe edit: add a text annotation, then append.
    void appendOneRevision(const fs::path& path, const std::string& marker) {
        auto doc = Document::open(ctx_, path);
        ASSERT_NE(doc, nullptr);
        auto page = doc->page(0);
        ASSERT_NE(page, nullptr);
        pdfe::annotations::AnnotationManager mgr(ctx_);
        auto a = mgr.createAnnotation(page.get(),
                                      pdfe::annotations::AnnotationType::Text,
                                      QRectF(72, 72, 20, 20));
        mgr.setContents(page.get(), a.annotIndex, marker);
        doc->saveIncremental();
    }

    std::shared_ptr<MuPdfContext> ctx_;
    QTemporaryDir tmp_;
};

} // namespace

// ===========================================================================
// Criterion 2 — the pre-save byte prefix is unchanged in the output
// ===========================================================================

TEST_F(IncrementalUpdateIntegrityTest, PriorRevisionBytesUnchanged) {
    const fs::path path = workCopy("test_single.pdf", "prefix_single.pdf");
    const std::string before = readAll(path);
    ASSERT_FALSE(before.empty());

    appendOneRevision(path, "F14 incremental marker");

    const std::string after = readAll(path);
    ASSERT_GT(after.size(), before.size())
        << "an incremental update must APPEND; the file did not grow, so the "
           "prior revision was rewritten rather than preserved";

    // The defining byte comparison.
    const size_t mismatch = firstPrefixMismatch(before, after);
    EXPECT_EQ(mismatch, std::string::npos)
        << "incremental save MUTATED the prior revision at byte offset "
        << mismatch << " of " << before.size()
        << ". Every signature whose /ByteRange covers that offset is now void.\n"
        << "  before: " << hexAround(before, mismatch) << "\n"
        << "  after : " << hexAround(after, mismatch);
}

TEST_F(IncrementalUpdateIntegrityTest, IncrementalSaveDoesNotRewriteTheHeader) {
    const fs::path path = workCopy("version_header_2_0.pdf", "prefix_v20.pdf");
    const std::string before = readAll(path);

    appendOneRevision(path, "F14 header-immutability marker");

    const std::string after = readAll(path);
    ASSERT_GT(after.size(), before.size());
    EXPECT_EQ(firstPrefixMismatch(before, after), std::string::npos);

    // Ties criterion 2 to criterion 1: the header is part of the prefix, so an
    // incremental update can neither move it nor change the version it states.
    ASSERT_GE(after.size(), 9u);
    EXPECT_EQ(after.substr(0, 8), std::string("%PDF-2.0"))
        << "the %PDF header is byte 0 of the preserved prefix";

    auto reopened = Document::open(ctx_, path);
    ASSERT_NE(reopened, nullptr);
    EXPECT_EQ(reopened->pdfVersion().toString(), "2.0")
        << "an incremental update downgraded the effective version";
}

// PDF-81: the corpus generator only ever appended two revisions. Real signed /
// reviewed documents carry ten or more, and each must remain byte-intact.
TEST_F(IncrementalUpdateIntegrityTest, DeepRevisionChainKeepsEveryPriorPrefix) {
    const fs::path path = workCopy("test_single.pdf", "prefix_deep.pdf");

    constexpr int kRevisions = 12;
    std::string previous = readAll(path);
    ASSERT_FALSE(previous.empty());

    for (int rev = 1; rev <= kRevisions; ++rev) {
        SCOPED_TRACE("revision " + std::to_string(rev));
        appendOneRevision(path, "F14 revision " + std::to_string(rev));

        const std::string current = readAll(path);
        ASSERT_GT(current.size(), previous.size())
            << "revision " << rev << " did not append";
        const size_t mismatch = firstPrefixMismatch(previous, current);
        ASSERT_EQ(mismatch, std::string::npos)
            << "revision " << rev << " mutated the prior revision at offset "
            << mismatch;
        previous = current;
    }

    // The whole chain is still a loadable document with all 12 markers.
    auto doc = Document::open(ctx_, path);
    ASSERT_NE(doc, nullptr);
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);
    pdfe::annotations::AnnotationManager mgr(ctx_);
    const auto annots = mgr.getAnnotations(page.get());
    int found = 0;
    for (int rev = 1; rev <= kRevisions; ++rev) {
        const std::string marker = "F14 revision " + std::to_string(rev);
        for (const auto& a : annots) {
            if (a.contents.find(marker) != std::string::npos) { ++found; break; }
        }
    }
    EXPECT_EQ(found, kRevisions)
        << "only " << found << " of " << kRevisions
        << " revisions survived the chain";
}

// PDF-09 / PDF-02: an object-stream + xref-stream document, appended to
// incrementally. The interaction (a classic appended xref over an xref-stream
// base) is where a writer most easily corrupts the prefix.
TEST_F(IncrementalUpdateIntegrityTest, ObjStmDocumentSurvivesIncrementalUpdate) {
    const fs::path src = fs::path(TEST_FIXTURES_DIR) / "compat_objstm_xref.pdf";
    ASSERT_TRUE(fs::exists(src)) << src;
    const fs::path path = workCopy("compat_objstm_xref.pdf", "prefix_objstm.pdf");

    const std::string before = readAll(path);
    ASSERT_NE(before.find("/ObjStm"), std::string::npos)
        << "fixture is no longer an object-stream document";

    appendOneRevision(path, "F14 objstm incremental marker");

    const std::string after = readAll(path);
    ASSERT_GT(after.size(), before.size());
    const size_t mismatch = firstPrefixMismatch(before, after);
    EXPECT_EQ(mismatch, std::string::npos)
        << "incremental update over an object-stream document mutated the "
           "prior revision at offset " << mismatch;

    // …and the result still loads, with the appended annotation visible.
    auto doc = Document::open(ctx_, path);
    ASSERT_NE(doc, nullptr);
    ASSERT_GE(doc->pageCount(), 1);
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);
    pdfe::annotations::AnnotationManager mgr(ctx_);
    bool found = false;
    for (const auto& a : mgr.getAnnotations(page.get())) {
        if (a.contents.find("F14 objstm incremental marker") != std::string::npos) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found)
        << "the appended revision is not readable over the object-stream base";
}
