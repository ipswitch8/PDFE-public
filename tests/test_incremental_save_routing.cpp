// -*- coding: utf-8 -*-
//
// Pipeline b2e47d91 — Tier 1: incremental-save routing for content-only
// sessions.
//
// Document::save() now routes a content-only session (form-field fills,
// annotation add/modify) back to the SAME file through the fast
// saveIncremental() append path instead of the multi-minute full-rewrite
// (do_garbage=3). Any destructive or structural operation taints the session
// (Document::sessionRequiresFullRewrite()), forcing the full rewrite so that
// removed/orphaned object bytes are scrubbed rather than left recoverable in
// the appended file.
//
// These tests lock:
//   * AC4b — a structural op (RotatePage) taints the session.
//   * AC4c — a form-field fill does NOT taint the session.
//   * security — redaction apply taints, and a redaction save does NOT take
//     the incremental-append path (which would leak the redacted bytes).
//   * the fast path actually appends for a content-only save.
//   * AC4a — (slow, gated by PDFE_RUN_SLOW_SAVE_TESTS) form-fill + save on the
//     ssa_sf86 XFA fixture completes well under 30 s.

#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "forms/FormManager.h"
#include "forms/FormFieldData.h"
#include "forms/FormFieldType.h"
#include "redaction/RedactionManager.h"
#include "textedit/ContentStreamEditor.h"
#include "textedit/TextEditModel.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

using namespace pdfe::core;

namespace {

std::filesystem::path fixturesDir()
{
    return std::filesystem::path(TEST_FIXTURES_DIR);
}

// Copy a fixture to a private temp path so a save() that writes in place does
// not mutate the checked-in fixture.
std::filesystem::path workCopy(const std::filesystem::path& fixtureRel,
                               const char* workName)
{
    auto d = std::filesystem::temp_directory_path() / "pdfe_incremental_routing";
    std::filesystem::create_directories(d);
    auto p = d / workName;
    std::filesystem::copy_file(fixturesDir() / fixtureRel, p,
        std::filesystem::copy_options::overwrite_existing);
    return p;
}

std::string readAll(const std::filesystem::path& p)
{
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// True iff `after` is `before` followed by additional bytes — i.e. the save
// literally appended (incremental) rather than rewrote the whole file.
bool isIncrementalAppend(const std::string& before, const std::string& after)
{
    return after.size() > before.size()
        && after.compare(0, before.size(), before) == 0;
}

// Find the first editable text field on the page; -1 if none.
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

} // namespace

class IncrementalSaveRoutingTest : public ::testing::Test {
protected:
    void SetUp() override {
        ctx_ = std::make_shared<MuPdfContext>();
    }
    std::shared_ptr<MuPdfContext> ctx_;
};

// ── AC4b — a structural op taints the session ────────────────────────────
TEST_F(IncrementalSaveRoutingTest, RotatePageTaintsSession)
{
    auto path = workCopy("test_3page.pdf", "rotate_taint.pdf");
    auto doc = Document::open(ctx_, path);
    ASSERT_NE(doc, nullptr);

    EXPECT_FALSE(doc->sessionRequiresFullRewrite())
        << "a freshly opened document is incremental-eligible";

    doc->rotatePage(0, 90);

    EXPECT_TRUE(doc->sessionRequiresFullRewrite())
        << "page rotation is a structural edit and must force a full rewrite";
}

// ── AC4c — a form-field fill leaves the session content-only ──────────────
TEST_F(IncrementalSaveRoutingTest, FormFillKeepsSessionContentOnly)
{
    auto path = workCopy("test_form.pdf", "formfill_contentonly.pdf");
    auto doc = Document::open(ctx_, path);
    ASSERT_NE(doc, nullptr);

    pdfe::forms::FormManager fm(ctx_);
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);
    const int idx = firstEditableTextField(fm, page.get());
    ASSERT_GE(idx, 0) << "test_form.pdf must contain an editable text field";

    fm.setFieldValue(page.get(), idx, "tier1_value");

    EXPECT_FALSE(doc->sessionRequiresFullRewrite())
        << "a pure form-field fill is additive and stays incremental-eligible";
}

// ── The fast path actually appends for a content-only save ───────────────
TEST_F(IncrementalSaveRoutingTest, ContentOnlySaveAppendsIncrementally)
{
    auto path = workCopy("test_form.pdf", "formfill_append.pdf");
    const std::string before = readAll(path);

    {
        auto doc = Document::open(ctx_, path);
        ASSERT_NE(doc, nullptr);
        pdfe::forms::FormManager fm(ctx_);
        auto page = doc->page(0);
        const int idx = firstEditableTextField(fm, page.get());
        ASSERT_GE(idx, 0);
        fm.setFieldValue(page.get(), idx, "tier1_append");

        ASSERT_FALSE(doc->sessionRequiresFullRewrite());
        page.reset();
        doc->save(path);   // same path + content-only → should append
    }

    const std::string after = readAll(path);
    EXPECT_TRUE(isIncrementalAppend(before, after))
        << "a content-only save to the original path must take the fast "
        << "incremental-append route (original bytes preserved as a prefix)";
}

// ── a vector-PDF text edit taints the session ────────────────────────────
// AC1's prose lists "text edits" as content-only, but a text edit rewrites the
// page content stream via pdf_filter_page_contents, orphaning the ORIGINAL text
// object. Persisting that incrementally would leave the pre-edit text
// recoverable — a confidentiality regression, since users edit text precisely
// to remove/replace sensitive strings. So text edits are classified as
// full-rewrite. (Form fills, the XFA performance target, stay content-only —
// see FormFillKeepsSessionContentOnly.) This test locks that intentional
// classification so a future change can't silently route text edits to the
// incremental-append path.
TEST_F(IncrementalSaveRoutingTest, TextEditTaintsSession)
{
    auto path = workCopy("test_3page.pdf", "textedit_taint.pdf");
    auto doc = Document::open(ctx_, path);
    ASSERT_NE(doc, nullptr);

    pdfe::textedit::ContentStreamEditor editor(ctx_.get());
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    auto blocks = page->extractText();
    pdfe::textedit::TextEditModel model(blocks);
    std::vector<pdfe::textedit::TextSpan> spans;
    for (int i = 0; i < model.spanCount(); ++i) {
        spans.push_back(model.span(i));
    }
    ASSERT_GT(spans.size(), 0u) << "test_3page.pdf must yield text spans";

    std::vector<std::pair<int, std::u32string>> mods{ {0, U"edited"} };
    editor.applyEdits(page.get(), doc.get(), spans, mods);

    EXPECT_TRUE(doc->sessionRequiresFullRewrite())
        << "a content-stream text edit orphans the original text and must "
        << "force a full rewrite, never an incremental append";
}

// ── security — redaction apply taints the session ────────────────────────
TEST_F(IncrementalSaveRoutingTest, RedactionApplyTaintsSession)
{
    auto path = workCopy("test_redaction.pdf", "redact_taint.pdf");
    auto doc = Document::open(ctx_, path);
    ASSERT_NE(doc, nullptr);

    pdfe::redaction::RedactionManager rm(ctx_);
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);
    rm.createRedactionMark(page.get(), QRectF(100, 100, 120, 40));
    page.reset();

    // rewriteDocument=false applies the redaction in memory only and defers the
    // scrub to the subsequent save() — the path (also used by BatchEngine) where
    // the taint MUST persist so that save does not leak via incremental append.
    pdfe::redaction::RedactionApplyOptions opts;
    opts.rewriteDocument = false;
    const auto result = rm.applyAllRedactions(doc.get(), opts);
    ASSERT_GT(result.marksApplied, 0)
        << "the created redaction mark must be detected by applyAllRedactions";

    EXPECT_TRUE(doc->sessionRequiresFullRewrite())
        << "applied redaction permanently removes content and must force a "
        << "full rewrite — incremental append would leave the redacted bytes "
        << "physically recoverable in the file";
}

// ── security — a redaction save must NOT append incrementally ────────────
TEST_F(IncrementalSaveRoutingTest, RedactionSaveDoesNotAppendIncrementally)
{
    auto path = workCopy("test_redaction.pdf", "redact_save.pdf");
    const std::string before = readAll(path);

    {
        auto doc = Document::open(ctx_, path);
        ASSERT_NE(doc, nullptr);
        pdfe::redaction::RedactionManager rm(ctx_);
        auto page = doc->page(0);
        rm.createRedactionMark(page.get(), QRectF(100, 100, 120, 40));
        page.reset();
        // rewriteDocument=false defers the scrub to this save() — the case the
        // taint exists to protect.
        pdfe::redaction::RedactionApplyOptions opts;
        opts.rewriteDocument = false;
        const auto result = rm.applyAllRedactions(doc.get(), opts);
        ASSERT_GT(result.marksApplied, 0);
        ASSERT_TRUE(doc->sessionRequiresFullRewrite());
        doc->save(path);
    }

    const std::string after = readAll(path);
    EXPECT_FALSE(isIncrementalAppend(before, after))
        << "redaction must full-rewrite (scrubbing orphaned bytes), never "
        << "append the original file — an append would leak redacted content";
}

// ── AC4a — content-only save on a large XFA form is fast (slow/gated) ─────
TEST_F(IncrementalSaveRoutingTest, FormFillSaveIsFastOnXfa)
{
    if (!slowSaveTestsEnabled()) {
        GTEST_SKIP() << "set PDFE_RUN_SLOW_SAVE_TESTS=1 (Release build) to run "
                        "the large-XFA timing test";
    }
    const auto fixture = std::filesystem::path("realworld") / "ssa_sf86.pdf";
    if (!std::filesystem::exists(fixturesDir() / fixture)) {
        GTEST_SKIP() << "ssa_sf86.pdf fixture not available";
    }
    auto path = workCopy(fixture, "xfa_formfill_fast.pdf");
    auto doc = Document::open(ctx_, path);
    ASSERT_NE(doc, nullptr);

    // Fill an editable text field if one is reachable; the routing decision
    // (content-only → incremental) is what we are timing either way.
    pdfe::forms::FormManager fm(ctx_);
    auto page = doc->page(0);
    if (page) {
        const int idx = firstEditableTextField(fm, page.get());
        if (idx >= 0) {
            fm.setFieldValue(page.get(), idx, "tier1_xfa");
        }
        page.reset();
    }
    ASSERT_FALSE(doc->sessionRequiresFullRewrite());

    const auto t0 = std::chrono::steady_clock::now();
    doc->save(path);
    const auto t1 = std::chrono::steady_clock::now();
    const long long elapsedMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

    EXPECT_LT(elapsedMs, 30000)
        << "content-only save on ssa_sf86 took " << elapsedMs
        << " ms — incremental routing should keep it far under the 314 s "
        << "gc=3 full-rewrite baseline";

    // A timing bound alone is satisfied perfectly by a save that did nothing.
    // Re-open the target (outside the timed window) so "fast" is anchored to
    // "and still a usable document" (GitHub #163).
    doc.reset();
    auto reopened = Document::open(ctx_, path);
    ASSERT_NE(reopened, nullptr)
        << "the fast incremental save produced a file that will not re-open";
    EXPECT_GT(reopened->pageCount(), 0);
}
