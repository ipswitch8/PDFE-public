// -*- coding: utf-8 -*-
//
// GitHub #354 — Crop Page must honor the vertical orientation of the selected
// region, not crop the mirrored part.
//
// The rect CropTool captures is in fitz/screen space (top-left origin, Y-DOWN),
// but a PDF /CropBox is PDF user space (Y-UP). Before the fix CropPageCommand
// wrote the captured rect straight into /CropBox with no Y-flip, so a selection
// in the TOP half of the page on screen cropped the BOTTOM half of the PDF
// (the retained region appeared blank / wrong). CropPageCommand now flips Y
// about the MediaBox height at the single fitz->PDF boundary.
//
// This is a GEOMETRY / vertical-orientation oracle, not a size-only one: it
// selects the TOP half of the page and asserts the stored /CropBox lands at
// HIGH PDF-y (near the MediaBox top). It fails pre-fix (the box lands low).
//
// Fixture: cropbox_offset.pdf — MediaBox [0 0 1200 1600] (origin at PDF y=0),
// read live via getPageMediaBox so the oracle is anchored to the real box.

#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/PageCommands.h"
#include "core/PageOperations.h"
#include "core/UndoManager.h"

#include <QRectF>

#include <filesystem>
#include <memory>

using namespace pdfe::core;

namespace {
std::filesystem::path fixturePath() {
    return std::filesystem::path(TEST_FIXTURES_DIR) / "cropbox_offset.pdf";
}
}  // namespace

TEST(CropPageVerticalFlip, TopHalfSelectionCropsTopOfPageNotBottom) {
    ASSERT_TRUE(std::filesystem::exists(fixturePath()))
        << "cropbox_offset.pdf is a committed fixture — regenerate with "
           "python tests/fixtures/gen_cropbox_offset.py";

    auto ctx = std::make_shared<MuPdfContext>();
    auto doc = Document::open(ctx, fixturePath());
    ASSERT_NE(doc, nullptr);

    // Real MediaBox, PDF user space. getPageMediaBox reports [x0 y0 w h] with
    // .top() = min PDF-y, .bottom() = max PDF-y.
    const QRectF media = getPageMediaBox(doc.get(), 0);
    ASSERT_FALSE(media.isEmpty());
    ASSERT_NEAR(media.top(), 0.0, 1.0)
        << "this oracle assumes an origin-0 MediaBox (fixture is [0 0 1200 1600])";
    const double H = media.height();
    ASSERT_GT(H, 1.0);

    // Select the TOP of the page as CropTool hands it to the command: fitz space
    // is Y-DOWN, so the top of the page is small Y. This rect spans fitz y in
    // [0, H/2] — the upper half on screen.
    const QRectF fitzTopHalf(media.left() + media.width() * 0.10,
                             0.0,
                             media.width() * 0.50,
                             H * 0.50);

    pdfe::commands::UndoManager mgr;
    auto cmd = std::make_unique<pdfe::commands::CropPageCommand>(
        doc.get(), 0, fitzTopHalf);
    mgr.execute(std::move(cmd));

    const QRectF stored = getPageCropBox(doc.get(), 0);
    ASSERT_FALSE(stored.isEmpty());

    // The stored /CropBox must sit in the TOP half of the page in PDF user space
    // (high Y). stored.top() = min PDF-y, stored.bottom() = max PDF-y.
    const double pageMidY     = media.top() + H * 0.5;
    const double storedCenter = stored.top() + stored.height() * 0.5;

    EXPECT_GT(storedCenter, pageMidY)
        << "GitHub #354: a TOP-half selection must crop the TOP of the page "
           "(high PDF-y). Stored CropBox center y=" << storedCenter
        << " must exceed page mid y=" << pageMidY
        << ". Pre-fix (no Y-flip) the box lands at ~" << (H * 0.25)
        << " — the BOTTOM half.";
    EXPECT_GT(stored.bottom(), media.top() + H * 0.75)
        << "the selection's top edge must map NEAR the MediaBox top (PDF-y ~"
        << (media.top() + H) << ")";

    // The Y-flip is a pure vertical mirror, so width/height are preserved.
    EXPECT_NEAR(stored.width(),  fitzTopHalf.width(),  2.0);
    EXPECT_NEAR(stored.height(), fitzTopHalf.height(), 2.0);
}
