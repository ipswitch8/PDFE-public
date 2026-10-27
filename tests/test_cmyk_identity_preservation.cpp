// -*- coding: utf-8 -*-
//
// R3-06 (follow-up) — DeviceCMYK / DeviceGray / Separation-spot colour-OPERATOR
// IDENTITY preservation on text edit + save.
//
// Gap this closes (docs/content-stream-state-continuity-audit.md; the P5
// gate-accepted R3-06 follow-up): the existing R3-06 test
// (test_fidelity_r3_06_color_preservation.cpp) pins document-level colour
// SURVIVAL — the /Separation resource, the unedited `scn` operator, the
// overprint ExtGState, and "no render-to-black". It explicitly does NOT pin the
// EDITED span's colour-OPERATOR identity, because the MuPDF sanitize
// text_filter callback exposes only trm/ctm/bbox — NOT the graphics-state fill
// colourspace — so an edited DeviceCMYK span was re-emitted as a
// visually-equivalent DeviceRGB `rg`, silently flattening the print-critical
// colourspace identity (`k` -> `rg`).
//
// The fix (src/textedit/ContentStreamEditor.cpp): a ColorTracker chain
// processor runs UPSTREAM of the sanitize filter, observes the fill-colour
// operators (g/rg/k and cs+scn) with q/Q save-restore, and records the source
// fill into FilterState::currentFill. textFilterCallback captures it onto the
// matched span, and the injection callbacks reproduce the ORIGINAL operator
// (`k` for DeviceCMYK, `g` for DeviceGray, `/CS1 cs … scn` for a Separation
// spot) instead of an unconditional `rg` flatten. DeviceRGB stays `rg`.
//
// Oracle (class-level, on the SAVED-AND-REOPENED file): edit one span, then
// decompress the page content and inspect the INJECTED block (the BT…text
// window of the replacement) for the colour operator that immediately precedes
// the replacement text. RED (pre-fix): the injected block carries an `rg`
// flatten and lacks the source `k`/`scn` operator. GREEN: the source operator
// with its original operands is present and no `rg` flatten appears.
//
// Fixture: tests/fixtures/cmyk_identity.pdf (gen_cmyk_identity.py) — five spans:
//   PUREK  `0 0 0 1 k` | RICHBK `0.2 0.2 0.2 1 k` | SPOTA/SPOTB `/CS1 cs 1 scn`
//   | RGBRED `1 0 0 rg`. SPOTB is left unedited so /CS1 stays referenced/alive
//   when SPOTA is edited (device CMYK/gray need no resource).

#include <gtest/gtest.h>

// GetCurrentProcessId, for the per-process temp directory in SetUp.
#include <windows.h>

#include "core/Document.h"
#include "core/Page.h"
#include "core/MuPdfContext.h"
#include "textedit/ContentStreamEditor.h"
#include "textedit/TextEditModel.h"

#include "support/PdfContentOracle.h"

#include <filesystem>
#include <memory>
#include <string>

using namespace pdfe::core;
using namespace pdfe::textedit;
using pdfe::testsupport::decompressedPageContent;

namespace {

// Return the injected content window for a replacement: the substring from the
// nearest "BT" preceding the replacement text back-to-front, up to (not
// including) the replacement text itself. The injection emits, in order,
// BT / Tf / (Tz Tc Tw Ts) / <colour op> / Tm / Tj(replacement), so this window
// contains exactly the colour operator the injection chose for the edited span.
std::string injectedColorWindow(const std::string& content,
                                const std::string& replAscii) {
    size_t tj = content.find(replAscii);
    if (tj == std::string::npos) return "";
    size_t bt = content.rfind("BT", tj);
    if (bt == std::string::npos) return "";
    return content.substr(bt, tj - bt);
}

class CmykIdentityPreservation : public ::testing::Test {
protected:
    void SetUp() override {
        context = std::make_shared<MuPdfContext>();
        std::filesystem::path dir = TEST_FIXTURES_DIR;
        fixture = dir / "cmyk_identity.pdf";
        if (!std::filesystem::exists(fixture)) {
            GTEST_SKIP() << "cmyk_identity.pdf fixture not available";
        }
        const std::string tn =
            ::testing::UnitTest::GetInstance()->current_test_info()->name();
        // PER-PROCESS DIRECTORY, not just a per-test NAME (phase-59).
        //
        // The per-test name below already fixed the in-process collision the
        // comment in the sibling fidelity fixture describes. It does NOT
        // isolate this run from anything ELSE living in the shared %TEMP%
        // root, and a sharded suite plus a rerun pass plus an on-access
        // scanner all touch that root at once. Measured 2026-08-13: a full
        // 16-shard run failed two of these cases with "Failed to atomically
        // replace original file (original left intact): Access is denied",
        // and THREE sibling temp files from the same window were still on
        // disk afterwards because their TearDown remove() had failed too --
        // a directory-wide lock window, not a defect in the save.
        //
        // The product behaved correctly there: it refused and left the target
        // byte-intact. What is wrong is the test writing into a directory it
        // does not own. Same fix as the LTV responder fixture took in
        // phase-52.
        outDir_ = std::filesystem::temp_directory_path()
                / ("pdfe_cmyk_ident_p"
                   + std::to_string(static_cast<unsigned long>(
                         ::GetCurrentProcessId())));
        std::filesystem::create_directories(outDir_);
        out = outDir_ / (tn + ".pdf");
        std::error_code ec;
        std::filesystem::remove(out, ec);
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove(out, ec);
        // Best-effort: removes the per-process directory only once the LAST
        // case in this process has cleaned up, because remove() on a
        // non-empty directory simply fails. No case may assume it ran last.
        std::filesystem::remove(outDir_, ec);
    }

    // Edit the span containing `needle`, replacing it with `replacement`, then
    // save to `out`. Returns false if the span could not be located.
    bool editSpanAndSave(const std::u32string& needle,
                         const std::u32string& replacement) {
        auto document = Document::open(context, fixture);
        if (!document) return false;
        auto page = document->page(0);
        if (!page) return false;

        auto blocks = page->extractText();
        TextEditModel model(blocks);
        std::vector<TextSpan> spans;
        for (int i = 0; i < model.spanCount(); ++i)
            spans.push_back(model.span(i));

        int targetIdx = -1;
        for (int i = 0; i < static_cast<int>(spans.size()); ++i) {
            if (spans[i].text.find(needle) != std::u32string::npos) {
                targetIdx = i;
                break;
            }
        }
        if (targetIdx < 0) return false;

        ContentStreamEditor editor(context.get());
        std::vector<std::pair<int, std::u32string>> mods;
        mods.emplace_back(targetIdx, replacement);
        editor.applyEdits(page.get(), document.get(), spans, mods);

        page.reset();
        document->save(out);
        document.reset();
        return true;
    }

    std::shared_ptr<MuPdfContext> context;
    std::filesystem::path fixture;
    std::filesystem::path outDir_;
    std::filesystem::path out;
};

// Pure black DeviceCMYK (0 0 0 1 k) must round-trip as `k`, not `rg`.
// Kills the mutation "edited CMYK span flattened to DeviceRGB": pre-fix the
// injected block carries `0 0 0 rg` (the stext-captured RGB) instead of the
// source `0 0 0 1 k`.
TEST_F(CmykIdentityPreservation, PureBlackCmykStaysK) {
    ASSERT_TRUE(editSpanAndSave(U"PUREK", U"ZPUREK"))
        << "could not locate the PUREK span to edit";
    ASSERT_TRUE(std::filesystem::exists(out));

    const std::string content = decompressedPageContent(out, 0);
    const std::string block = injectedColorWindow(content, "ZPUREK");
    ASSERT_FALSE(block.empty())
        << "edited replacement text ZPUREK not found in saved content";

    EXPECT_NE(block.find("0 0 0 1 k"), std::string::npos)
        << "edited DeviceCMYK pure-black span lost its `k` operator identity "
           "(flattened to RGB) — injected block:\n" << block;
    EXPECT_EQ(block.find("rg"), std::string::npos)
        << "edited DeviceCMYK span was flattened to a DeviceRGB `rg` operator — "
           "injected block:\n" << block;
}

// Rich black DeviceCMYK (0.2 0.2 0.2 1 k) must preserve all four components as
// `k`. Kills a mutation that keeps only the K channel or flattens to rg — the
// stext RGB of this colour is black (0 0 0), so a flatten is a total loss of
// the C/M/Y ink build.
TEST_F(CmykIdentityPreservation, RichBlackCmykKeepsAllComponents) {
    ASSERT_TRUE(editSpanAndSave(U"RICHBK", U"ZRICHBK"))
        << "could not locate the RICHBK span to edit";
    ASSERT_TRUE(std::filesystem::exists(out));

    const std::string content = decompressedPageContent(out, 0);
    const std::string block = injectedColorWindow(content, "ZRICHBK");
    ASSERT_FALSE(block.empty())
        << "edited replacement text ZRICHBK not found in saved content";

    // MuPDF serialises fractional operands with the leading zero stripped
    // (0.2 -> .2), so accept either canonical numeric form. Both express the
    // identical four-component rich-black CMYK build; a flatten-to-rg or a
    // K-only mutation matches neither, so the teeth are preserved.
    const bool hasRichBlackK =
        block.find("0.2 0.2 0.2 1 k") != std::string::npos ||
        block.find(".2 .2 .2 1 k") != std::string::npos;
    EXPECT_TRUE(hasRichBlackK)
        << "edited DeviceCMYK rich-black span lost its exact CMYK build "
           "(expected `.2 .2 .2 1 k`) — injected block:\n" << block;
    EXPECT_EQ(block.find("rg"), std::string::npos)
        << "edited DeviceCMYK rich-black span was flattened to `rg` — "
           "injected block:\n" << block;
}

// Separation spot (/CS1 cs 1 scn) must round-trip as the named colourspace +
// scn tint, not a flattened rg. SPOTB is left unedited so /CS1 stays alive.
// Kills the mutation "edited spot span dropped to RGB tint".
TEST_F(CmykIdentityPreservation, SpotColourStaysSeparationScn) {
    ASSERT_TRUE(editSpanAndSave(U"SPOTA", U"ZSPOTA"))
        << "could not locate the SPOTA span to edit";
    ASSERT_TRUE(std::filesystem::exists(out));

    const std::string content = decompressedPageContent(out, 0);
    const std::string block = injectedColorWindow(content, "ZSPOTA");
    ASSERT_FALSE(block.empty())
        << "edited replacement text ZSPOTA not found in saved content";

    EXPECT_NE(block.find("/CS1 cs"), std::string::npos)
        << "edited Separation spot span lost its `/CS1 cs` colourspace "
           "selection — injected block:\n" << block;
    EXPECT_NE(block.find("scn"), std::string::npos)
        << "edited Separation spot span lost its `scn` tint operator — "
           "injected block:\n" << block;
    EXPECT_EQ(block.find("rg"), std::string::npos)
        << "edited Separation spot span was flattened to a DeviceRGB `rg` "
           "tint — injected block:\n" << block;
}

// Regression guard: DeviceRGB edits must KEEP emitting `rg` (the fix must not
// over-reach and rewrite RGB spans as k/scn). GREEN both before and after the
// fix — pins the "keep DeviceRGB as rg" contract.
TEST_F(CmykIdentityPreservation, DeviceRgbStaysRg) {
    ASSERT_TRUE(editSpanAndSave(U"RGBRED", U"ZRGBRED"))
        << "could not locate the RGBRED span to edit";
    ASSERT_TRUE(std::filesystem::exists(out));

    const std::string content = decompressedPageContent(out, 0);
    const std::string block = injectedColorWindow(content, "ZRGBRED");
    ASSERT_FALSE(block.empty())
        << "edited replacement text ZRGBRED not found in saved content";

    EXPECT_NE(block.find("1 0 0 rg"), std::string::npos)
        << "edited DeviceRGB span lost its `rg` operator — injected block:\n"
        << block;
    // Must not have been rewritten into a CMYK `k` operator.
    EXPECT_EQ(block.find(" k"), std::string::npos)
        << "edited DeviceRGB span was wrongly rewritten to a CMYK `k` "
           "operator — injected block:\n" << block;
}

} // namespace
