// -*- coding: utf-8 -*-
//
// Meta-audit follow-up F12 — WIN-23 (High) and WIN-25 (Low).
//
// THE GAP (WIN-23)
// ----------------
// PDFE's copy-out is a headline feature and five places in src/ assert IN PROSE
// that "QClipboard::setImage exposes the image as CF_DIB / CF_BITMAP"
// (src/ui/MainWindow.cpp:7134,10777; MainWindow.h:445; PagePanel.cpp:26;
// PagePanel.h:70). Nothing verified it. Every image-side clipboard read in
// tests/ went back through QClipboard — an internal round-trip that would still
// pass if Qt published nothing a foreign application could read, which is the
// user-visible failure (paste into Word/Paint does nothing).
//
// These tests ask the WINDOWS clipboard, through the Win32 API, what formats a
// PDFE copy actually published: OpenClipboard / EnumClipboardFormats /
// GetClipboardData(CF_DIB). CF_DIB is the cross-application bitmap contract —
// it is what Word, Paint, Excel and Explorer consume. The DIB header is then
// decoded and its pixel dimensions compared with the source, so the test proves
// the bytes are a real bitmap, not merely that a format tag was advertised.
//
// THE GAP (WIN-25)
// ----------------
// The clipboard image fixtures were all small selections. A full page at 300 DPI
// is a ~30 MB DIB and exercises allocation/marshalling paths the small fixtures
// never reach. LargePageRasterSurvivesTheWindowsClipboard renders a REAL page at
// 300 DPI and asserts the DIB that lands on the Windows clipboard has exactly
// the source dimensions.
//
// CLEANUP / INTERFERENCE
// ----------------------
// Clipboard tests interfere with each other and with the rest of the suite.
// Every OpenClipboard here is paired with CloseClipboard by an RAII holder that
// runs on every exit path including a fatal ASSERT; the fixture clears the
// clipboard in TearDown; and the Win32 open is retried, because under an RDP
// session clipboard ownership transiently flips to the session's clipboard
// monitor (rdpclip) and a single OpenClipboard can legitimately fail.

#include <gtest/gtest.h>

#include <QApplication>
#include <QClipboard>
#include <QCoreApplication>
#include <QEventLoop>
#include <QImage>
#include <QRectF>
#include <QThread>

#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "ui/MainWindow.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using namespace pdfe::ui;

#ifdef _WIN32

namespace {

std::filesystem::path fixturesDir()
{
    return std::filesystem::path(TEST_FIXTURES_DIR);
}

// RAII OpenClipboard/CloseClipboard. Opening is retried because RDP sessions
// hand clipboard ownership back and forth with rdpclip.exe.
class ClipboardHolder {
public:
    explicit ClipboardHolder(int attempts = 40, int delayMs = 25)
    {
        for (int i = 0; i < attempts && !open_; ++i) {
            if (::OpenClipboard(nullptr)) {
                open_ = true;
                break;
            }
            lastError_ = ::GetLastError();
            QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, delayMs);
            QThread::msleep(delayMs);
        }
    }
    ~ClipboardHolder()
    {
        if (open_)
            ::CloseClipboard();
    }
    ClipboardHolder(const ClipboardHolder&) = delete;
    ClipboardHolder& operator=(const ClipboardHolder&) = delete;

    bool isOpen() const { return open_; }
    DWORD lastError() const { return lastError_; }

private:
    bool open_ = false;
    DWORD lastError_ = 0;
};

std::vector<UINT> enumerateClipboardFormats()
{
    std::vector<UINT> formats;
    UINT fmt = 0;
    while ((fmt = ::EnumClipboardFormats(fmt)) != 0)
        formats.push_back(fmt);
    return formats;
}

bool hasFormat(const std::vector<UINT>& formats, UINT want)
{
    for (UINT f : formats)
        if (f == want)
            return true;
    return false;
}

std::string describeFormats(const std::vector<UINT>& formats)
{
    std::string out;
    for (UINT f : formats) {
        wchar_t name[256] = {0};
        std::string label = std::to_string(f);
        if (::GetClipboardFormatNameW(f, name, 255) > 0) {
            char narrow[512] = {0};
            ::WideCharToMultiByte(CP_UTF8, 0, name, -1, narrow, sizeof(narrow) - 1,
                                  nullptr, nullptr);
            label += "(" + std::string(narrow) + ")";
        } else {
            switch (f) {
            case CF_TEXT: label += "(CF_TEXT)"; break;
            case CF_BITMAP: label += "(CF_BITMAP)"; break;
            case CF_DIB: label += "(CF_DIB)"; break;
            case CF_DIBV5: label += "(CF_DIBV5)"; break;
            case CF_UNICODETEXT: label += "(CF_UNICODETEXT)"; break;
            case CF_HDROP: label += "(CF_HDROP)"; break;
            default: break;
            }
        }
        if (!out.empty())
            out += ", ";
        out += label;
    }
    return out.empty() ? std::string("<none>") : out;
}

struct DibGeometry {
    bool valid = false;
    long width = 0;
    long height = 0;   // absolute value; DIBs may be bottom-up (negative height)
    int bitCount = 0;
    size_t bytes = 0;
};

// Read the BITMAPINFOHEADER off whichever DIB format is on the clipboard.
// The clipboard must already be open.
DibGeometry readDibGeometry(UINT format)
{
    DibGeometry g;
    HANDLE h = ::GetClipboardData(format);
    if (!h)
        return g;
    const SIZE_T total = ::GlobalSize(h);
    void* p = ::GlobalLock(h);
    if (!p)
        return g;
    if (total >= sizeof(BITMAPINFOHEADER)) {
        const BITMAPINFOHEADER* bih = static_cast<const BITMAPINFOHEADER*>(p);
        g.valid = true;
        g.width = bih->biWidth;
        g.height = bih->biHeight < 0 ? -bih->biHeight : bih->biHeight;
        g.bitCount = bih->biBitCount;
        g.bytes = static_cast<size_t>(total);
    }
    ::GlobalUnlock(h);
    return g;
}

// Write a QImage to the clipboard and confirm it landed (RDP-safe), mirroring
// the helper in tests/test_ui_clipboard_dragdrop.cpp.
bool rdpSafeSetClipboardImage(QClipboard* cb, const QImage& img,
                              int maxAttempts = 8, int delayMs = 30)
{
    if (!cb || img.isNull())
        return false;
    for (int i = 0; i < maxAttempts; ++i) {
        cb->setImage(img);
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, delayMs);
        if (!cb->image().isNull())
            return true;
        QThread::msleep(delayMs);
    }
    return false;
}

} // namespace

class ClipboardNativeFormatsTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        pdf_ = fixturesDir() / "test_3page.pdf";
        ASSERT_TRUE(std::filesystem::exists(pdf_))
            << "test_3page.pdf is a configure-time required fixture";
    }

    void TearDown() override
    {
        mw_.reset();
        if (auto* cb = QApplication::clipboard())
            cb->clear();
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 20);
    }

    std::unique_ptr<MainWindow> mw_;
    std::filesystem::path pdf_;
};

// ─────────────────────────────────────────────────────────────────────────
// WIN-23 — after a PDFE copy-out, the WINDOWS clipboard carries a genuine
// cross-application bitmap format.
//
// This is the check the existing suite could not make: EnumClipboardFormats is
// the OS's own answer to "what can another application paste?".
//
// RED / mutation: replace MainWindow::copySelectionAsImage's
// QClipboard::setImage with setText (or drop the image branch of onCopy) and
// CF_DIB/CF_BITMAP vanish from the enumeration → this fails, while the
// pre-existing QClipboard-only tests would keep passing for the text path.
// ─────────────────────────────────────────────────────────────────────────
TEST_F(ClipboardNativeFormatsTest, CopyOutPublishesCrossApplicationBitmapFormat)
{
    mw_ = std::make_unique<MainWindow>();
    mw_->openFile(QString::fromStdString(pdf_.string()));
    QCoreApplication::processEvents();
    ASSERT_NE(mw_->documentForTesting(), nullptr) << "fixture must open";

    QClipboard* cb = QApplication::clipboard();
    ASSERT_NE(cb, nullptr);
    cb->clear();
    QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 20);

    // Select an 80x60 pt region on page 0 and run the production copy path.
    const QRectF sel(100.0, 100.0, 80.0, 60.0);
    mw_->setCopySelectionRect(0, sel);
    ASSERT_TRUE(mw_->hasCopySelection());
    mw_->copyForTesting();   // onCopy() → copySelectionAsImage() → setImage()
    QCoreApplication::processEvents();
    QThread::msleep(50);
    QCoreApplication::processEvents();

    ClipboardHolder clip;
    ASSERT_TRUE(clip.isOpen())
        << "could not open the Windows clipboard after 40 attempts (last error "
        << clip.lastError()
        << "). This is an environment condition, not a product verdict.";

    const std::vector<UINT> formats = enumerateClipboardFormats();
    ASSERT_FALSE(formats.empty())
        << "the Windows clipboard advertised no formats at all after a copy";

    const bool hasDib = hasFormat(formats, CF_DIB);
    const bool hasDibV5 = hasFormat(formats, CF_DIBV5);
    const bool hasBitmap = hasFormat(formats, CF_BITMAP);

    EXPECT_TRUE(hasDib || hasDibV5 || hasBitmap)
        << "after a page-region copy the Windows clipboard must carry a "
           "cross-application bitmap format (CF_DIB / CF_DIBV5 / CF_BITMAP) so "
           "another application can paste the image. Advertised formats were: "
        << describeFormats(formats);

    // CF_DIB specifically is the format every classic paste target asks for.
    EXPECT_TRUE(hasDib)
        << "CF_DIB (8) must be present — it is the format Word/Paint/Excel "
           "request. Advertised formats: "
        << describeFormats(formats);

    if (hasDib) {
        const DibGeometry g = readDibGeometry(CF_DIB);
        EXPECT_TRUE(g.valid)
            << "CF_DIB was advertised but GetClipboardData produced no usable "
               "BITMAPINFOHEADER — an advertised-but-unrenderable format is the "
               "exact failure a QClipboard-only test cannot see";
        if (g.valid) {
            // copySelectionAsImage renders the selection at 2x, so ~160x120 px.
            EXPECT_NEAR(g.width, 160, 8)
                << "the DIB on the Windows clipboard must be the SELECTION, not "
                   "the whole page";
            EXPECT_NEAR(g.height, 120, 8)
                << "the DIB on the Windows clipboard must be the SELECTION, not "
                   "the whole page";
            EXPECT_GE(g.bitCount, 8)
                << "the published DIB must carry real colour depth";
            EXPECT_GT(g.bytes, static_cast<size_t>(sizeof(BITMAPINFOHEADER)))
                << "the published DIB must carry pixel data, not a bare header";
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────
// WIN-23 companion — the same enumeration for the TEXT side of a copy, so the
// bitmap assertion above cannot be satisfied by an unrelated stale clipboard
// state. A copy that publishes ONLY text is a real (and previously
// indistinguishable) regression shape.
//
// RED / mutation: if setImage were replaced by setText, the previous test would
// fail and this one would still pass — the pair localises the regression.
// ─────────────────────────────────────────────────────────────────────────
TEST_F(ClipboardNativeFormatsTest, PlainTextCopyPublishesUnicodeTextButNoBitmap)
{
    QClipboard* cb = QApplication::clipboard();
    ASSERT_NE(cb, nullptr);
    cb->clear();
    QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 20);
    cb->setText(QStringLiteral("PDFE WIN-23 control payload"));
    QCoreApplication::processEvents();
    QThread::msleep(50);
    QCoreApplication::processEvents();

    ClipboardHolder clip;
    ASSERT_TRUE(clip.isOpen())
        << "could not open the Windows clipboard (last error " << clip.lastError()
        << "). Environment condition, not a product verdict.";

    const std::vector<UINT> formats = enumerateClipboardFormats();
    EXPECT_TRUE(hasFormat(formats, CF_UNICODETEXT))
        << "a text copy must publish CF_UNICODETEXT. Advertised: "
        << describeFormats(formats);
    EXPECT_FALSE(hasFormat(formats, CF_DIB))
        << "a text-only copy must NOT leave a bitmap on the clipboard; if it "
           "does, the bitmap assertion in the copy-out test could be satisfied "
           "by stale state. Advertised: "
        << describeFormats(formats);
}

// ─────────────────────────────────────────────────────────────────────────
// WIN-25 — a full page rendered at 300 DPI survives the Windows clipboard.
//
// A letter page at 300 DPI is ~2550x3300 px (~32 MB as a 32-bit DIB), an order
// of magnitude past every previous clipboard fixture. The assertion is exact
// dimension equality on the DIB the OS holds — a truncated or silently
// downscaled transfer fails.
//
// RED / mutation: any allocation-failure path that silently drops the large
// payload leaves no CF_DIB (or one with the wrong dimensions) → this fails.
// ─────────────────────────────────────────────────────────────────────────
TEST_F(ClipboardNativeFormatsTest, LargePageRasterSurvivesTheWindowsClipboard)
{
    auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
    auto doc = pdfe::core::Document::open(ctx, pdf_);
    ASSERT_NE(doc, nullptr);
    ASSERT_GT(doc->pageCount(), 0);

    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);
    const QRectF box = page->boundingBox();

    // 300 DPI against a 72 DPI page coordinate space.
    const float scale300 = 300.0f / 72.0f;
    QImage big = page->render(box, scale300);
    ASSERT_FALSE(big.isNull()) << "300 DPI page render must succeed";
    ASSERT_GT(big.width(), 1500)
        << "a 300 DPI render of a letter-ish page must be > 1500 px wide; got "
        << big.width();
    ASSERT_GT(big.height(), 1500)
        << "a 300 DPI render of a letter-ish page must be > 1500 px tall; got "
        << big.height();

    QClipboard* cb = QApplication::clipboard();
    ASSERT_NE(cb, nullptr);
    cb->clear();
    QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 20);
    ASSERT_TRUE(rdpSafeSetClipboardImage(cb, big))
        << "the 300 DPI page raster (" << big.width() << "x" << big.height()
        << ") did not land on the clipboard — large-payload transfer failure";

    ClipboardHolder clip;
    ASSERT_TRUE(clip.isOpen())
        << "could not open the Windows clipboard (last error " << clip.lastError()
        << "). Environment condition, not a product verdict.";

    const std::vector<UINT> formats = enumerateClipboardFormats();
    ASSERT_TRUE(hasFormat(formats, CF_DIB))
        << "a large page raster must still be published as CF_DIB. Advertised: "
        << describeFormats(formats);

    const DibGeometry g = readDibGeometry(CF_DIB);
    ASSERT_TRUE(g.valid) << "the large CF_DIB must be renderable by the OS";
    EXPECT_EQ(g.width, static_cast<long>(big.width()))
        << "the clipboard DIB width must match the 300 DPI source exactly";
    EXPECT_EQ(g.height, static_cast<long>(big.height()))
        << "the clipboard DIB height must match the 300 DPI source exactly";
    EXPECT_GT(g.bytes, static_cast<size_t>(big.width()) *
                           static_cast<size_t>(big.height()))
        << "the clipboard DIB must carry at least one byte per source pixel; a "
           "smaller payload means the transfer was truncated";
}

#endif // _WIN32
