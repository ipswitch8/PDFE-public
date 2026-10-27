// -*- coding: utf-8 -*-
//
// Hybrid-reference cross-reference (`/XRefStm`) on the INCREMENTAL-SAVE path —
// meta-audit A1-87 (docs/audit/input-domain-audit.md:104,149), previously
// "zero fixtures": grep for XRefStm/hybrid across tests/ hit only an unrelated
// QualityAssessor case.
//
// Fixture: tests/fixtures/hybrid_xref.pdf (tests/fixtures/gen_hybrid_xref.py)
//
// A hybrid-reference file (ISO 32000-1 §7.5.8.4) carries a classic `xref`
// TABLE *and* a cross-reference STREAM referenced by the trailer's /XRefStm.
// The fixture is a REAL hybrid, not a decoration: its classic table is written
// as the two subsections `0 5` and `6 2`, deliberately skipping object 5.
// Object 5 is the page's second content stream and the only thing that draws
// the token "HYBRIDONLY", so that token is reachable ONLY through the
// /XRefStm. (MuPDF's pdf_read_new_xref_section fills an entry only when its
// type is still 0, so an object marked *free* in the classic table would be
// unrecoverable from the /XRefStm — hence "omitted", not "free".)
//
// Verified negative control while authoring the fixture: neutering the
// trailer's "/XRefStm" key (same byte length, so all offsets stay valid) makes
// MuPDF report "cannot find object in xref (5 0 R)" and extract only
// "TABLEBASE". So every "HYBRIDONLY" assertion below is genuinely killable and
// is NOT rescued by MuPDF's repair-by-scan fallback.
//
// Contract under test (src/core/Document.h): saveIncremental() APPENDS a new
// xref/updated objects to the file the document was opened from, preserving
// the original bytes as a prefix. Adding an annotation is a purely additive
// edit, so sessionRequiresFullRewrite() must stay false and the incremental
// path is the legitimate route for this input — the product does NOT escalate
// to a full rewrite here.

#include <gtest/gtest.h>

#include "annotations/AnnotationManager.h"
#include "annotations/AnnotationType.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"

#include <QRectF>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace {

// ── Authored ground truth (mirrors gen_hybrid_xref.py) ──────────────────────
constexpr const char* kTableToken  = "TABLEBASE";   // object 4, classic table
constexpr const char* kHybridToken = "HYBRIDONLY";  // object 5, /XRefStm ONLY
constexpr const char* kEditSentinel = "INCREMENTALEDIT7f2b";

std::filesystem::path fixturePath()
{
    return std::filesystem::path(TEST_FIXTURES_DIR) / "hybrid_xref.pdf";
}

inline long pdfeProcessId()
{
#ifdef _WIN32
    return static_cast<long>(_getpid());
#else
    return static_cast<long>(getpid());
#endif
}

// Unique per-call working directory. saveIncremental() writes back to the file
// the document was opened from, so every test MUST work on its own copy — a
// shared path would mutate the committed fixture and would still be locked on
// the next --gtest_repeat iteration.
std::filesystem::path uniqueDir(const char* stem)
{
    static std::atomic<unsigned> counter{0};
    const unsigned n = counter.fetch_add(1, std::memory_order_relaxed);
    auto d = std::filesystem::temp_directory_path() / "pdfe_hybrid_xref"
             / (std::string(stem) + "_" + std::to_string(pdfeProcessId())
                + "_" + std::to_string(n));
    std::filesystem::create_directories(d);
    return d;
}

std::string readAllBytes(const std::filesystem::path& p)
{
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
}

class HybridXrefTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        ASSERT_TRUE(std::filesystem::exists(fixturePath()))
            << "hybrid_xref.pdf is a COMMITTED fixture — regenerate with "
               "python tests/fixtures/gen_hybrid_xref.py";
        context_ = std::make_shared<pdfe::core::MuPdfContext>();
        dir_ = uniqueDir("work");
        work_ = dir_ / "hybrid_xref_work.pdf";
        std::filesystem::copy_file(
            fixturePath(), work_,
            std::filesystem::copy_options::overwrite_existing);
        originalBytes_ = readAllBytes(work_);
        ASSERT_FALSE(originalBytes_.empty());
    }

    void TearDown() override
    {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }

    std::shared_ptr<pdfe::core::MuPdfContext> context_;
    std::filesystem::path dir_;
    std::filesystem::path work_;
    std::string originalBytes_;
};

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// 0. Fixture self-check. If the generator ever stopped producing a real hybrid
// file, every assertion below would pass vacuously off the classic table alone.
// This pins the STRUCTURE in the committed bytes.
// ─────────────────────────────────────────────────────────────────────────────
TEST_F(HybridXrefTest, FixtureIsAGenuineHybridReferenceFile)
{
    const std::string& b = originalBytes_;

    EXPECT_NE(b.find("/XRefStm "), std::string::npos)
        << "trailer must carry /XRefStm";
    EXPECT_NE(b.find("/Type /XRef"), std::string::npos)
        << "the /XRefStm target must be a cross-reference STREAM";
    EXPECT_NE(b.find("/W [1 4 2]"), std::string::npos)
        << "cross-reference stream must declare its field widths";
    EXPECT_NE(b.find("/Index [5 1]"), std::string::npos)
        << "the cross-reference stream must be the ONLY place object 5 is "
           "described";
    EXPECT_NE(b.find("\nxref\n0 5\n"), std::string::npos)
        << "classic table subsection 1 must be '0 5' (objects 0..4)";
    EXPECT_NE(b.find("\n6 2\n"), std::string::npos)
        << "classic table subsection 2 must be '6 2' (objects 6..7) — the gap "
           "at object 5 is what makes this file hybrid-ONLY resolvable";
}

// ─────────────────────────────────────────────────────────────────────────────
// 1. Opening the file resolves the hybrid-only object.
// Kills: any mutation that stops honouring the trailer's /XRefStm (verified:
// the token disappears and MuPDF does not repair its way back to it).
// ─────────────────────────────────────────────────────────────────────────────
TEST_F(HybridXrefTest, HybridOnlyObjectResolvesOnOpen)
{
    auto doc = pdfe::core::Document::open(context_, work_);
    ASSERT_NE(doc, nullptr);
    ASSERT_EQ(doc->pageCount(), 1);

    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);
    const std::string text = page->extractPlainText();

    EXPECT_NE(text.find(kTableToken), std::string::npos)
        << "object 4 is in the classic table and must always resolve";
    EXPECT_NE(text.find(kHybridToken), std::string::npos)
        << "object 5 is described ONLY by the trailer's /XRefStm cross-"
           "reference stream. Missing '" << kHybridToken << "' means the "
           "hybrid-reference path was not used. Extracted: " << text;
}

// ─────────────────────────────────────────────────────────────────────────────
// 2. An annotation add on a hybrid file is a purely ADDITIVE edit, so the
// session must stay eligible for the incremental-append path.
// Kills: a mutation that marks annotation creation (or hybrid-xref input)
// destructive/structural, which would silently reroute File>Save to a full
// rewrite and lose the byte-prefix guarantee this fixture exists to protect.
// ─────────────────────────────────────────────────────────────────────────────
TEST_F(HybridXrefTest, AnnotationAddKeepsTheSessionOnTheIncrementalPath)
{
    auto doc = pdfe::core::Document::open(context_, work_);
    ASSERT_NE(doc, nullptr);
    ASSERT_FALSE(doc->sessionRequiresFullRewrite())
        << "a freshly opened hybrid-reference document has performed no "
           "destructive operation";

    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    pdfe::annotations::AnnotationManager mgr(context_);
    const auto data = mgr.createAnnotation(
        page.get(), pdfe::annotations::AnnotationType::Square,
        QRectF(100.0, 100.0, 100.0, 50.0));
    ASSERT_NE(data.annotIndex, -1);

    EXPECT_FALSE(doc->sessionRequiresFullRewrite())
        << "adding an annotation is additive (Document.h: 'Pure additive edits "
           "— form-field fills, annotation add/modify — leave this false'), so "
           "the hybrid file must stay on the incremental-save route";
}

// ─────────────────────────────────────────────────────────────────────────────
// 3. saveIncremental() really APPENDS: the file grows and the original bytes
// survive byte-for-byte as a prefix.
// Kills: a mutation that silently converts the incremental save into a full
// rewrite (do_incremental = 0, or routing through saveAs) — the rewritten file
// would not start with the original bytes, and a signed /ByteRange over the
// original revision would be destroyed.
// ─────────────────────────────────────────────────────────────────────────────
TEST_F(HybridXrefTest, IncrementalSaveAppendsAndPreservesTheOriginalPrefix)
{
    {
        auto doc = pdfe::core::Document::open(context_, work_);
        ASSERT_NE(doc, nullptr);
        auto page = doc->page(0);
        ASSERT_NE(page, nullptr);

        pdfe::annotations::AnnotationManager mgr(context_);
        const auto data = mgr.createAnnotation(
            page.get(), pdfe::annotations::AnnotationType::Square,
            QRectF(100.0, 100.0, 100.0, 50.0));
        ASSERT_NE(data.annotIndex, -1);
        mgr.setContents(page.get(), data.annotIndex, kEditSentinel);

        page.reset();
        doc->saveIncremental();
    }

    const std::string after = readAllBytes(work_);
    ASSERT_FALSE(after.empty());

    EXPECT_GT(after.size(), originalBytes_.size())
        << "an incremental save must APPEND a revision, so the file can only "
           "grow";
    ASSERT_GE(after.size(), originalBytes_.size());
    EXPECT_EQ(after.compare(0, originalBytes_.size(), originalBytes_), 0)
        << "the first " << originalBytes_.size() << " bytes must be the "
           "ORIGINAL revision, unchanged — a full rewrite would have replaced "
           "them (and with them the classic table, the /XRefStm and any signed "
           "byte range)";

    // The original revision's own cross-reference structures are inside that
    // preserved prefix, so both must still be physically present.
    EXPECT_NE(after.find("/XRefStm "), std::string::npos);
    EXPECT_NE(after.find("\nxref\n0 5\n"), std::string::npos);
}

// ─────────────────────────────────────────────────────────────────────────────
// 4. The appended revision reopens with the edit PRESENT and every original
// object — including the hybrid-only one — INTACT.
// Kills: an incremental writer that appends a new xref which shadows or drops
// the objects the previous revision reached through /XRefStm (object 5 would
// stop resolving and "HYBRIDONLY" would vanish), and one that writes an edit
// the reopened document cannot see.
// ─────────────────────────────────────────────────────────────────────────────
TEST_F(HybridXrefTest, ReopenedIncrementalSaveKeepsBothTheEditAndTheOriginals)
{
    {
        auto doc = pdfe::core::Document::open(context_, work_);
        ASSERT_NE(doc, nullptr);
        auto page = doc->page(0);
        ASSERT_NE(page, nullptr);

        // Pre-condition: both authored tokens are present BEFORE the edit, so
        // the survival assertions below cannot pass vacuously.
        const std::string before = page->extractPlainText();
        ASSERT_NE(before.find(kTableToken), std::string::npos);
        ASSERT_NE(before.find(kHybridToken), std::string::npos);

        pdfe::annotations::AnnotationManager mgr(context_);
        const auto data = mgr.createAnnotation(
            page.get(), pdfe::annotations::AnnotationType::Square,
            QRectF(100.0, 100.0, 100.0, 50.0));
        ASSERT_NE(data.annotIndex, -1);
        mgr.setContents(page.get(), data.annotIndex, kEditSentinel);

        page.reset();
        doc->saveIncremental();
    }

    auto reopened = pdfe::core::Document::open(context_, work_);
    ASSERT_NE(reopened, nullptr);
    ASSERT_EQ(reopened->pageCount(), 1);
    auto page = reopened->page(0);
    ASSERT_NE(page, nullptr);

    // (a) the original objects survive — including the hybrid-only one, which
    // proves the appended revision did not break /XRefStm resolution.
    const std::string text = page->extractPlainText();
    EXPECT_NE(text.find(kTableToken), std::string::npos)
        << "classic-table object 4 must survive the incremental save";
    EXPECT_NE(text.find(kHybridToken), std::string::npos)
        << "the /XRefStm-only object 5 must still resolve after the "
           "incremental append. Extracted: " << text;

    // (b) the edit is present.
    pdfe::annotations::AnnotationManager mgr(context_);
    const auto annots = mgr.getAnnotations(page.get());
    ASSERT_EQ(annots.size(), 1u)
        << "exactly the one appended annotation must be found";
    EXPECT_EQ(annots[0].type, pdfe::annotations::AnnotationType::Square);
    EXPECT_EQ(annots[0].contents, std::string(kEditSentinel))
        << "the incrementally appended annotation must reopen with its "
           "authored /Contents";

    page.reset();
    reopened.reset();
}
