// -*- coding: utf-8 -*-
// Create PDF from Images wizard — verifies writePdf() produces a
// valid PDF whose page count matches the input image count and whose
// page sizes match each source image's pixel dimensions.
#include <gtest/gtest.h>
#include "ui/ImageToPdfWizard.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"

#include <QImage>
#include <QImageReader>
#include <QSize>
#include <QString>
#include <QTemporaryDir>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>

class ImageToPdfWizardTest : public ::testing::Test {
protected:
    void SetUp() override {
        tempDir = std::make_unique<QTemporaryDir>();
        ASSERT_TRUE(tempDir->isValid());
        dir = std::filesystem::path(tempDir->path().toStdString());
    }
    QStringList writeImage(const char* name, int w, int h, QColor color) {
        QImage img(w, h, QImage::Format_RGB888);
        img.fill(color);
        QString p = tempDir->path() + "/" + name;
        if (!img.save(p, "PNG")) return {};
        return QStringList{p};
    }
    std::unique_ptr<QTemporaryDir> tempDir;
    std::filesystem::path dir;
};

TEST_F(ImageToPdfWizardTest, SupportedExtensionsListedNonEmpty)
{
    const QStringList exts = pdfe::ui::ImageToPdfWizard::supportedImageExtensions();
    EXPECT_FALSE(exts.isEmpty());
    EXPECT_TRUE(exts.contains("png"));
    EXPECT_TRUE(exts.contains("jpg"));
    EXPECT_TRUE(exts.contains("tiff"));
}

TEST_F(ImageToPdfWizardTest, FilterStringIncludesAllExtensions)
{
    const QString filter = pdfe::ui::ImageToPdfWizard::imageFilterString();
    for (const QString& ext : pdfe::ui::ImageToPdfWizard::supportedImageExtensions()) {
        EXPECT_TRUE(filter.contains("*." + ext)) << ext.toStdString();
    }
    EXPECT_TRUE(filter.contains("All Files"));
}

TEST_F(ImageToPdfWizardTest, EmptyImageListReturnsFalse)
{
    const auto out = dir / "out.pdf";
    EXPECT_FALSE(pdfe::ui::ImageToPdfWizard::writePdf({}, out));
    EXPECT_FALSE(std::filesystem::exists(out));
}

TEST_F(ImageToPdfWizardTest, SingleImageProducesOnePagePdf)
{
    QStringList imgs = writeImage("a.png", 300, 200, Qt::red);
    ASSERT_FALSE(imgs.isEmpty());

    const auto out = dir / "one_page.pdf";
    ASSERT_TRUE(pdfe::ui::ImageToPdfWizard::writePdf(imgs, out));
    ASSERT_TRUE(std::filesystem::exists(out));

    auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
    auto doc = pdfe::core::Document::open(ctx, out);
    ASSERT_NE(doc, nullptr);
    EXPECT_EQ(doc->pageCount(), 1);
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);
    // Page size should match image at 72 DPI → width 300 pt, height 200 pt.
    const auto info = doc->pageInfo(0);
    EXPECT_NEAR(info.widthPt,  300.0f, 1.5f);
    EXPECT_NEAR(info.heightPt, 200.0f, 1.5f);
}

TEST_F(ImageToPdfWizardTest, MultipleImagesProduceMultiPagePdf)
{
    QStringList all;
    all += writeImage("a.png", 200, 100, Qt::red);
    all += writeImage("b.png", 400, 300, Qt::green);
    all += writeImage("c.png", 150, 250, Qt::blue);
    ASSERT_EQ(all.size(), 3);

    const auto out = dir / "multi_page.pdf";
    ASSERT_TRUE(pdfe::ui::ImageToPdfWizard::writePdf(all, out));

    auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
    auto doc = pdfe::core::Document::open(ctx, out);
    ASSERT_NE(doc, nullptr);
    EXPECT_EQ(doc->pageCount(), 3);

    const auto p0 = doc->pageInfo(0);
    const auto p1 = doc->pageInfo(1);
    const auto p2 = doc->pageInfo(2);
    EXPECT_NEAR(p0.widthPt,  200.0f, 1.5f);
    EXPECT_NEAR(p0.heightPt, 100.0f, 1.5f);
    EXPECT_NEAR(p1.widthPt,  400.0f, 1.5f);
    EXPECT_NEAR(p1.heightPt, 300.0f, 1.5f);
    EXPECT_NEAR(p2.widthPt,  150.0f, 1.5f);
    EXPECT_NEAR(p2.heightPt, 250.0f, 1.5f);
}

TEST_F(ImageToPdfWizardTest, UnreadableImageSkippedWithValidOnesWritten)
{
    QStringList imgs;
    imgs += writeImage("real.png", 100, 100, Qt::black);
    imgs += QStringList{tempDir->path() + "/does-not-exist.png"};
    ASSERT_EQ(imgs.size(), 2);

    const auto out = dir / "skip.pdf";
    ASSERT_TRUE(pdfe::ui::ImageToPdfWizard::writePdf(imgs, out));

    auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
    auto doc = pdfe::core::Document::open(ctx, out);
    ASSERT_NE(doc, nullptr);
    EXPECT_EQ(doc->pageCount(), 1);  // only the real image produced a page
}

TEST_F(ImageToPdfWizardTest, AllImagesUnreadableReturnsFalse)
{
    const QStringList imgs{tempDir->path() + "/nope.png"};
    const auto out = dir / "empty_out.pdf";
    EXPECT_FALSE(pdfe::ui::ImageToPdfWizard::writePdf(imgs, out));
}

// =====================================================================
// Gap A1-31 — boundary cases (corrupt / oversized / zero images)
// =====================================================================
//
// Everything above this line is happy path plus the "file does not exist"
// case. A missing file is the EASY negative: QImage never opens anything.
// The three cases below are the ones the audit row named, and each has a
// documented contract in src/ui/ImageToPdfWizard.h:
//
//   "`images` is a list of image file paths; each one that loads becomes a
//    page in the output PDF. Returns true when at least one image was
//    successfully written."
//
// So the promised behaviour is: an image that does NOT load contributes no
// page; if none load, writePdf() returns false. Returning false means
// nothing was written — which must include leaving no orphan output file
// behind, because runInteractive() reports "nothing written" to the user on
// exactly that return value (ImageToPdfWizard.cpp:91-95). A 0-byte or
// 0-page .pdf sitting at the user's chosen path after that message is a
// broken promise, and it is invisible to a bare `EXPECT_FALSE(writePdf(...))`.

namespace {

// Fixture paths are asserted present, never skipped: these two files are
// committed and regenerated by tests/fixtures/gen_image_wizard_boundary.py.
std::filesystem::path boundaryFixture(const char* name)
{
    return std::filesystem::path(TEST_FIXTURES_DIR) / name;
}

// Count the entries in `d`. Used to prove a failed writePdf() leaves no
// orphan output AND no stray sibling (temp/partial) file.
std::size_t entryCount(const std::filesystem::path& d)
{
    std::size_t n = 0;
    for (auto it = std::filesystem::directory_iterator(d);
         it != std::filesystem::directory_iterator(); ++it) {
        ++n;
    }
    return n;
}

} // namespace

// (a) CORRUPT INPUT — a file that is present, has a valid PNG signature and a
// valid CRC-correct IHDR, but whose deflate stream is truncated. Nothing can
// decode it. Contract: it does not load -> no page -> writePdf false -> no
// output artifact of any kind.
//
// RED / mutation this kills:
//   * delete the `if (img.isNull()) continue;` guard in
//     ImageToPdfWizard::writePdf -> a null QImage yields width()/height() 0,
//     the std::max(1, ...) clamp turns it into a 1x1 page, `any` becomes true
//     and writePdf returns TRUE with a junk 1pt page. EXPECT_FALSE fires.
//   * move `QPdfWriter writer(...)` construction to eagerly create/open the
//     output file (or add an unconditional touch of outPath) -> the
//     no-orphan-file assertions fire.
TEST_F(ImageToPdfWizardTest, CorruptImageRejected)
{
    const auto corrupt = boundaryFixture("image_wizard_corrupt.png");
    ASSERT_TRUE(std::filesystem::exists(corrupt))
        << "committed fixture missing; regenerate with "
           "python tests/fixtures/gen_image_wizard_boundary.py";

    // The fixture must really be undecodable — otherwise this test would be
    // silently re-testing the happy path.
    ASSERT_TRUE(QImage(QString::fromStdString(corrupt.string())).isNull())
        << "fixture image_wizard_corrupt.png decoded; it is supposed to be a "
           "truncated PNG";

    const auto out = dir / "corrupt_only.pdf";
    const std::size_t before = entryCount(dir);

    EXPECT_FALSE(pdfe::ui::ImageToPdfWizard::writePdf(
        QStringList{QString::fromStdString(corrupt.string())}, out))
        << "a corrupt image must not count as a written page";

    EXPECT_FALSE(std::filesystem::exists(out))
        << "writePdf returned false but left an output file at " << out.string()
        << " — runInteractive() tells the user 'nothing written' on this path";
    EXPECT_EQ(entryCount(dir), before)
        << "writePdf failed but left a stray file in the output directory";
}

// A corrupt image mixed with a good one must be SKIPPED, not fatal, and must
// not shift the surviving page's geometry.
//
// RED / mutation this kills: replace `continue` with `return false` on a null
// image (fail-fast instead of skip) -> the ASSERT_TRUE fires. Drop the
// per-image page-size assignment -> the authored 300x200 page-size check fires
// (the surviving page would inherit the default A4/Letter layout).
TEST_F(ImageToPdfWizardTest, CorruptImageSkippedGoodImageStillWritten)
{
    const auto corrupt = boundaryFixture("image_wizard_corrupt.png");
    ASSERT_TRUE(std::filesystem::exists(corrupt));

    QStringList imgs;
    imgs += QString::fromStdString(corrupt.string());
    imgs += writeImage("good.png", 300, 200, Qt::magenta);
    ASSERT_EQ(imgs.size(), 2);

    const auto out = dir / "corrupt_plus_good.pdf";
    ASSERT_TRUE(pdfe::ui::ImageToPdfWizard::writePdf(imgs, out))
        << "one decodable image among corrupt ones must still produce a PDF";
    ASSERT_TRUE(std::filesystem::exists(out));

    auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
    auto doc = pdfe::core::Document::open(ctx, out);
    ASSERT_NE(doc, nullptr);
    EXPECT_EQ(doc->pageCount(), 1)
        << "the corrupt image must contribute no page";
    const auto info = doc->pageInfo(0);
    // Authored ground truth: the surviving image is 300x200 px, mapped 1px=1pt.
    EXPECT_NEAR(info.widthPt,  300.0f, 1.5f);
    EXPECT_NEAR(info.heightPt, 200.0f, 1.5f);
}

// (b) OVERSIZED INPUT — a PNG whose IHDR declares 65535x65535 RGBA (~17 GB of
// pixels) in a 69-byte file. Qt's PNG handler sizes the destination QImage
// from the IHDR via QImageIOHandler::allocateImage(), which enforces
// QImageReader::allocationLimit(), so the decode must be refused before any
// giant allocation. From writePdf()'s point of view that is just "does not
// load" -> no page -> false -> no output artifact.
//
// RED / mutation this kills:
//   * `QImageReader::setAllocationLimit(0)` (limit disabled) anywhere in
//     product start-up, or a switch to a raw decode path that ignores the
//     limit -> the image would load (or the process would die trying) and
//     EXPECT_FALSE / the allocation-limit assertion fires.
//   * removal of the `img.isNull()` guard -> a 1x1 junk page, as above.
TEST_F(ImageToPdfWizardTest, OversizedImageRejected)
{
    const auto oversized = boundaryFixture("image_wizard_oversized.png");
    ASSERT_TRUE(std::filesystem::exists(oversized))
        << "committed fixture missing; regenerate with "
           "python tests/fixtures/gen_image_wizard_boundary.py";
    // The whole point of the fixture is that the declared size is enormous
    // while the file is tiny — assert both halves so a regenerated fixture
    // cannot quietly become a small valid image.
    EXPECT_LT(std::filesystem::file_size(oversized), std::uintmax_t{4096});

    QImageReader reader(QString::fromStdString(oversized.string()));
    EXPECT_EQ(reader.size(), QSize(65535, 65535))
        << "fixture no longer declares oversized dimensions";
    EXPECT_GT(QImageReader::allocationLimit(), 0)
        << "QImageReader's allocation limit is the ONLY thing standing between "
           "a 69-byte file and a 17 GB allocation on this code path; the wizard "
           "performs no size check of its own";

    const auto out = dir / "oversized_only.pdf";
    const std::size_t before = entryCount(dir);

    EXPECT_FALSE(pdfe::ui::ImageToPdfWizard::writePdf(
        QStringList{QString::fromStdString(oversized.string())}, out))
        << "an image that cannot be allocated must not count as a written page";
    EXPECT_FALSE(std::filesystem::exists(out))
        << "writePdf returned false but left an output file behind";
    EXPECT_EQ(entryCount(dir), before)
        << "writePdf failed but left a stray file in the output directory";
}

// An oversized declaration mixed with a good image: skip, don't abort, and do
// not emit a page carrying the bogus dimensions.
TEST_F(ImageToPdfWizardTest, OversizedImageSkippedGoodImageStillWritten)
{
    const auto oversized = boundaryFixture("image_wizard_oversized.png");
    ASSERT_TRUE(std::filesystem::exists(oversized));

    QStringList imgs;
    imgs += QString::fromStdString(oversized.string());
    imgs += writeImage("ok.png", 120, 340, Qt::darkCyan);
    ASSERT_EQ(imgs.size(), 2);

    const auto out = dir / "oversized_plus_good.pdf";
    ASSERT_TRUE(pdfe::ui::ImageToPdfWizard::writePdf(imgs, out));

    auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
    auto doc = pdfe::core::Document::open(ctx, out);
    ASSERT_NE(doc, nullptr);
    EXPECT_EQ(doc->pageCount(), 1)
        << "the oversized-declaration image must contribute no page";
    const auto info = doc->pageInfo(0);
    EXPECT_NEAR(info.widthPt,  120.0f, 1.5f);
    EXPECT_NEAR(info.heightPt, 340.0f, 1.5f);
}

// (b2) OVERSIZED IN THE OTHER DIRECTION — an image that decodes perfectly well
// (1,000,000 px, ~4 MB, far under any allocation limit) but whose PIXEL COUNT
// is spread over one enormous axis. writePdf maps 1 px -> 1 pt with no cap:
//
//     const int wPt = std::max(1, img.width());
//     QPageSize pageSize(QSizeF(wPt, hPt), QPageSize::Point);
//
// PDF's architectural limits (ISO 32000-1 / PDF 1.7 Annex C.2) cap a page at
// 14400 user-space units (200 inches) per side; Acrobat refuses to open a
// document that exceeds it. The ONLY documented clamp in writePdf is the
// LOWER one (std::max(1, ...)) — there is no upper clamp at all, so a
// 20000 px wide input asks for a 20000 pt page.
//
// This asserts the CORRECT behaviour (spec ground truth, not current output):
// whatever the wizard emits must be a page a conforming reader can open.
// Acceptable implementations: clamp the page to 14400 and scale the image
// down, split the image, or refuse the image. Emitting an out-of-spec page is
// not acceptable.
//
// RED / mutation: none needed — this is a real-defect probe. It also kills any
// future change that removes a clamp once one is added.
TEST_F(ImageToPdfWizardTest, ExtremeAspectImageDoesNotExceedPdfPageLimit)
{
    // 20000 x 50 = 1e6 px. Cheap to allocate; catastrophic as a page size.
    QImage wide(20000, 50, QImage::Format_RGB888);
    wide.fill(Qt::yellow);
    const QString wpath = tempDir->path() + "/extreme_aspect.png";
    ASSERT_TRUE(wide.save(wpath, "PNG"));
    ASSERT_FALSE(QImage(wpath).isNull())
        << "this input is supposed to DECODE — the boundary under test is the "
           "page-size mapping, not the decoder";

    const auto out = dir / "extreme_aspect.pdf";
    ASSERT_TRUE(pdfe::ui::ImageToPdfWizard::writePdf(QStringList{wpath}, out));
    ASSERT_TRUE(std::filesystem::exists(out));

    auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
    auto doc = pdfe::core::Document::open(ctx, out);
    ASSERT_NE(doc, nullptr);
    ASSERT_EQ(doc->pageCount(), 1);
    const auto info = doc->pageInfo(0);

    // ISO 32000-1 Annex C.2: 14400 units maximum per side.
    constexpr float kPdfMaxPageUnits = 14400.0f;
    EXPECT_LE(info.widthPt, kPdfMaxPageUnits)
        << "emitted a " << info.widthPt << " pt wide page from a 20000 px image; "
           "PDF's architectural limit is 14400 units and conforming readers "
           "reject anything larger. writePdf clamps only the LOWER bound "
           "(std::max(1, img.width())) and applies no upper clamp.";
    EXPECT_LE(info.heightPt, kPdfMaxPageUnits)
        << "emitted a " << info.heightPt << " pt tall page";
    EXPECT_GT(info.widthPt, 0.0f);
    EXPECT_GT(info.heightPt, 0.0f);
}

// (c) ZERO IMAGES — the degenerate list. writePdf() must produce NO document:
// not an empty one, not a 0-byte one, not a stray temp file.
//
// RED / mutation this kills: delete `if (images.isEmpty()) return false;` ->
// the loop body never runs, `any` stays false and the return value is still
// false, BUT QPdfWriter has now been constructed against outPath. Any change
// that makes QPdfWriter (or a future replacement writer) touch the file on
// construction turns that into an orphan artifact at the path the user chose,
// immediately after being told nothing was written. Only the file-system
// assertions below can see it.
TEST_F(ImageToPdfWizardTest, ZeroImagesProducesNoDocument)
{
    const auto out = dir / "zero_images.pdf";
    const std::size_t before = entryCount(dir);

    EXPECT_FALSE(pdfe::ui::ImageToPdfWizard::writePdf(QStringList{}, out));

    EXPECT_FALSE(std::filesystem::exists(out))
        << "an empty image list must not create " << out.string();
    EXPECT_EQ(entryCount(dir), before)
        << "an empty image list created " << (entryCount(dir) - before)
        << " file(s) in the output directory";

    // Same for a list of empty strings — a plausible shape for a file dialog
    // that returned placeholder entries. Still no document.
    const auto out2 = dir / "zero_images_blank_paths.pdf";
    EXPECT_FALSE(pdfe::ui::ImageToPdfWizard::writePdf(QStringList{QString(), QString("")}, out2));
    EXPECT_FALSE(std::filesystem::exists(out2));
    EXPECT_EQ(entryCount(dir), before)
        << "blank path entries created a stray file";
}
