// -*- coding: utf-8 -*-
// Pipeline E phase-4 / Bucket 3a: custom stamp from image.
//
// Verifies the ImageStampLibrary import/retrieval API and the
// AnnotationManager::setStampImageAppearance path that writes a
// Form XObject containing the imported image as a /Stamp annotation
// appearance, then survives a save+reopen round-trip.
#include <gtest/gtest.h>

#include "annotations/AnnotationManager.h"
#include "annotations/AnnotationData.h"
#include "annotations/ImageStampLibrary.h"
#include "annotations/StampTool.h"
#include "support/SettingsIsolation.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"

#include <QBuffer>
#include <QByteArray>
#include <QCoreApplication>
#include <QImage>
#include <QRectF>
#include <QSettings>
#include <QString>
#include <QTemporaryDir>
#include <QTemporaryFile>

#include <cstring>
#include <filesystem>
#include <memory>
#include <string>

using namespace pdfe::annotations;
using namespace pdfe::core;

namespace {

// Build a tiny opaque RGB QImage filled with a strong pure red so we
// can verify, after stamping and rendering, that the stamp's pixels
// dominate the rendered region (R >> G, R >> B). A pure-red stamp on
// a white page is the easiest signal to detect via Page::render.
QImage makeTestImage(int w = 32, int h = 16)
{
    QImage img(w, h, QImage::Format_RGB32);
    img.fill(qRgb(220, 20, 20));
    return img;
}

// Count pixels in `img` whose RGB looks "red-dominant" (R high,
// G/B low) — i.e. the signature of our test stamp. Used to detect
// that the stamp's appearance stream actually rendered onto the
// page bitmap.
int countRedPixels(const QImage& imgIn,
                   int rMin = 150, int gMax = 100, int bMax = 100)
{
    // Page::render returns QImage::Format_RGBA8888 (byte-order RGBA),
    // which differs from QRgb's ARGB packed layout. Convert to ARGB32
    // first so qRed/qGreen/qBlue interpret pixels correctly.
    QImage img = imgIn.format() == QImage::Format_ARGB32
                     ? imgIn
                     : imgIn.convertToFormat(QImage::Format_ARGB32);
    int count = 0;
    for (int y = 0; y < img.height(); ++y) {
        const QRgb* row = reinterpret_cast<const QRgb*>(img.constScanLine(y));
        for (int x = 0; x < img.width(); ++x) {
            QRgb p = row[x];
            if (qRed(p) >= rMin && qGreen(p) <= gMax && qBlue(p) <= bMax) {
                ++count;
            }
        }
    }
    return count;
}

QString writeImageToTempPng(const QImage& img, QTemporaryDir& dir,
                            const QString& fileName = QStringLiteral("stamp.png"))
{
    QString path = dir.filePath(fileName);
    bool ok = img.save(path, "PNG");
    EXPECT_TRUE(ok) << "Failed to write fixture PNG to " << path.toStdString();
    return path;
}

} // namespace

// ---------------- ImageStampLibrary unit tests ----------------

TEST(ImageStampLibrary, ImportAndRetrieveByName)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    QImage src = makeTestImage();
    QString path = writeImageToTempPng(src, dir);

    ImageStampLibrary lib;
    EXPECT_EQ(lib.count(), 0);
    EXPECT_FALSE(lib.hasStamp(QStringLiteral("logo")));

    ASSERT_TRUE(lib.importImage(QStringLiteral("logo"), path));
    EXPECT_EQ(lib.count(), 1);
    EXPECT_TRUE(lib.hasStamp(QStringLiteral("logo")));
    EXPECT_EQ(lib.stampNames(), QStringList{QStringLiteral("logo")});

    QImage retrieved = lib.stamp(QStringLiteral("logo"));
    EXPECT_FALSE(retrieved.isNull());
    EXPECT_EQ(retrieved.width(), src.width());
    EXPECT_EQ(retrieved.height(), src.height());
}

TEST(ImageStampLibrary, ImportFailsForMissingFile)
{
    ImageStampLibrary lib;
    EXPECT_FALSE(lib.importImage(QStringLiteral("nope"),
                                 QStringLiteral("/path/does/not/exist.png")));
    EXPECT_EQ(lib.count(), 0);
}

TEST(ImageStampLibrary, AddStampReplacesByName)
{
    ImageStampLibrary lib;
    QImage a = makeTestImage(8, 8);
    QImage b = makeTestImage(16, 16);
    ASSERT_TRUE(lib.addStamp(QStringLiteral("x"), a));
    EXPECT_EQ(lib.count(), 1);
    ASSERT_TRUE(lib.addStamp(QStringLiteral("x"), b));
    EXPECT_EQ(lib.count(), 1) << "Same name must replace, not append";
    EXPECT_EQ(lib.stamp(QStringLiteral("x")).width(), 16);
}

TEST(ImageStampLibrary, SaveAndLoadFromSettingsRoundTrip)
{
    // ISOLATION (meta-audit follow-up F28). This test used to save priorOrg /
    // priorApp into locals and restore them at the BOTTOM of the test body.
    // That is not a restore, it is a restore-if-nothing-goes-wrong: the body
    // below contains ASSERT_TRUE / ASSERT_EQ, and a failing ASSERT_* returns
    // from the test function immediately, jumping over the two trailing
    // setOrganizationName/setApplicationName calls. The very first failure of
    // this test would therefore have left "PDFE_ImageStampTest" /
    // "ImageStampLibraryTest" installed for every later test in the process.
    //
    // It also called QSettings::setDefaultFormat(IniFormat) and NEVER put that
    // back on any path, success or failure — that one is process-global too,
    // and it silently moved every subsequent default QSettings() in the binary
    // off the registry and into an INI file.
    //
    // pdfe_test::SettingsScope is RAII, so both are restored on every exit
    // path, and purgeOnExit deletes the test-only hive so it cannot outlive
    // the process.
    pdfe_test::SettingsScope settingsScope(
        QStringLiteral("PDFE_ImageStampTest"),
        QStringLiteral("ImageStampLibraryTest"),
        /*purgeOnExit=*/true);
    QSettings::setDefaultFormat(QSettings::IniFormat);
    {
        QSettings cleanup;
        cleanup.clear();
    }

    {
        ImageStampLibrary writer;
        ASSERT_TRUE(writer.addStamp(QStringLiteral("logo"),
                                    makeTestImage(16, 16)));
        ASSERT_TRUE(writer.addStamp(QStringLiteral("seal"),
                                    makeTestImage(8, 8)));
        writer.saveToSettings();
    }

    ImageStampLibrary reader;
    EXPECT_EQ(reader.count(), 0);
    reader.loadFromSettings();
    EXPECT_EQ(reader.count(), 2)
        << "Both saved stamps must round-trip through QSettings";
    EXPECT_TRUE(reader.hasStamp(QStringLiteral("logo")));
    EXPECT_TRUE(reader.hasStamp(QStringLiteral("seal")));
    QImage logo = reader.stamp(QStringLiteral("logo"));
    EXPECT_FALSE(logo.isNull());
    EXPECT_EQ(logo.width(), 16);
    EXPECT_EQ(logo.height(), 16);
    // Insertion order is preserved by stampNames().
    QStringList names = reader.stampNames();
    ASSERT_EQ(names.size(), 2);
    EXPECT_EQ(names[0], QStringLiteral("logo"));
    EXPECT_EQ(names[1], QStringLiteral("seal"));

    // Cleanup the test's QSettings. The org/app names and the default format
    // are restored by ~SettingsScope on EVERY exit path, including the early
    // return an ASSERT_* failure above would take.
    {
        QSettings cleanup;
        cleanup.clear();
    }
}

TEST(ImageStampLibrary, RemoveStamp)
{
    ImageStampLibrary lib;
    lib.addStamp(QStringLiteral("a"), makeTestImage());
    lib.addStamp(QStringLiteral("b"), makeTestImage());
    EXPECT_EQ(lib.count(), 2);
    EXPECT_TRUE(lib.removeStamp(QStringLiteral("a")));
    EXPECT_FALSE(lib.removeStamp(QStringLiteral("a"))) << "second remove no-op";
    EXPECT_EQ(lib.count(), 1);
    EXPECT_FALSE(lib.hasStamp(QStringLiteral("a")));
    EXPECT_TRUE(lib.hasStamp(QStringLiteral("b")));
}

// ---------------- Placement on page writes a Form XObject ----------------

class ImageStampPlacementTest : public ::testing::Test {
protected:
    void SetUp() override {
        ctx = std::make_shared<MuPdfContext>();
        auto fixtures = std::filesystem::path(TEST_FIXTURES_DIR);
        auto src = fixtures / "test_single.pdf";
        if (!std::filesystem::exists(src)) {
            GTEST_SKIP() << "test_single.pdf not available";
        }
        doc = Document::open(ctx, src);
        ASSERT_NE(doc, nullptr);
        page = doc->page(0);
        ASSERT_NE(page, nullptr);
        mgr = std::make_unique<AnnotationManager>(ctx);
    }
    std::shared_ptr<MuPdfContext> ctx;
    std::unique_ptr<Document> doc;
    std::unique_ptr<Page> page;
    std::unique_ptr<AnnotationManager> mgr;
};

TEST_F(ImageStampPlacementTest, PlaceImageStampRendersImageInAnnotationRect)
{
    // Place a pure-red stamp on a white page; the rendered annotation
    // rect must come back red-dominant. This proves the appearance
    // stream wrote an Image XObject inside a Form XObject — the only
    // way pure pixel color from the source image surfaces in the
    // rendered output is via /AP/N + Image XObject (PDF spec
    // 12.5.5 "Appearance Streams").
    QImage img = makeTestImage(40, 20);
    QRectF rect(72, 72, 200, 100);

    AnnotationData data = mgr->createAnnotation(
        page.get(), AnnotationType::Stamp, rect);
    ASSERT_GE(data.annotIndex, 0);

    // Render the original (unstamped) page to establish a baseline:
    // the test fixture has no red pixels.
    QImage baseline = page->render(QRectF(), 1.0f);
    ASSERT_FALSE(baseline.isNull());
    const int baselineRed = countRedPixels(baseline);

    ASSERT_NO_THROW(mgr->setStampImageAppearance(
        page.get(), data.annotIndex, img));

    // Re-render the page with the stamp applied; the stamp's pixels
    // are pure red, so the rendered page should now contain a chunk
    // of red pixels roughly the size of the annotation rect (200x100
    // pt at 1.0 scale = 20000 px, allow >> 1000 to absorb any
    // anti-aliasing or background bleed).
    QImage withStamp = page->render(QRectF(), 1.0f);
    ASSERT_FALSE(withStamp.isNull());
    const int afterRed = countRedPixels(withStamp);

    EXPECT_GT(afterRed, baselineRed + 1000)
        << "Stamp must add a substantial red region to the rendered page; "
        << "baseline=" << baselineRed << " after=" << afterRed;
}

TEST_F(ImageStampPlacementTest, SaveAndReopenPreservesImageStampAppearance)
{
    QImage img = makeTestImage(24, 24);
    QRectF rect(50, 50, 120, 120);

    AnnotationData data = mgr->createAnnotation(
        page.get(), AnnotationType::Stamp, rect);
    mgr->setContents(page.get(), data.annotIndex, std::string("logo"));
    mgr->setStampImageAppearance(page.get(), data.annotIndex, img);

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    std::string outPath = dir.filePath(
        QStringLiteral("image_stamp_round_trip.pdf")).toStdString();
    ASSERT_NO_THROW(doc->saveAs(outPath));

    // Drop the original document and reopen the saved file.
    page.reset();
    mgr.reset();
    doc.reset();

    auto reopened = Document::open(ctx, outPath);
    ASSERT_NE(reopened, nullptr);
    auto reopenedPage = reopened->page(0);
    ASSERT_NE(reopenedPage, nullptr);
    auto reopenedMgr = std::make_unique<AnnotationManager>(ctx);

    auto annots = reopenedMgr->getAnnotations(reopenedPage.get());
    int stampIdx = -1;
    QRectF stampRect;
    for (const auto& a : annots) {
        if (a.type == AnnotationType::Stamp) {
            stampIdx = a.annotIndex;
            stampRect = a.rect;
            break;
        }
    }
    ASSERT_GE(stampIdx, 0) << "No /Stamp annotation found in reopened doc";

    (void)stampRect;
    // Round-trip preservation check: render the reopened page and
    // verify the red stamp pixels are still there. If the appearance
    // was lost during save (or stripped on reopen), the area would
    // render as the page's white background and we would see zero red.
    QImage rendered = reopenedPage->render(QRectF(), 1.0f);
    ASSERT_FALSE(rendered.isNull());
    const int redPixels = countRedPixels(rendered);
    EXPECT_GT(redPixels, 1000)
        << "Image stamp appearance must survive save+reopen; "
        << "found " << redPixels << " red pixels";

    // Contents (used to record the library entry name) survives too.
    auto reread = reopenedMgr->getAnnotation(reopenedPage.get(), stampIdx);
    EXPECT_EQ(reread.contents, std::string("logo"));
}

TEST_F(ImageStampPlacementTest, RejectsNullImage)
{
    QRectF rect(72, 72, 100, 100);
    AnnotationData data = mgr->createAnnotation(
        page.get(), AnnotationType::Stamp, rect);
    EXPECT_THROW(
        mgr->setStampImageAppearance(page.get(), data.annotIndex, QImage()),
        std::invalid_argument);
}

// ---------------- StampTool image-mode flag ----------------

TEST(StampToolImageMode, IsImageStampReflectsState)
{
    auto ctx = std::make_shared<MuPdfContext>();
    AnnotationManager mgr(ctx);
    StampTool tool(&mgr);
    EXPECT_FALSE(tool.isImageStamp());

    QImage img = makeTestImage(8, 8);
    tool.setImageStamp(QStringLiteral("logo"), img);
    EXPECT_TRUE(tool.isImageStamp());
    EXPECT_EQ(tool.stampName(), QStringLiteral("logo"));
    EXPECT_FALSE(tool.stampImage().isNull());

    tool.setImageStamp(QString(), QImage());
    EXPECT_FALSE(tool.isImageStamp());
}
