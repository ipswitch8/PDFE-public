// -*- coding: utf-8 -*-
//
// Regression test for bugfix-3e91a7c4 #6 (the reproduced scroll/Replace-All crash).
//
// Root cause (see tests/docs/bugfix-3e91a7c4-phase1-findings.md): a page whose
// /ActualText marked-content entry is a proper PREFIX of the displayed span
// (e.g. the span shows "ABC" with /ActualText (A)) drives MuPDF's
// do_extract_within_actualtext (source/fitz/stext-device.c) to decrement the
// running actualtext length `z` (size_t) to 0 while the span still has
// unconsumed items (start < span->len). It then hits `assert(z != 0)` at
// stext-device.c:1142, and — because `z` is unsigned — line 1148's
// rune_index(actualtext, z-1) evaluates z-1 == SIZE_MAX and reads out of
// bounds. Debug build => abort() dialog; Release build (assert compiled out)
// => the user's reported "Access Violation: read address 0x0". PDFE reaches
// this via every text-extraction path (search / Replace All / edit /
// hasTextContent), so opening the real 51-page document and running Replace All
// (which extracts all pages) crashed.
//
// The fix: PDFE's text pipeline must extract the DISPLAYED glyphs the user sees
// and edits, not the /ActualText accessibility override — i.e. pass
// FZ_STEXT_IGNORE_ACTUALTEXT. That is both the correct extraction semantics for
// an editor and it bypasses the crashing MuPDF code path.
//
// This test is RED before the fix (Debug abort / Release AV inside
// extractPlainText) and GREEN after it (returns the displayed "ABC").

#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/Page.h"
#include "core/MuPdfContext.h"

#include <filesystem>
#include <memory>
#include <string>

using namespace pdfe::core;

TEST(ActualTextExtractionCrash, PrefixActualTextDoesNotAbortAndReturnsDisplayedGlyphs) {
    auto context = std::make_shared<MuPdfContext>();
    const std::filesystem::path pdf =
        std::filesystem::path(TEST_FIXTURES_DIR) / "actualtext_prefix_crash.pdf";
    ASSERT_TRUE(std::filesystem::exists(pdf))
        << "fixture missing: " << pdf.string();

    auto document = Document::open(context, pdf);
    ASSERT_NE(document, nullptr);
    auto page = document->page(0);
    ASSERT_NE(page, nullptr);

    // On unfixed code this call aborts (Debug assert at stext-device.c:1142) /
    // access-violates (Release) inside MuPDF's ActualText extraction.
    const std::string text = page->extractPlainText();

    // With the fix, extraction ignores /ActualText and returns the DISPLAYED
    // glyphs "ABC" (not the ActualText override "A").
    EXPECT_NE(text.find("ABC"), std::string::npos)
        << "expected displayed glyphs 'ABC' in extraction, got: '" << text << "'";
}
