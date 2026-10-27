// -*- coding: utf-8 -*-
//
// Verifies the branding + legal Qt resource collection is actually
// compiled into the test binary and serves the expected payloads.
//
// The branding.qrc lives in pdfe_ui (linked into pdfe_tests via the
// existing test target dependencies). Because pdfe_ui is a static
// library, the auto-generated `qInitResources_branding()` symbol can
// be stripped at link time if nothing references it. Q_INIT_RESOURCE
// in the test setup pins it in. AboutDialog will do the same in
// production code once Phase 5 lands.
//
// What this test pins:
// * :/branding/color_logo.png loads as a non-null QPixmap whose
//   underlying QImage has alpha (transparent background present).
//   Dimensions are asserted as bounded ranges, not exact pixel
//   counts, so a future re-crop / re-export does not break the test
//   while still failing if the logo is replaced with a tiny stub.
// * :/branding/itwerx_logo_small.png loads as a non-null QPixmap.
// * :/legal/LICENSE.txt is readable and contains the AGPL-3.0
//   marker.

#include <gtest/gtest.h>

#include <QByteArray>
#include <QFile>
#include <QImage>
#include <QPixmap>
#include <QResource>
#include <QString>
#include <QTextStream>

class BrandingResourcesTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        // Pin the static-lib resource init so the linker can't strip
        // qInitResources_branding(). Safe to call repeatedly; Qt
        // refcounts the registration.
        Q_INIT_RESOURCE(branding);
    }
};

TEST_F(BrandingResourcesTest, ColorLogoLoads) {
    QPixmap pix(":/branding/color_logo.png");
    ASSERT_FALSE(pix.isNull())
        << "color_logo.png missing from :/branding/ resource path";

    // Invariant: the logo is large enough to be a real branding asset
    // (not a stub) but bounded so a runaway export doesn't slip
    // through. Cropped dimensions are 901x674 today; bound generously
    // for future re-exports.
    EXPECT_GT(pix.width(), 400);
    EXPECT_LT(pix.width(), 2048);
    EXPECT_GT(pix.height(), 300);
    EXPECT_LT(pix.height(), 1536);

    // Invariant: cropped + transparent-bg means the underlying QImage
    // has alpha and at least one fully-transparent pixel. Both are
    // false for the pre-Phase-1 flood-fill version.
    const QImage img = pix.toImage();
    ASSERT_FALSE(img.isNull());
    EXPECT_TRUE(img.hasAlphaChannel())
        << "color_logo.png lost its alpha channel — the cropped/"
           "transparent contract is violated.";
    bool sawTransparent = false;
    for (int y = 0; y < img.height() && !sawTransparent; ++y) {
        for (int x = 0; x < img.width() && !sawTransparent; ++x) {
            if (qAlpha(img.pixel(x, y)) == 0) {
                sawTransparent = true;
            }
        }
    }
    EXPECT_TRUE(sawTransparent)
        << "color_logo.png has no fully-transparent pixels — the "
           "background was not converted to alpha=0.";
}

TEST_F(BrandingResourcesTest, ItwerxLogoLoads) {
    QPixmap pix(":/branding/itwerx_logo_small.png");
    ASSERT_FALSE(pix.isNull())
        << "itwerx_logo_small.png missing from :/branding/ resource path";
    // Source is 353x73; we don't pin exact dims here in case the
    // asset gets re-exported with minor rounding, but assert it's
    // in a reasonable range.
    EXPECT_GT(pix.width(), 100);
    EXPECT_LT(pix.width(), 600);
    EXPECT_GT(pix.height(), 30);
    EXPECT_LT(pix.height(), 200);

    // Invariant: same alpha contract as color_logo, plus the >=5%
    // floor mandated by phase-1 acceptance criterion 5 — a near-white
    // background converted to alpha=0 should leave a substantial
    // transparent area, not just a couple of edge pixels.
    const QImage img = pix.toImage();
    ASSERT_FALSE(img.isNull());
    EXPECT_TRUE(img.hasAlphaChannel())
        << "itwerx_logo_small.png lost its alpha channel.";
    qint64 transparentPixels = 0;
    const qint64 totalPixels = static_cast<qint64>(img.width())
                             * static_cast<qint64>(img.height());
    for (int y = 0; y < img.height(); ++y) {
        for (int x = 0; x < img.width(); ++x) {
            if (qAlpha(img.pixel(x, y)) == 0) {
                ++transparentPixels;
            }
        }
    }
    EXPECT_GT(transparentPixels, 0)
        << "itwerx_logo_small.png has no fully-transparent pixels.";
    EXPECT_GE(transparentPixels * 100, totalPixels * 5)
        << "itwerx_logo_small.png has < 5% transparent pixels ("
        << transparentPixels << " of " << totalPixels
        << "); the near-white background was not converted to alpha=0.";
}

TEST_F(BrandingResourcesTest, LicenseTextLoads) {
    QFile file(":/legal/LICENSE.txt");
    ASSERT_TRUE(file.open(QIODevice::ReadOnly | QIODevice::Text))
        << "LICENSE.txt missing from :/legal/ resource path";
    const QString text = QTextStream(&file).readAll();
    EXPECT_TRUE(text.contains("GNU AFFERO GENERAL PUBLIC LICENSE"))
        << "LICENSE.txt does not contain the AGPL-3.0 marker";
    EXPECT_GT(text.size(), 10000)
        << "LICENSE.txt suspiciously short — full AGPL text expected";
}
