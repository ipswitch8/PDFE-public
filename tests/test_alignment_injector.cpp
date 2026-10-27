// -*- coding: utf-8 -*-
// Phase 9 (splice): AlignmentInjector — appends AlignmentWriter output
// to a PDF's page content streams, idempotently.
#include <gtest/gtest.h>
#include "reflow/AlignmentInjector.h"
#include "reflow/AlignmentWriter.h"
#include "reflow/TextReflowEngine.h"
#include "reflow/ReflowTypes.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"

#include <filesystem>
#include <memory>
#include <cmath>
#include <string>

extern "C" {
#include <mupdf/fitz.h>
}

using namespace pdfe::reflow;
using namespace pdfe::core;

namespace {

// First-glyph upper-left x (points) of the first stext line whose text contains
// `needle`, read through an isolated fitz context (a fresh viewer's read).
// Returns NaN when not found. Used to prove a CENTER-aligned block actually
// shifts its lines right vs the LEFT baseline (phase-3 mutant M05 zeroed the
// Center offset → centered blocks rendered left-aligned; extraction is blind to
// x-position, so only a glyph-geometry oracle catches it).
float firstGlyphUlx(const std::filesystem::path& pdf, const std::string& needle)
{
    float result = std::nanf("");
    fz_context* ctx = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
    fz_register_document_handlers(ctx);
    fz_document* doc = nullptr;
    fz_stext_page* stext = nullptr;
    fz_try(ctx) {
        doc = fz_open_document(ctx, pdf.string().c_str());
        stext = fz_new_stext_page_from_page_number(ctx, doc, 0, nullptr);
        for (fz_stext_block* b = stext->first_block; b && std::isnan(result); b = b->next) {
            if (b->type != FZ_STEXT_BLOCK_TEXT) continue;
            for (fz_stext_line* ln = b->u.t.first_line; ln; ln = ln->next) {
                std::string t;
                for (fz_stext_char* ch = ln->first_char; ch; ch = ch->next) {
                    if (ch->c < 128) t.push_back(static_cast<char>(ch->c));
                }
                if (t.find(needle) == std::string::npos) continue;
                if (ln->first_char) result = ln->first_char->quad.ul.x;
                break;
            }
        }
    }
    fz_always(ctx) {
        if (stext) fz_drop_stext_page(ctx, stext);
        if (doc) fz_drop_document(ctx, doc);
    }
    fz_catch(ctx) {}
    fz_drop_context(ctx);
    return result;
}
std::filesystem::path fixturesDir()
{
    return std::filesystem::path(TEST_FIXTURES_DIR);
}
std::filesystem::path tempOut(const char* name)
{
    auto d = std::filesystem::temp_directory_path() / "pdfe_align_inject";
    std::filesystem::create_directories(d);
    return d / name;
}

std::vector<ReflowLine> buildLines(const std::string& text, float maxW, float charW)
{
    auto measurer = [charW](const std::string& w) {
        return static_cast<float>(w.size()) * charW;
    };
    auto words = TextReflowEngine::tokenize(text, measurer);
    return TextReflowEngine::reflow(words, maxW, charW);
}

int countOccurrences(const std::string& hay, const std::string& needle)
{
    int n = 0;
    size_t pos = 0;
    while ((pos = hay.find(needle, pos)) != std::string::npos) {
        ++n; pos += needle.size();
    }
    return n;
}

} // namespace

class AlignmentInjectorTest : public ::testing::Test {
protected:
    void SetUp() override {
        ctx = std::make_shared<MuPdfContext>();
        src = fixturesDir() / "test_3page.pdf";
        if (!std::filesystem::exists(src)) {
            GTEST_SKIP() << "test_3page.pdf not available";
        }
    }
    std::shared_ptr<MuPdfContext> ctx;
    std::filesystem::path src;

    static AlignmentInjector::PageBlock makeBlock(int pageIdx, Alignment align)
    {
        AlignmentInjector::PageBlock b;
        b.pageIndex = pageIdx;
        b.params.alignment = align;
        b.params.lines = buildLines(
            "injected alignment test paragraph wrapping",
            120.0f, 6.0f);
        b.params.blockLeftPt = 72.0f;
        b.params.blockTopPt  = 700.0f;
        b.params.blockWidthPt = 400.0f;
        b.params.lineHeightPt = 14.0f;
        b.params.fontSizePt = 12.0f;
        b.params.fontResourceName = "F1";
        return b;
    }
};

TEST_F(AlignmentInjectorTest, ApplyInjectsBlockAndFileRemainsParseable)
{
    AlignmentInjector inj(ctx);
    std::vector<AlignmentInjector::PageBlock> blocks = {
        makeBlock(0, Alignment::Left),
    };
    auto out = tempOut("apply_basic.pdf");
    std::filesystem::remove(out);
    ASSERT_NO_THROW(inj.apply(src, out, blocks));
    ASSERT_TRUE(std::filesystem::exists(out));

    auto doc = Document::open(ctx, out);
    ASSERT_NE(doc, nullptr);
    EXPECT_EQ(doc->pageCount(), 3);
}

TEST_F(AlignmentInjectorTest, ReApplyDoesNotStackDuplicates)
{
    AlignmentInjector inj(ctx);
    std::vector<AlignmentInjector::PageBlock> blocks = {
        makeBlock(0, Alignment::Center),
    };
    auto first = tempOut("idempotent_1.pdf");
    auto second = tempOut("idempotent_2.pdf");
    std::filesystem::remove(first);
    std::filesystem::remove(second);

    inj.apply(src, first, blocks);
    inj.apply(first, second, blocks);

    auto doc1 = Document::open(ctx, first);
    auto doc2 = Document::open(ctx, second);
    ASSERT_NE(doc1, nullptr);
    ASSERT_NE(doc2, nullptr);

    // Extract plain text from page 0 — a re-apply should not double
    // the "injected alignment test paragraph" wording.
    auto p1 = doc1->page(0);
    auto p2 = doc2->page(0);
    ASSERT_NE(p1, nullptr);
    ASSERT_NE(p2, nullptr);
    const int c1 = countOccurrences(p1->extractPlainText(),
                                    "injected alignment");
    const int c2 = countOccurrences(p2->extractPlainText(),
                                    "injected alignment");
    EXPECT_EQ(c1, c2) << "re-apply must not stack (was " << c1
                      << ", now " << c2 << ")";
}

TEST_F(AlignmentInjectorTest, RemoveDropsPreviouslyInjectedBlocks)
{
    AlignmentInjector inj(ctx);
    std::vector<AlignmentInjector::PageBlock> blocks = {
        makeBlock(1, Alignment::Right),
    };
    auto withInj = tempOut("with_injection.pdf");
    auto stripped = tempOut("stripped.pdf");
    std::filesystem::remove(withInj);
    std::filesystem::remove(stripped);

    inj.apply(src, withInj, blocks);
    inj.remove(withInj, stripped);

    auto doc = Document::open(ctx, stripped);
    ASSERT_NE(doc, nullptr);
    // Page 1 should no longer contain the injected phrase.
    auto p = doc->page(1);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->extractPlainText().find("injected alignment"),
              std::string::npos)
        << "remove() must drop AlignmentInjector-marked streams";
}

TEST_F(AlignmentInjectorTest, MultipleBlocksOnDifferentPagesEachLand)
{
    AlignmentInjector inj(ctx);
    AlignmentInjector::PageBlock a = makeBlock(0, Alignment::Left);
    // Use Right — Justify's Tw handling is covered in isolation by
    // test_alignment_writer.cpp; this test asserts multi-page
    // dispatch, so a simpler alignment keeps coverage decoupled.
    AlignmentInjector::PageBlock b = makeBlock(2, Alignment::Right);
    auto out = tempOut("multi_block.pdf");
    std::filesystem::remove(out);
    inj.apply(src, out, {a, b});

    auto doc = Document::open(ctx, out);
    ASSERT_NE(doc, nullptr);
    auto p0 = doc->page(0);
    auto p2 = doc->page(2);
    EXPECT_NE(p0->extractPlainText().find("injected alignment"),
              std::string::npos);
    EXPECT_NE(p2->extractPlainText().find("injected alignment"),
              std::string::npos);
    auto p1 = doc->page(1);
    EXPECT_EQ(p1->extractPlainText().find("injected alignment"),
              std::string::npos)
        << "page 1 wasn't targeted; must remain untouched";
}

// Phase-3 mutant M05 KILL: TextReflowEngine::lineXOffset supplies the Center
// x-shift that IS the alignment feature. The old suite asserted only
// extractPlainText().find(marker) (presence), never geometry — so a block drawn
// left-aligned when Center was requested extracted identically and survived.
// This metamorphic oracle injects the SAME reflowed block once Left and once
// Center and reads back the first line's first-glyph x: Center must sit well to
// the RIGHT of Left. Mutant (Center offset → 0) makes them equal → RED.
TEST_F(AlignmentInjectorTest, CenteredLineIsActuallyCenteredNotLeftAligned)
{
    AlignmentInjector inj(ctx);

    auto outLeft = tempOut("center_vs_left_L.pdf");
    auto outCenter = tempOut("center_vs_left_C.pdf");
    std::filesystem::remove(outLeft);
    std::filesystem::remove(outCenter);

    std::vector<AlignmentInjector::PageBlock> left = { makeBlock(0, Alignment::Left) };
    std::vector<AlignmentInjector::PageBlock> center = { makeBlock(0, Alignment::Center) };
    inj.apply(src, outLeft, left);
    inj.apply(src, outCenter, center);

    const float xLeft = firstGlyphUlx(outLeft, "injected");
    const float xCenter = firstGlyphUlx(outCenter, "injected");
    ASSERT_FALSE(std::isnan(xLeft)) << "left-aligned injected line not found";
    ASSERT_FALSE(std::isnan(xCenter)) << "center-aligned injected line not found";

    // blockWidthPt=400, blockLeftPt=72; each reflowed line is ≤120pt wide, so the
    // Center offset (blockWidth - lineWidth)/2 is well over 100pt. Require the
    // centered line to start clearly right of the left-aligned one. The mutant
    // zeroes the offset → xCenter == xLeft → this fails.
    EXPECT_GT(xCenter, xLeft + 30.0f)
        << "Center-aligned line was NOT shifted right of Left (xLeft=" << xLeft
        << " xCenter=" << xCenter << ") — lineXOffset Center offset is a no-op";

    // Absolute backstop: Left starts near blockLeft (72pt); Center must be pushed
    // well past it, independent of the Left baseline.
    EXPECT_GT(xCenter, 110.0f)
        << "Center line x=" << xCenter << " is at/near the left margin (72pt) — "
           "not centered";
}

TEST_F(AlignmentInjectorTest, EmptyBlockListProducesParseableFile)
{
    AlignmentInjector inj(ctx);
    auto out = tempOut("no_blocks.pdf");
    std::filesystem::remove(out);
    ASSERT_NO_THROW(inj.apply(src, out, {}));
    auto doc = Document::open(ctx, out);
    ASSERT_NE(doc, nullptr);
    EXPECT_EQ(doc->pageCount(), 3);
}
