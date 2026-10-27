// -*- coding: utf-8 -*-
//
// R3-12 (clipboard image in/out) + R3-15 (drag-and-drop) regression suite.
//
// These tests post REAL Qt events / drive the REAL clipboard round-trips —
// they do NOT grep the source. They replace the source-string-grep drop test
// (Phase9ChromeTest.MainWindowAcceptsPdfFileDrop in test_chrome_phase9.cpp)
// with a genuine QDropEvent that must actually open a document.
//
// Coverage:
//   * DropOpensPdfDocument      — a real QDropEvent carrying a file:// URL to a
//                                 fixture PDF opens the document (R3-15 drop-in).
//   * DragEnterAcceptsPdfUrl     — a real QDragEnterEvent accepts a .pdf URL and
//                                 rejects a non-file payload (R3-15).
//   * CopySelectionOutAsImage    — copy honours the CURRENT selection: the
//                                 clipboard receives an IMAGE sized to the
//                                 selection, not the whole page (R3-12 copy-out,
//                                 CF_DIB/CF_BITMAP via QClipboard::setImage).
//   * PasteImageIntoPage         — a known QImage on the clipboard is pasted
//                                 INTO the page, inserting an image object
//                                 (R3-12 paste-in).
//   * PageDragOutCarriesImage    — the page-panel drag payload carries the page
//                                 thumbnail as image data + a text label
//                                 (R3-15 drag-out).
//
// MainWindow is heap-allocated (unique_ptr) — stack-allocating it overflows the
// test thread's CRT stack-frame guard (see test_main_window_q_invokable.cpp).
// QApplication is owned by test_main.cpp for the whole process, so qApp is
// always live inside test bodies.

#include <gtest/gtest.h>

#include <QApplication>
#include <QClipboard>
#include <QCoreApplication>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QEventLoop>
#include <QImage>
#include <QLineEdit>
#include <QMimeData>
#include <QPixmap>
#include <QThread>
#include <QUrl>

#include "ui/MainWindow.h"
#include "ui/PagePanel.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "support/ClipboardTestUtil.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <system_error>
#include <QTimer>
#include <QMimeData>
#include <functional>
#include <memory>
#include <vector>

using namespace pdfe::ui;

namespace {

std::filesystem::path fixturesDir() {
    return std::filesystem::path(TEST_FIXTURES_DIR);
}

// Clipboard helpers now live in tests/support/ClipboardTestUtil.h so the read
// and write retry budgets cannot drift apart between test files again (this
// file carried 6x20ms while test_mediabox_origin_crop.cpp carried 8x20ms).
using pdfe::testsupport::rdpSafeClipboardImage;
using pdfe::testsupport::rdpSafeSetClipboardImage;

// Pump the event loop until `pred()` is true or the budget elapses. openFile()
// itself is synchronous, but a dropped-file open runs through the full
// document-open pipeline (out-of-process preflight child + a deferred
// fitPage/layout singleShot), so a fixed single processEvents() is not a
// reliable settle point. Waiting on the real observable condition (document
// loaded) is deterministic and honest — not a skip.
template <typename Pred>
bool spinUntil(Pred pred, int budgetMs = 5000, int stepMs = 20) {
    for (int waited = 0; waited <= budgetMs; waited += stepMs) {
        if (pred()) return true;
        QCoreApplication::processEvents(
            QEventLoop::ExcludeUserInputEvents, stepMs);
        QThread::msleep(stepMs);
    }
    return pred();
}

} // namespace

class UiClipboardDragDropTest : public ::testing::Test {
protected:
    void SetUp() override {
        mw_ = std::make_unique<MainWindow>();
        pdf_ = fixturesDir() / "test_3page.pdf";
    }
    void TearDown() override {
        if (auto* cb = QApplication::clipboard()) cb->clear();

        // Balance any drag this test left open BEFORE the widget dies.
        //
        // These tests synthesise drag events straight into the widget with
        // QApplication::sendEvent, bypassing the platform DnD machinery
        // (IDropTarget -> QWindowsDrag) that normally guarantees every
        // dragEnter is followed by a dragLeave or a drop. A test that sends an
        // accepted dragEnter and then returns leaves QApplication::notify's
        // drag bookkeeping pointing at this widget; destroying it turns that
        // into a dangling pointer, and the NEXT test's drop crashes inside
        // Qt's own event routing before any PDFE handler runs:
        //
        //   Qt6Cored!QObject::isWidgetType          <- AV, rax=cdcdcdcd00000031
        //   Qt6Widgetsd!QApplication::notify
        //   Qt6Cored!QCoreApplication::sendEvent
        //   ...TestBody  test_ui_clipboard_dragdrop.cpp:684
        //
        // 0xCDCDCDCD is the debug CRT's uninitialised-heap marker. Measured
        // before this line existed: DragEnterAcceptsPdfUrl followed by
        // ARealDropStillWorksAfterAVirtualFileDrop crashed 7 of 10 runs, and
        // the pair was reported as two "transient flakes" because each test
        // passes when re-run alone.
        //
        // Sending the leave unconditionally is safe: Qt ignores it when no
        // drag is in progress.
        if (mw_) {
            QDragLeaveEvent leave;
            QApplication::sendEvent(mw_.get(), &leave);
        }

        mw_.reset();
        // Guarantee the annotation-pipe env used by the end-to-end test never
        // leaks into a later test's SetUp() (runs even after a fatal ASSERT).
        qunsetenv("PDFE_TEST_ANNOT_PIPES");
    }

    std::unique_ptr<MainWindow> mw_;
    std::filesystem::path pdf_;
};

// ─────────────────────────────────────────────────────────────────────
// R3-15 — real QDropEvent opens the dropped PDF.
//
// RED / mutation: revert MainWindow::dropEvent to a no-op (or break its
// `.pdf` suffix branch so openFile() is never reached) → documentForTesting()
// stays null → this test fails. The pre-existing grep test could not catch
// that regression; this real-event test does.
// ─────────────────────────────────────────────────────────────────────
TEST_F(UiClipboardDragDropTest, DropOpensPdfDocument) {
    if (!std::filesystem::exists(pdf_)) {
        GTEST_SKIP() << "test_3page.pdf fixture missing";
    }
    ASSERT_EQ(mw_->documentForTesting(), nullptr)
        << "precondition: no document before the drop";

    QMimeData mime;
    mime.setUrls({ QUrl::fromLocalFile(QString::fromStdString(pdf_.string())) });

    // Faithful DnD sequence: a real drop is always preceded by an accepted
    // drag-enter that establishes the widget as the drop target. Post the
    // enter, then the drop — both real events through the widget's event()
    // dispatch (setAcceptDrops(true) is set in the MainWindow ctor).
    QDragEnterEvent enter(QPoint(20, 20), Qt::CopyAction, &mime,
                          Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(mw_.get(), &enter);
    ASSERT_TRUE(enter.isAccepted())
        << "dragEnter must accept the .pdf payload before the drop";

    QDropEvent drop(QPointF(20, 20), Qt::CopyAction, &mime,
                    Qt::LeftButton, Qt::NoModifier, QEvent::Drop);
    QApplication::sendEvent(mw_.get(), &drop);
    EXPECT_TRUE(drop.isAccepted())
        << "the drop event must be accepted for a .pdf payload";

    // The drop path runs the full open pipeline (preflight child process +
    // deferred layout); wait on the real observable outcome.
    ASSERT_TRUE(spinUntil([&]{ return mw_->documentForTesting() != nullptr; }))
        << "dropping a .pdf must open the document (waited for the open "
           "pipeline to settle)";
    EXPECT_GT(mw_->documentForTesting()->pageCount(), 0)
        << "opened document must have pages";
}

// ─────────────────────────────────────────────────────────────────────
// R3-15 — real QDragEnterEvent accepts a .pdf URL, rejects non-file text.
//
// RED / mutation: make dragEnterEvent unconditionally acceptProposedAction()
// → the non-PDF sub-case (text payload) would wrongly be accepted.
// ─────────────────────────────────────────────────────────────────────
TEST_F(UiClipboardDragDropTest, DragEnterAcceptsPdfUrl) {
    if (!std::filesystem::exists(pdf_)) {
        GTEST_SKIP() << "test_3page.pdf fixture missing";
    }
    QMimeData pdfMime;
    pdfMime.setUrls({ QUrl::fromLocalFile(QString::fromStdString(pdf_.string())) });
    QDragEnterEvent enter(QPoint(20, 20), Qt::CopyAction, &pdfMime,
                          Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(mw_.get(), &enter);
    EXPECT_TRUE(enter.isAccepted())
        << "dragEnter must accept a .pdf URL payload";

    QMimeData textMime;
    textMime.setText("just some text, not a file");
    QDragEnterEvent enterText(QPoint(20, 20), Qt::CopyAction, &textMime,
                              Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(mw_.get(), &enterText);
    EXPECT_FALSE(enterText.isAccepted())
        << "dragEnter must NOT accept a non-file text payload";
}

// ─────────────────────────────────────────────────────────────────────
// R3-12 — END-TO-END: a REAL on-canvas selection makes onCopy copy that
// region. This exercises the PRODUCTION path SelectTool → regionSelected →
// MainWindow::setCopySelectionRect → onCopy → selection-cropped image. The
// test does NOT call setCopySelectionRect directly.
//
// RED / mutation: drop the `if (regionSelected) regionSelected(...)` emit in
// SelectTool::selectByIndex (or the `selectTool->regionSelected = ...` wiring
// in MainWindow::onAnnotationToolSelected) → production never sets the rect →
// hasCopySelection() is false → onCopy falls back to whole-page text → the
// clipboard image is null → this test fails (reverts to pre-wiring behavior).
// ─────────────────────────────────────────────────────────────────────
TEST_F(UiClipboardDragDropTest, CopySelectionOutAsImageEndToEnd) {
    if (!std::filesystem::exists(pdf_)) {
        GTEST_SKIP() << "test_3page.pdf fixture missing";
    }
    // The testAnnotCreatePipe seam is built at MainWindow CONSTRUCTION time,
    // gated on PDFE_TEST_ANNOT_PIPES. SetUp() already built mw_ WITHOUT the env,
    // so set it now and rebuild mw_ so the pipe actually exists.
    qputenv("PDFE_TEST_ANNOT_PIPES", "1");
    mw_ = std::make_unique<MainWindow>();

    mw_->openFile(QString::fromStdString(pdf_.string()));
    QApplication::processEvents();
    ASSERT_NE(mw_->documentForTesting(), nullptr);

    // Create a Square annotation with a known rect (100,100,80,60) through the
    // production create-pipe (same CreateAnnotationCommand path a tool takes).
    auto* createPipe = mw_->findChild<QLineEdit*>("testAnnotCreatePipe");
    ASSERT_NE(createPipe, nullptr);
    createPipe->setText("Square,0,100,100,80,60");
    QApplication::processEvents();

    QClipboard* cb = QApplication::clipboard();
    ASSERT_NE(cb, nullptr);
    cb->clear();

    // Drive a REAL selection through the LIVE SelectTool — NOT setCopySelectionRect.
    // (activates the Select tool → selectByIndex → regionSelected → setCopySelectionRect)
    ASSERT_TRUE(mw_->selectAnnotationRegionForTesting(0, 0));
    QApplication::processEvents();
    ASSERT_TRUE(mw_->hasCopySelection())
        << "a real on-canvas selection must set the copy-selection region via "
           "the SelectTool::regionSelected → setCopySelectionRect production "
           "wiring; if this fails the seam is orphaned again";

    mw_->copyForTesting();  // onCopy() → copySelectionAsImage()
    QApplication::processEvents();

    QImage out = rdpSafeClipboardImage(cb);
    ASSERT_FALSE(out.isNull())
        << "copy after a real selection must place a selection-cropped IMAGE "
           "on the clipboard (not whole-page text)";
    EXPECT_NEAR(out.width(), 160, 8)
        << "image width must match the selected annotation (80pt * 2x)";
    EXPECT_NEAR(out.height(), 120, 8)
        << "image height must match the selected annotation (60pt * 2x)";
    EXPECT_LT(out.width(), 400) << "must NOT be the whole page width";
    EXPECT_LT(out.height(), 400) << "must NOT be the whole page height";
    // Env cleanup happens in TearDown (runs even after a fatal ASSERT).
}

// ─────────────────────────────────────────────────────────────────────
// R3-12 — copy-out honours a directly-set selection as an image (unit-level
// coverage of copySelectionAsImage's crop/permission logic; the end-to-end
// production wiring is proven by CopySelectionOutAsImageEndToEnd above).
//
// RED / mutation: remove the `if (copySelectionValid_)` image branch from
// MainWindow::onCopy() → onCopy falls back to whole-page TEXT copy →
// clipboard()->image() is null → this test fails (pre-change behavior).
// ─────────────────────────────────────────────────────────────────────
TEST_F(UiClipboardDragDropTest, CopySelectionOutAsImage) {
    if (!std::filesystem::exists(pdf_)) {
        GTEST_SKIP() << "test_3page.pdf fixture missing";
    }
    mw_->openFile(QString::fromStdString(pdf_.string()));
    QApplication::processEvents();
    ASSERT_NE(mw_->documentForTesting(), nullptr);

    QClipboard* cb = QApplication::clipboard();
    ASSERT_NE(cb, nullptr);
    cb->clear();

    // Select an 80x60pt region on page 0 and copy.
    const QRectF sel(100.0, 100.0, 80.0, 60.0);
    mw_->setCopySelectionRect(0, sel);
    ASSERT_TRUE(mw_->hasCopySelection());

    mw_->copyForTesting();  // routes onCopy() → copySelectionAsImage()
    QApplication::processEvents();

    QImage out = rdpSafeClipboardImage(cb);
    ASSERT_FALSE(out.isNull())
        << "copy of a selection must place an IMAGE on the clipboard";

    // The image is rendered at 2x, so ~160x120 px. Assert it matches the
    // SELECTION (not the whole page, which at 2x would be > 1000px each side).
    EXPECT_NEAR(out.width(), 160, 8)
        << "copied image width must match the selection (80pt * 2x)";
    EXPECT_NEAR(out.height(), 120, 8)
        << "copied image height must match the selection (60pt * 2x)";
    EXPECT_LT(out.width(), 400)
        << "copied image must NOT be the whole page width";
    EXPECT_LT(out.height(), 400)
        << "copied image must NOT be the whole page height";
}

// ─────────────────────────────────────────────────────────────────────
// R3-12 — paste an image from the clipboard INTO the page.
//
// RED / mutation: remove the image branch from MainWindow::onPaste() (or make
// pasteImageFromClipboard() a no-op returning false) → the page image count is
// unchanged after paste → this test fails. That is the pre-change behavior
// (paste only handled text).
// ─────────────────────────────────────────────────────────────────────
TEST_F(UiClipboardDragDropTest, PasteImageIntoPage) {
    if (!std::filesystem::exists(pdf_)) {
        GTEST_SKIP() << "test_3page.pdf fixture missing";
    }
    mw_->openFile(QString::fromStdString(pdf_.string()));
    QApplication::processEvents();
    ASSERT_NE(mw_->documentForTesting(), nullptr);

    const int before = mw_->pageImageCountForTesting(0);
    ASSERT_GE(before, 0) << "image manager must be available after open";

    // Put a known solid image on the clipboard.
    QImage src(24, 24, QImage::Format_RGBA8888);
    src.fill(Qt::red);
    QClipboard* cb = QApplication::clipboard();
    ASSERT_NE(cb, nullptr);
    ASSERT_TRUE(rdpSafeSetClipboardImage(cb, src))
        << "clipboard setImage did not verify — test env issue";

    mw_->pasteForTesting();  // routes onPaste() → pasteImageFromClipboard()
    QApplication::processEvents();

    const int after = mw_->pageImageCountForTesting(0);
    EXPECT_EQ(after, before + 1)
        << "pasting a clipboard image must insert exactly one image into the page";
}

// ─────────────────────────────────────────────────────────────────────
// R3-15 — drag-OUT of a page carries the page thumbnail as image data.
//
// RED / mutation: make PagePanel::buildPageDragMimeData() return an empty
// QMimeData (drop the attachPageThumbToMime call) → hasImage() is false →
// this test fails. Pre-change, the panel only supported InternalMove reorder
// with no drag-out payload at all.
// ─────────────────────────────────────────────────────────────────────
TEST_F(UiClipboardDragDropTest, PageDragOutCarriesImage) {
    if (!std::filesystem::exists(pdf_)) {
        GTEST_SKIP() << "test_3page.pdf fixture missing";
    }
    auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
    auto doc = pdfe::core::Document::open(ctx, pdf_);
    ASSERT_NE(doc, nullptr);
    ASSERT_GT(doc->pageCount(), 0);

    PagePanel panel;
    panel.setDocument(doc.get());
    QApplication::processEvents();

    std::unique_ptr<QMimeData> md(panel.buildPageDragMimeData(0));
    ASSERT_NE(md, nullptr) << "in-range page must produce a drag payload";
    EXPECT_TRUE(md->hasImage())
        << "page drag-out payload must carry the page thumbnail as image data";
    QImage img = qvariant_cast<QImage>(md->imageData());
    EXPECT_FALSE(img.isNull())
        << "the carried image data must be a valid, non-null image";
    EXPECT_TRUE(md->hasText())
        << "page drag-out payload must carry a text label";

    // Out-of-range index yields no payload.
    std::unique_ptr<QMimeData> bad(panel.buildPageDragMimeData(9999));
    EXPECT_EQ(bad, nullptr);
}

// ─────────────────────────────────────────────────────────────────────
// WIN-20 (meta-audit follow-up F12) — MULTI-FILE drop.
//
// The pre-existing DropOpensPdfDocument sets exactly ONE url. Users routinely
// drop several PDFs at once, and "opens the first", "opens all in one window"
// and "crashes" were indistinguishable. MainWindow::dropEvent documents the
// intended contract — "Open one .pdf from the drop. Multi-file drops open a
// single document; the rest are ignored" — which was asserted by nothing.
//
// WIN-21 (phase-61) REFINED THAT CONTRACT, and this comment used to state the
// old one: it was "open the first .pdf URL", and it is now "open the first .pdf
// URL THAT EXISTS", falling back to the first candidate so an entirely
// unreadable drop still produces a specific report rather than silence. The
// case below drops three readable files, so its expectation is unchanged —
// but the sentence describing why was not, and a line number was cited that
// has since moved. Cited by behaviour now, not by line.
//
// The oracle is the PAGE COUNT of the opened document, so it identifies WHICH
// of the three dropped files was opened rather than merely that something was.
//
// RED / mutation: remove the `return` after openFile() in dropEvent (so the
// loop keeps going and the LAST pdf wins) → the opened document becomes the
// 10-page fixture → this fails. Today it passes only for "the first one".
// ─────────────────────────────────────────────────────────────────────
TEST_F(UiClipboardDragDropTest, MultiUrlDropOpensExactlyTheFirstPdf) {
    const std::filesystem::path first  = fixturesDir() / "test_3page.pdf";
    const std::filesystem::path second = fixturesDir() / "test_single.pdf";
    const std::filesystem::path third  = fixturesDir() / "test_10page.pdf";
    for (const auto& p : {first, second, third}) {
        if (!std::filesystem::exists(p)) {
            GTEST_SKIP() << "required fixture missing: " << p.string();
        }
    }
    ASSERT_EQ(mw_->documentForTesting(), nullptr)
        << "precondition: no document before the drop";

    QMimeData mime;
    mime.setUrls({
        QUrl::fromLocalFile(QString::fromStdString(first.string())),
        QUrl::fromLocalFile(QString::fromStdString(second.string())),
        QUrl::fromLocalFile(QString::fromStdString(third.string())),
    });
    ASSERT_EQ(mime.urls().size(), 3) << "this test is about a MULTI-url drop";

    QDragEnterEvent enter(QPoint(20, 20), Qt::CopyAction, &mime,
                          Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(mw_.get(), &enter);
    ASSERT_TRUE(enter.isAccepted())
        << "dragEnter must accept a multi-file .pdf payload";

    QDropEvent drop(QPointF(20, 20), Qt::CopyAction, &mime,
                    Qt::LeftButton, Qt::NoModifier, QEvent::Drop);
    QApplication::sendEvent(mw_.get(), &drop);
    EXPECT_TRUE(drop.isAccepted())
        << "a multi-file drop must be accepted, not silently dropped";

    ASSERT_TRUE(spinUntil([&]{ return mw_->documentForTesting() != nullptr; }))
        << "a multi-file drop must open a document (the first .pdf)";
    EXPECT_EQ(mw_->documentForTesting()->pageCount(), 3)
        << "the FIRST dropped pdf (test_3page.pdf, 3 pages) must be the one "
           "opened; 1 page means the second won, 10 means the last won";
}

// ─────────────────────────────────────────────────────────────────────
// WIN-20 companion — a mixed drop whose FIRST url is not a PDF.
//
// dropEvent skips non-local and non-.pdf urls and opens the first READABLE
// .pdf it finds (WIN-21, phase-61: "readable" was added because a mixed drop
// out of an archive can put an unextracted entry ahead of a real one). Before
// this test, a `continue` turned into a `return` — refusing the whole drop
// because the first item was a .txt — would have gone unnoticed.
//
// RED / mutation: change the `continue` in dropEvent's loop to `return` → no
// document opens → this fails.
// ─────────────────────────────────────────────────────────────────────
TEST_F(UiClipboardDragDropTest, MixedDropSkipsNonPdfAndOpensFirstPdf) {
    const std::filesystem::path pdfFirst = fixturesDir() / "test_3page.pdf";
    if (!std::filesystem::exists(pdfFirst)) {
        GTEST_SKIP() << "test_3page.pdf fixture missing";
    }

    const std::filesystem::path txt =
        std::filesystem::temp_directory_path() / "pdfe_win20_not_a_pdf.txt";
    {
        std::ofstream out(txt, std::ios::binary);
        ASSERT_TRUE(out.is_open()) << "could not stage the non-PDF drop item";
        out << "not a pdf";
    }

    QMimeData mime;
    mime.setUrls({
        QUrl::fromLocalFile(QString::fromStdString(txt.string())),
        QUrl::fromLocalFile(QString::fromStdString(pdfFirst.string())),
    });

    QDragEnterEvent enter(QPoint(20, 20), Qt::CopyAction, &mime,
                          Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(mw_.get(), &enter);

    QDropEvent drop(QPointF(20, 20), Qt::CopyAction, &mime,
                    Qt::LeftButton, Qt::NoModifier, QEvent::Drop);
    QApplication::sendEvent(mw_.get(), &drop);

    const bool opened =
        spinUntil([&]{ return mw_->documentForTesting() != nullptr; });
    std::error_code ec;
    std::filesystem::remove(txt, ec);

    ASSERT_TRUE(opened)
        << "a drop whose first item is a .txt must still open the first .pdf "
           "later in the list, not refuse the whole drop";
    EXPECT_EQ(mw_->documentForTesting()->pageCount(), 3)
        << "the opened document must be test_3page.pdf";
}

// ═════════════════════════════════════════════════════════════════════════
// WIN-21 (phase-8) — dropping a PDF that is not a file on disk
// ═════════════════════════════════════════════════════════════════════════
//
// The register's scenario is "drop a PDF directly out of a .zip in Explorer
// onto PDFE", filed as HYPOTHESIS with the rationale that "the failure mode
// depends on dropEvent's null-handling, which was not read in full".
//
// WHAT EXPLORER ACTUALLY HANDS OVER, which is why this is a GoogleTest and not
// only a PyAutoGUI probe. Dragging out of a compressed folder does not produce
// a `text/uri-list` naming a real file. Explorer offers the item as a SHELL
// VIRTUAL FILE — `CFSTR_FILEDESCRIPTOR` + `CFSTR_FILECONTENTS` — which Qt
// surfaces as a windows-mime payload with no URLs; or, if the shell has
// already extracted to a scratch location, as a `file:` URL to a temp path
// that may be gone by the time the drop is handled. Either way the interesting
// input is a drop whose URL list is empty, non-local, or names a path that
// does not exist, and every one of those is reachable deterministically from
// here. The live Explorer probe is a separate and weaker instrument: it
// exercises exactly one of these shapes, only on an interactive desktop, and
// cannot report which shape it produced.
//
// The property asserted is the register's — a clean refusal or a successful
// open, never a crash — plus one it does not name and should: never a WEDGE.
// A refusal that leaves the drop target dead is not clean, and "no crash"
// alone would not notice.
//
// A NOTE ON THE MODAL. A `.pdf` URL naming a file that is not there reaches
// MainWindow::openFile, which reports it through a blocking
// `QMessageBox::exec()` (objectName `openFileErrorDialog`). That is correct
// for a person and fatal for an unattended run, so these cases arm a dismisser
// first. The dismissal COUNT is reported rather than required to be zero: a
// dialog is one legitimate form of "clean refusal", and asserting it away
// would be asserting the product's behaviour away.

namespace {

// A payload shaped like Explorer's compressed-folder drag: the shell's
// virtual-file formats are present, and there is no URL at all.
std::unique_ptr<QMimeData> makeShellVirtualFilePayload()
{
    auto mime = std::make_unique<QMimeData>();
    // Qt exposes unconvertible Windows clipboard formats under this prefix.
    // The bytes are not a real FILEGROUPDESCRIPTORW — nothing reads them; what
    // is under test is the branch taken when hasUrls() is false.
    mime->setData(
        QStringLiteral("application/x-qt-windows-mime;value=\"FileGroupDescriptorW\""),
        QByteArray("\x01\x00\x00\x00", 4));
    mime->setData(
        QStringLiteral("application/x-qt-windows-mime;value=\"FileContents\""),
        QByteArray("%PDF-1.7\n", 9));
    return mime;
}

// Closes any modal that appears, for as long as it is alive, and counts them.
// A repeating timer rather than a single-shot because QMessageBox::exec()
// spins its own event loop: the dismisser has to keep firing INSIDE that loop
// until the box is gone.
class ModalDismisser {
public:
    ModalDismisser()
    {
        timer_.setInterval(10);
        QObject::connect(&timer_, &QTimer::timeout, [this] {
            if (auto* modal = QApplication::activeModalWidget()) {
                ++dismissed_;
                modal->close();
            }
        });
        timer_.start();
    }
    ~ModalDismisser() { timer_.stop(); }

    ModalDismisser(const ModalDismisser&) = delete;
    ModalDismisser& operator=(const ModalDismisser&) = delete;

    [[nodiscard]] int dismissed() const noexcept { return dismissed_; }

private:
    QTimer timer_;
    int dismissed_ = 0;
};

} // namespace

TEST_F(UiClipboardDragDropTest, HostileAndVirtualDropsAreRefusedCleanly)
{
    ASSERT_EQ(mw_->documentForTesting(), nullptr);

    ModalDismisser dismisser;

    // A path that is syntactically a .pdf and is not there: the
    // already-extracted-then-cleaned-up shape.
    const auto vanished =
        std::filesystem::temp_directory_path() / "pdfe_win21_not_there.pdf";
    std::error_code ec;
    std::filesystem::remove(vanished, ec);
    ASSERT_FALSE(std::filesystem::exists(vanished));

    // A directory wearing a .pdf name — an archive entry that is a folder.
    const auto dirNamedPdf =
        std::filesystem::temp_directory_path() / "pdfe_win21_dir.pdf";
    std::filesystem::create_directories(dirNamedPdf, ec);

    struct Case {
        const char* what;
        std::function<std::unique_ptr<QMimeData>()> make;
        bool expectDragEnterAccepted;
    };

    const std::vector<Case> cases = {
        {"shell virtual file (compressed folder), no URLs at all",
         makeShellVirtualFilePayload, false},

        {"empty URL list",
         [] { auto m = std::make_unique<QMimeData>();
              m->setUrls({}); return m; }, false},

        {"non-local URL with a .pdf suffix",
         [] { auto m = std::make_unique<QMimeData>();
              m->setUrls({QUrl(QStringLiteral(
                  "https://attacker.example/inner.pdf"))});
              return m; }, false},

        {"archive-internal pseudo-URL",
         [] { auto m = std::make_unique<QMimeData>();
              m->setUrls({QUrl(QStringLiteral(
                  "zip:///C:/archive.zip/inner.pdf"))});
              return m; }, false},

        {"a URL list whose only entry is empty",
         [] { auto m = std::make_unique<QMimeData>();
              m->setUrls({QUrl()}); return m; }, false},

        {"text payload with no URLs (drag of the archive entry's name)",
         [] { auto m = std::make_unique<QMimeData>();
              m->setText(QStringLiteral("inner.pdf")); return m; }, false},

        // A UNC target is refused at drag-enter, BEFORE anything stats it —
        // statting it is the NetBIOS/LLMNR broadcast the gate exists to stop.
        {"UNC .pdf URL — the path-security gate must hold on the drop path too",
         [] { auto m = std::make_unique<QMimeData>();
              m->setUrls({QUrl::fromLocalFile(QStringLiteral(
                  "\\\\attacker.example\\share\\inner.pdf"))});
              return m; }, false},

        // These two DO reach openFile, and are refused there with a dialog.
        {"local .pdf URL naming a file that does not exist",
         [&] { auto m = std::make_unique<QMimeData>();
               m->setUrls({QUrl::fromLocalFile(
                   QString::fromStdString(vanished.string()))});
               return m; }, true},

        {"local .pdf URL naming a DIRECTORY",
         [&] { auto m = std::make_unique<QMimeData>();
               m->setUrls({QUrl::fromLocalFile(
                   QString::fromStdString(dirNamedPdf.string()))});
               return m; }, true},
    };

    for (const auto& c : cases) {
        SCOPED_TRACE(c.what);
        auto mime = c.make();
        ASSERT_NE(mime, nullptr);

        QDragEnterEvent enter(QPoint(20, 20), Qt::CopyAction, mime.get(),
                              Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(mw_.get(), &enter);
        EXPECT_EQ(enter.isAccepted(), c.expectDragEnterAccepted)
            << "drag-enter acceptance for [" << c.what << "] is not what the "
               "payload warrants; a payload carrying no readable local .pdf "
               "must not advertise itself as droppable";

        QDropEvent drop(QPointF(20, 20), Qt::CopyAction, mime.get(),
                        Qt::LeftButton, Qt::NoModifier, QEvent::Drop);
        QApplication::sendEvent(mw_.get(), &drop);

        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 50);

        EXPECT_EQ(mw_->documentForTesting(), nullptr)
            << "a document was opened from a payload that names no readable "
               "local PDF: " << c.what;
    }

    // Not an assertion on the count, a record of it: the three openFile-reaching
    // cases are expected to have produced a user-visible refusal.
    GTEST_LOG_(INFO) << "modal refusals dismissed: " << dismisser.dismissed();

    std::filesystem::remove_all(dirNamedPdf, ec);
}

// The half that matters to a user.
TEST_F(UiClipboardDragDropTest, ARealDropStillWorksAfterAVirtualFileDrop)
{
    if (!std::filesystem::exists(pdf_)) {
        GTEST_SKIP() << "test_3page.pdf fixture missing";
    }

    ModalDismisser dismisser;

    auto hostile = makeShellVirtualFilePayload();
    QDragEnterEvent badEnter(QPoint(20, 20), Qt::CopyAction, hostile.get(),
                             Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(mw_.get(), &badEnter);
    EXPECT_FALSE(badEnter.isAccepted())
        << "a payload with no file URLs must not be accepted for drop";

    QDropEvent badDrop(QPointF(20, 20), Qt::CopyAction, hostile.get(),
                       Qt::LeftButton, Qt::NoModifier, QEvent::Drop);
    QApplication::sendEvent(mw_.get(), &badDrop);
    QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 50);
    ASSERT_EQ(mw_->documentForTesting(), nullptr);

    // Now the real thing.
    QMimeData good;
    good.setUrls({ QUrl::fromLocalFile(QString::fromStdString(pdf_.string())) });

    QDragEnterEvent enter(QPoint(20, 20), Qt::CopyAction, &good,
                          Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(mw_.get(), &enter);
    ASSERT_TRUE(enter.isAccepted())
        << "the drop target stopped accepting valid payloads after a "
           "virtual-file drop — the refusal wedged it";

    QDropEvent drop(QPointF(20, 20), Qt::CopyAction, &good,
                    Qt::LeftButton, Qt::NoModifier, QEvent::Drop);
    QApplication::sendEvent(mw_.get(), &drop);

    ASSERT_TRUE(spinUntil([&]{ return mw_->documentForTesting() != nullptr; }))
        << "a valid .pdf drop no longer opens after a refused one";
    EXPECT_GT(mw_->documentForTesting()->pageCount(), 0);
}

// A mixed drop — several items selected inside an archive, only some of which
// extract. The readable one must open; the rest must not prevent it. This is
// the "successful open" arm of the register's "clean refusal OR a successful
// open".
TEST_F(UiClipboardDragDropTest, AMixedVirtualAndRealDropOpensTheRealOne)
{
    if (!std::filesystem::exists(pdf_)) {
        GTEST_SKIP() << "test_3page.pdf fixture missing";
    }

    ModalDismisser dismisser;

    const auto vanished =
        std::filesystem::temp_directory_path() / "pdfe_win21_mixed_gone.pdf";
    std::error_code ec;
    std::filesystem::remove(vanished, ec);

    QMimeData mime;
    mime.setUrls({
        QUrl(QStringLiteral("zip:///C:/archive.zip/first.pdf")),
        QUrl::fromLocalFile(QString::fromStdString(vanished.string())),
        QUrl::fromLocalFile(QString::fromStdString(pdf_.string())),
    });

    QDragEnterEvent enter(QPoint(20, 20), Qt::CopyAction, &mime,
                          Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(mw_.get(), &enter);
    ASSERT_TRUE(enter.isAccepted());

    QDropEvent drop(QPointF(20, 20), Qt::CopyAction, &mime,
                    Qt::LeftButton, Qt::NoModifier, QEvent::Drop);
    QApplication::sendEvent(mw_.get(), &drop);

    ASSERT_TRUE(spinUntil([&]{ return mw_->documentForTesting() != nullptr; }))
        << "the readable PDF in a mixed drop was never opened; dropEvent "
           "stops at the FIRST .pdf-suffixed URL, so an unreadable entry "
           "ahead of a readable one loses the drop";
    EXPECT_GT(mw_->documentForTesting()->pageCount(), 0);
}
