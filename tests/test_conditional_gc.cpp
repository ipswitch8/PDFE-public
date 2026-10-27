// -*- coding: utf-8 -*-
//
// Pipeline b2e47d91 — Tier 2: edit-aware conditional garbage collection.
//
// Full-rewrite saves now default to the cheap GC level (do_garbage=1, which
// still removes unreferenced/orphaned objects) and escalate to do_garbage=3
// (de-duplicate) only when the session performed a *dedup-worthy* op:
// redaction apply, page delete/reorder/import, form flatten, font embedding,
// or encryption change. Document::effectiveGarbageLevel() is the inspectable
// seam (1 vs 3) and Document::needsFullGc() the predicate.
//
// These tests lock:
//   * a clean session uses the lower default (AC test a, via the seam);
//   * redaction apply → needsFullGc()==true (AC test b);
//   * form-field fill → needsFullGc()==false (AC test c);
//   * page delete → needsFullGc()==true;
//   * page rotate is requires-full-rewrite (Tier 1) but NOT needsFullGc
//     (Tier 2) — gc stays at the cheap default — the subtle distinction;
//   * (slow, gated) saveAs of a large XFA form with no structural op is fast.

#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "forms/FormManager.h"
#include "forms/FormFieldData.h"
#include "forms/FormFieldType.h"
#include "redaction/RedactionManager.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

using namespace pdfe::core;

namespace {

std::filesystem::path fixturesDir()
{
    return std::filesystem::path(TEST_FIXTURES_DIR);
}

std::filesystem::path workCopy(const std::filesystem::path& fixtureRel,
                               const char* workName)
{
    auto d = std::filesystem::temp_directory_path() / "pdfe_conditional_gc";
    std::filesystem::create_directories(d);
    auto p = d / workName;
    std::filesystem::copy_file(fixturesDir() / fixtureRel, p,
        std::filesystem::copy_options::overwrite_existing);
    return p;
}

int firstEditableTextField(pdfe::forms::FormManager& fm, pdfe::core::Page* page)
{
    for (const auto& f : fm.getFormFields(page)) {
        if (f.type == pdfe::forms::FormFieldType::Text && !f.isReadOnly) {
            return f.widgetIndex;
        }
    }
    return -1;
}

bool slowSaveTestsEnabled()
{
    const char* v = std::getenv("PDFE_RUN_SLOW_SAVE_TESTS");
    return v && std::string(v) == "1";
}

// The Phase-1-validated safe default GC level for full-rewrite saves.
constexpr int kExpectedDefaultGc = 1;

} // namespace

class ConditionalGcTest : public ::testing::Test {
protected:
    void SetUp() override { ctx_ = std::make_shared<MuPdfContext>(); }
    std::shared_ptr<MuPdfContext> ctx_;
};

// ── AC test (a) — a clean session uses the lower default GC level ─────────
TEST_F(ConditionalGcTest, CleanSessionUsesDefaultGc)
{
    auto path = workCopy("test_3page.pdf", "gc_clean.pdf");
    auto doc = Document::open(ctx_, path);
    ASSERT_NE(doc, nullptr);

    EXPECT_FALSE(doc->needsFullGc())
        << "a freshly opened document has no dedup-worthy edit";
    EXPECT_EQ(doc->effectiveGarbageLevel(), kExpectedDefaultGc)
        << "a clean full-rewrite save must use the cheap default GC level, "
        << "not the expensive do_garbage=3 dedup pass";
}

// ── AC test (c) — a form-field fill does NOT need full GC ─────────────────
TEST_F(ConditionalGcTest, FormFillDoesNotNeedFullGc)
{
    auto path = workCopy("test_form.pdf", "gc_formfill.pdf");
    auto doc = Document::open(ctx_, path);
    ASSERT_NE(doc, nullptr);

    pdfe::forms::FormManager fm(ctx_);
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);
    const int idx = firstEditableTextField(fm, page.get());
    ASSERT_GE(idx, 0);
    fm.setFieldValue(page.get(), idx, "tier2_value");

    EXPECT_FALSE(doc->needsFullGc());
    EXPECT_EQ(doc->effectiveGarbageLevel(), kExpectedDefaultGc);
}

// ── AC test (b) — redaction apply needs full GC ──────────────────────────
TEST_F(ConditionalGcTest, RedactionNeedsFullGc)
{
    auto path = workCopy("test_redaction.pdf", "gc_redact.pdf");
    auto doc = Document::open(ctx_, path);
    ASSERT_NE(doc, nullptr);

    pdfe::redaction::RedactionManager rm(ctx_);
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);
    rm.createRedactionMark(page.get(), QRectF(100, 100, 120, 40));
    page.reset();

    pdfe::redaction::RedactionApplyOptions opts;
    opts.rewriteDocument = false;
    const auto result = rm.applyAllRedactions(doc.get(), opts);
    ASSERT_GT(result.marksApplied, 0);

    EXPECT_TRUE(doc->needsFullGc())
        << "redaction is dedup-worthy and must escalate to do_garbage=3";
    EXPECT_EQ(doc->effectiveGarbageLevel(), 3);
}

// ── page delete needs full GC ────────────────────────────────────────────
TEST_F(ConditionalGcTest, PageDeleteNeedsFullGc)
{
    auto path = workCopy("test_3page.pdf", "gc_delete.pdf");
    auto doc = Document::open(ctx_, path);
    ASSERT_NE(doc, nullptr);

    doc->deletePage(2);

    EXPECT_TRUE(doc->needsFullGc())
        << "page delete orphans page objects and must escalate to do_garbage=3";
    EXPECT_EQ(doc->effectiveGarbageLevel(), 3);
}

// ── page rotate is full-rewrite (Tier 1) but NOT full-GC (Tier 2) ────────
// This locks the distinction between the two flags: rotate changes /Rotate
// (so the save must full-rewrite, not incremental-append) but creates no
// orphans worth de-duplicating, so it stays at the cheap default GC level.
TEST_F(ConditionalGcTest, RotateIsFullRewriteButNotFullGc)
{
    auto path = workCopy("test_3page.pdf", "gc_rotate.pdf");
    auto doc = Document::open(ctx_, path);
    ASSERT_NE(doc, nullptr);

    doc->rotatePage(0, 90);

    EXPECT_TRUE(doc->sessionRequiresFullRewrite())
        << "rotate must still take the full-rewrite path (Tier 1)";
    EXPECT_FALSE(doc->needsFullGc())
        << "rotate creates no dedup-worthy orphans (Tier 2)";
    EXPECT_EQ(doc->effectiveGarbageLevel(), kExpectedDefaultGc);
}

// ── a PURE PERMUTATION is full-rewrite but NOT full-GC (GitHub #183) ─────
// The distinction PageDeleteNeedsFullGc draws is between operations that leave
// something behind and operations that do not. A reorder is on the far side of
// that line from a delete: it re-orders one /Kids array, creating no object and
// duplicating none, so do_garbage=3's all-pairs removeduplicateobjs pass
// (pdf-write.c:237) has nothing to find and the document pays an O(objects^2)
// scan for it. The full rewrite is still required -- the page tree changed --
// and its gc=1 sweep still collects the intermediate /Pages nodes that the
// flattening orphans. This is the non-timing half of #183's guard: the timing
// half is PerfStressTest.ReorderAndSaveScalingCurveIsNearLinear, and this is the
// seam that says WHY it is allowed to be fast.
//
// The delete case above is the control: the two tests run the same fixture
// through the same method and must disagree, so a change that flattened the
// distinction in either direction reddens one of them.
TEST_F(ConditionalGcTest, PureReorderIsFullRewriteButNotFullGc)
{
    auto path = workCopy("test_3page.pdf", "gc_reorder.pdf");
    auto doc = Document::open(ctx_, path);
    ASSERT_NE(doc, nullptr);
    ASSERT_EQ(doc->pageCount(), 3);
    ASSERT_FALSE(doc->needsFullGc())
        << "control: a freshly opened document must not already be escalated, "
           "or the assertions below prove nothing about the reorder";
    ASSERT_FALSE(doc->sessionRequiresFullRewrite());

    doc->reorderPages({2, 1, 0});
    ASSERT_EQ(doc->pageCount(), 3)
        << "control: the reorder must actually have happened";

    EXPECT_TRUE(doc->sessionRequiresFullRewrite())
        << "a page-tree change must still take the full-rewrite path (Tier 1)";
    EXPECT_FALSE(doc->needsFullGc())
        << "a permutation creates no duplicate objects, so it must not pay for "
           "the dedup pass (Tier 2 / GitHub #183)";
    EXPECT_EQ(doc->effectiveGarbageLevel(), kExpectedDefaultGc);
}

// ── (slow, gated) saveAs of a large XFA form with no structural op is fast ─
TEST_F(ConditionalGcTest, SaveAsFastOnXfaWithoutStructuralOp)
{
    if (!slowSaveTestsEnabled()) {
        GTEST_SKIP() << "set PDFE_RUN_SLOW_SAVE_TESTS=1 (Release) to run the "
                        "large-XFA saveAs timing test";
    }
    const auto fixture = std::filesystem::path("realworld") / "ssa_sf86.pdf";
    if (!std::filesystem::exists(fixturesDir() / fixture)) {
        GTEST_SKIP() << "ssa_sf86.pdf fixture not available";
    }
    auto src = workCopy(fixture, "gc_xfa_src.pdf");
    auto doc = Document::open(ctx_, src);
    ASSERT_NE(doc, nullptr);
    ASSERT_FALSE(doc->needsFullGc());
    ASSERT_EQ(doc->effectiveGarbageLevel(), kExpectedDefaultGc);

    auto out = std::filesystem::temp_directory_path() / "pdfe_conditional_gc"
               / "gc_xfa_out.pdf";
    const auto t0 = std::chrono::steady_clock::now();
    doc->saveAs(out);
    const auto t1 = std::chrono::steady_clock::now();
    const long long elapsedMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

    EXPECT_LT(elapsedMs, 60000)
        << "saveAs of ssa_sf86 with no structural op took " << elapsedMs
        << " ms — the cheap default GC should keep it far under the ~314 s "
        << "gc=3 baseline (>2x faster, the Tier 2 perf objective)";

    // Round-trip: the gc=1 output must reopen cleanly with its pages intact.
    auto reopened = Document::open(ctx_, out);
    ASSERT_NE(reopened, nullptr);
    EXPECT_GT(reopened->pageCount(), 0);
}

// ── PDF-103 (meta-audit gap register, issue #6) ──────────────────────────────
//
// PDF-02 (tests/test_format_compat.cpp, FormatCompatShape.SavedOutputKeepsObjStm)
// establishes that a DEFAULT full-rewrite save keeps the object streams the
// document arrived with. That save runs at the cheap GC level.
//
// This is the same invariant under the OTHER branch of effectiveGarbageLevel().
// do_garbage=3 is not "the same write, slower": it renumbers every object and
// then de-duplicates byte-identical ones, which is precisely the pass that
// rebuilds the xref from scratch and where a shape carried only by the original
// xref's entry types would be dropped. A user hits this branch by doing any
// dedup-worthy edit — redacting, deleting a page, flattening a form — so
// "your file doubles in size, but only if you edited it structurally" is a real
// and confusing outcome, not a hypothetical.
//
// The byte-level shape assertion is deliberately spelled out here rather than
// shared with test_format_compat.cpp: both files keep their helpers in an
// anonymous namespace (correctly — they are per-TU test scaffolding), and
// promoting a two-line byte search into a shared test header to avoid
// restating it would couple two suites for no benefit.
TEST_F(ConditionalGcTest, EscalatedGcOutputRetainsObjectStreams)
{
    auto path = workCopy("compat_objstm_xref.pdf", "gc_objstm_src.pdf");
    auto doc = Document::open(ctx_, path);
    ASSERT_NE(doc, nullptr);

    ASSERT_TRUE(doc->sourceUsesObjectStreams())
        << "precondition: compat_objstm_xref.pdf must arrive using object "
           "streams, otherwise there is no shape for the save to retain";

    // Escalate to the dedup level the way a real session does.
    doc->markNeedsFullGc();
    ASSERT_EQ(doc->effectiveGarbageLevel(), 3)
        << "guard: markNeedsFullGc() must select do_garbage=3 — without the "
           "escalation this test would silently re-run the PDF-02 scenario";

    auto out = std::filesystem::temp_directory_path() / "pdfe_conditional_gc"
               / "gc_objstm_out.pdf";
    std::error_code ec;
    std::filesystem::remove(out, ec);
    ASSERT_NO_THROW(doc->saveAs(out));
    ASSERT_TRUE(std::filesystem::exists(out));
    doc.reset();

    std::ifstream f(out, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(f)),
                            std::istreambuf_iterator<char>());
    f.close();
    ASSERT_FALSE(bytes.empty());

    EXPECT_NE(bytes.find("/ObjStm"), std::string::npos)
        << "a gc=3 (dedup) full rewrite expanded the document's object streams "
           "back into free-standing objects — the escalated GC path loses the "
           "compact shape the default path keeps";
    EXPECT_NE(bytes.find("/XRef"), std::string::npos)
        << "the gc=3 output has no cross-reference stream, so any object stream "
           "in it would be unreachable";

    // The retained shape must still be a readable document, not just the right
    // bytes: a file that keeps /ObjStm but no longer opens is a worse outcome
    // than one that expanded it.
    auto reopened = Document::open(ctx_, out);
    ASSERT_NE(reopened, nullptr)
        << "the gc=3 object-stream output does not reopen";
    EXPECT_EQ(reopened->pageCount(), 2);
    reopened.reset();
    std::filesystem::remove(out, ec);
}
