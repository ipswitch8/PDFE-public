// -*- coding: utf-8 -*-
//
// Verifies the application icon resource (pdfe_icons.qrc):
//   :/icons/pdfe.ico   — multi-resolution Windows ICO sourced from
//                        pdfe-red.ico at the repo root (16/24/32/48/
//                        64/128/256 PNG-encoded frames). Sole icon
//                        source for the application after the SVG
//                        theme-aware variants were retired.
//
// What this test pins:
//   * QIcon(":/icons/pdfe.ico") is non-null and the 16x16 frame
//     contains live (non-transparent) pixels — Windows pulls the
//     16x16 entry for the title bar at 100% DPI; an all-transparent
//     frame would render an invisible title-bar glyph.
//   * pdfeAppIcon() returns a non-null QIcon and pixmap(64,64) is
//     non-null — the helper used by MainWindow + AboutDialog.

#include <gtest/gtest.h>

#include <QIcon>
#include <QImage>
#include <QPixmap>
#include <QSize>

#include "ui/MainWindow.h"  // for pdfeAppIcon()

class PdfeIconsTest : public ::testing::Test {};

TEST_F(PdfeIconsTest, IcoResourceLoads) {
    QIcon ic(QStringLiteral(":/icons/pdfe.ico"));
    ASSERT_FALSE(ic.isNull())
        << ":/icons/pdfe.ico missing from the embedded qrc";
    QPixmap pm = ic.pixmap(QSize(32, 32));
    EXPECT_FALSE(pm.isNull())
        << ":/icons/pdfe.ico failed to render at 32x32";
}

TEST_F(PdfeIconsTest, IcoSmallSize16IsLiveContent) {
    // Confirm via QIcon(":/icons/pdfe.ico").pixmap(16,16) that the
    // 16x16 entry is non-null and not all-transparent. This is what
    // Windows pulls for the title-bar icon at 100% DPI; if it were
    // all-transparent, even a correct setWindowIcon() call would
    // render an invisible glyph in the title bar.
    QIcon ic(QStringLiteral(":/icons/pdfe.ico"));
    ASSERT_FALSE(ic.isNull());
    QPixmap pm = ic.pixmap(QSize(16, 16));
    ASSERT_FALSE(pm.isNull())
        << ":/icons/pdfe.ico has no readable 16x16 frame.";
    QImage img = pm.toImage();
    ASSERT_FALSE(img.isNull());
    bool sawOpaqueOrSemi = false;
    for (int y = 0; y < img.height() && !sawOpaqueOrSemi; ++y) {
        for (int x = 0; x < img.width() && !sawOpaqueOrSemi; ++x) {
            if (qAlpha(img.pixel(x, y)) > 0) {
                sawOpaqueOrSemi = true;
            }
        }
    }
    EXPECT_TRUE(sawOpaqueOrSemi)
        << "16x16 frame of pdfe.ico is fully transparent — "
           "Windows would render the title bar as invisible.";
}

TEST_F(PdfeIconsTest, AppIconHelperReturnsNonNull) {
    // pdfeAppIcon() is the central helper used by main.cpp,
    // MainWindow, and AboutDialog. With the SVG theme-aware
    // variants retired, it always returns QIcon(":/icons/pdfe.ico").
    QIcon ic = pdfe::ui::pdfeAppIcon();
    ASSERT_FALSE(ic.isNull())
        << "pdfeAppIcon() returned a null QIcon — the .ico resource "
           "is not embedded or qico image-format plugin missing.";
    QPixmap pm = ic.pixmap(QSize(64, 64));
    EXPECT_FALSE(pm.isNull())
        << "pdfeAppIcon().pixmap(64,64) is null — the ICO resource "
           "cannot render at 64x64.";
}
