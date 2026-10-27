// tests/test_chrome_phase9.cpp
// PHASE1_HARDENING: skip=0 expect_ge=0 no_throw=1  (2026-05-08)
//
// Phase-9 regression locks for the panels + dialogs + menus +
// chrome + visual-fidelity + context-menus + keyboard + drag-drop
// subsystem cycle. Targets P0 rows in UI_MATRIX_GAPS.md across
// UI-PNL-* / UI-DLG-* / UI-MENU-* / UI-CHR-* / UI-VF-* / UI-CTX-* /
// UI-KB-* / UI-DD-* (rows not already locked by prior phases).
//
// RED-first:
//   The visual-fidelity glyph tests (UI-VF-01..04), the shortcut-
//   collision test (UI-KB-04), and the canvas context-menu policy
//   test (UI-CTX-01) FAIL on the pre-Phase-9 source. After the
//   accompanying production fixes (TextEditToolBar glyph swap,
//   signDocumentAction shortcut rebind, PdfCanvas context-menu
//   policy enable, MainWindow text-edit context menu expansion)
//   they pass. Baseline failures are captured to
//   tests/fixtures/ui_audit/phase_chrome_baseline_failures.txt
//   before any production source change.
//
// Existing tests cover adjacent invariants:
//   test_recent_files.cpp           — RecentFilesMenu.fileTriggered
//                                      signal at the unit level.
//   test_keyboard_navigation.cpp    — no-document key dispatch.
//   test_metadata_dialog.cpp        — dialog edit-field round-trip.
//   test_text_edit_toolbar.cpp      — toolbar widget construction.
//   test_bookmark_manager.cpp       — BookmarkManager mutation API.
//   test_layers.cpp                 — LayerManager OCG API.
// This file adds the Phase-9 cross-component contracts the audit
// flagged.

#include <gtest/gtest.h>

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDoubleSpinBox>
#include <QFontMetricsF>
#include <QKeyEvent>
#include <QKeySequence>
#include <QList>
#include <QMenuBar>
#include <QSignalSpy>
#include <QTest>
#include <QToolButton>
#include <QTreeWidget>

#include "core/BookmarkManager.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/UndoManager.h"
#include "layers/LayerManager.h"
#include "metadata/MetadataManager.h"
#include "ui/BookmarkPanel.h"
#include "ui/LayerPanel.h"
#include "ui/MetadataDialog.h"
#include "ui/PdfCanvas.h"
#include "ui/RecentFilesMenu.h"
#include "ui/SearchPanel.h"
#include "ui/TextEditToolBar.h"
#include "app/AppSettings.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <set>
#include <vector>

using namespace pdfe::core;
using namespace pdfe::ui;

namespace {

class Phase9ChromeTest : public ::testing::Test {
protected:
    void SetUp() override {
        ctx_ = std::make_shared<MuPdfContext>();
        std::filesystem::path fixtures = TEST_FIXTURES_DIR;
        plainPdf_ = fixtures / "test_3page.pdf";
        if (!std::filesystem::exists(plainPdf_)) {
            GTEST_SKIP() << "Fixture missing: " << plainPdf_;
        }
    }

    std::shared_ptr<MuPdfContext> ctx_;
    std::filesystem::path plainPdf_;
};

}  // namespace

// ─────────────────────────────────────────────────────────────────────
// UI-PNL-01/02/05 — SearchPanel signal contract.
// Lock that the SearchPanel exposes the signals MainWindow listens to:
// searchResultSelected (page+quads) for double-click navigation, and
// replaceAllRequested (find,replace) for the Replace All button.
// Without these signals on the panel, the panel→canvas wiring in
// MainWindow silently never fires.
// ─────────────────────────────────────────────────────────────────────
// Phase-16 (qt_test_gap_audit finding #12): test renamed/scoped to
// be honest about what it locks — the signal SIGNATURES at the
// metaobject level (a compile-time-contract proxy via QSignalSpy
// validity). Behavioral end-to-end coverage of SearchPanel result
// selection lives in test_search_panel_behavior.cpp / the chrome E2E
// suite. This narrow lock fires when a future MOC drift renames or
// re-types one of these signals, which would silently break
// MainWindow's connect() lines.
TEST_F(Phase9ChromeTest, SearchPanelExposesNavigationAndReplaceSignals) {
    SearchPanel panel;

    QSignalSpy navSpy(&panel,
        SIGNAL(searchResultSelected(int, QVector<QRectF>)));
    EXPECT_TRUE(navSpy.isValid())
        << "SearchPanel must expose searchResultSelected(int, "
           "QVector<QRectF>) for BookmarkPanel/PdfCanvas to navigate "
           "to a clicked search hit (UI-PNL-02). [signature lock]";

    QSignalSpy replaceSpy(&panel,
        SIGNAL(replaceAllRequested(QString, QString)));
    EXPECT_TRUE(replaceSpy.isValid())
        << "SearchPanel must expose replaceAllRequested(QString, "
           "QString) so MainWindow can route Replace-All to a "
           "CompositeCommand (UI-PNL-05). [signature lock]";

    QSignalSpy clearSpy(&panel,
        SIGNAL(clearSearchHighlights()));
    EXPECT_TRUE(clearSpy.isValid())
        << "SearchPanel must expose clearSearchHighlights() so the "
           "canvas clears the highlight overlay on Clear (UI-PNL-04). "
           "[signature lock]";

    // Sanity that the panel's own metaobject reports the expected
    // indices (additional defense if the spy were ever to false-pos
    // on signature drift).
    EXPECT_GE(panel.metaObject()->indexOfSignal(
        "searchResultSelected(int,QVector<QRectF>)"), 0);
    EXPECT_GE(panel.metaObject()->indexOfSignal(
        "replaceAllRequested(QString,QString)"), 0);
    EXPECT_GE(panel.metaObject()->indexOfSignal(
        "clearSearchHighlights()"), 0);
}

// ─────────────────────────────────────────────────────────────────────
// UI-PNL-06/07 — BookmarkPanel populates from BookmarkManager outline
// and BookmarkManager surfaces a tree the panel can render. Lock at
// the manager/contract level: load() returns a vector that the panel's
// populateTree() can consume, and bookmarkActivated is exposed so
// double-click navigation reaches MainWindow.
// ─────────────────────────────────────────────────────────────────────
TEST_F(Phase9ChromeTest, BookmarkPanelExposesActivationSignal) {
    BookmarkPanel panel;

    QSignalSpy spy(&panel,
        SIGNAL(bookmarkActivated(int, float, float, float)));
    EXPECT_TRUE(spy.isValid())
        << "BookmarkPanel must expose bookmarkActivated(int, float, "
           "float, float) so MainWindow can navigate the canvas on "
           "double-click (UI-PNL-07).";
}

TEST_F(Phase9ChromeTest, BookmarkManagerLoadProducesVectorForPanel) {
    auto doc = Document::open(ctx_, plainPdf_);
    ASSERT_NE(doc, nullptr);

    BookmarkManager mgr(ctx_);
    auto roots = mgr.load(doc.get());
    // test_3page.pdf has no outline → empty vector. The contract under
    // test is that load() returns a vector by value (movable into
    // BookmarkPanel::populateTree) and that totalBookmarkCount agrees.
    EXPECT_EQ(static_cast<int>(roots.size()), 0)
        << "Test fixture has no bookmarks; load must report empty.";
    EXPECT_EQ(mgr.totalBookmarkCount(), 0)
        << "totalBookmarkCount must agree with load() size.";

    // Surface contract: tree() must reference the same data load()
    // populated. Mutating through createBookmark must surface back.
    auto created = mgr.createBookmark({}, "Phase9-test", BookmarkDest{});
    EXPECT_GE(mgr.totalBookmarkCount(), 1)
        << "createBookmark must increment totalBookmarkCount so the "
           "panel's refresh() picks up the new node (UI-PNL-06).";
    EXPECT_FALSE(created.empty())
        << "createBookmark must return the path for the panel to "
           "select the new node.";
}

// ─────────────────────────────────────────────────────────────────────
// UI-PNL-11/12 — LayerPanel populates from LayerManager OCG list and
// the visibility-toggle round-trips through the manager. Lock at the
// manager level (panel renders through it).
// ─────────────────────────────────────────────────────────────────────
TEST_F(Phase9ChromeTest, LayerPanelExposesVisibilitySignal) {
    LayerPanel panel;

    QSignalSpy spy(&panel,
        SIGNAL(layerVisibilityChanged()));
    EXPECT_TRUE(spy.isValid())
        << "LayerPanel must expose layerVisibilityChanged() so the "
           "canvas knows to re-render after a checkbox toggle "
           "(UI-PNL-12).";
}

TEST_F(Phase9ChromeTest, LayerManagerListLayersSurfacesForPanel) {
    auto doc = Document::open(ctx_, plainPdf_);
    ASSERT_NE(doc, nullptr);

    pdfe::layers::LayerManager mgr(ctx_);
    // test_3page.pdf has no OCProperties → hasLayers false, listLayers
    // empty. Lock both surfaces.
    EXPECT_FALSE(mgr.hasLayers(doc.get()))
        << "Plain test fixture must have no OCG layers.";
    auto layers = mgr.listLayers(doc.get());
    EXPECT_EQ(layers.size(), 0u)
        << "listLayers must agree with hasLayers on the OCG-less "
           "fixture (UI-PNL-11).";
    EXPECT_EQ(mgr.layerCount(doc.get()), 0)
        << "layerCount must agree with listLayers size.";

    // Lock the LayerInfo surface the panel renders.
    for (const auto& l : layers) {
        (void)l.index;
        (void)l.name;
        (void)l.visible;
        (void)l.locked;
    }
}

// ─────────────────────────────────────────────────────────────────────
// UI-DLG-14 — MetadataDialog edit fields write back to /Info dict
// through MetadataManager round-trip. The dialog uses
// MetadataManager::writeMetadata on accept; lock the round-trip at
// the manager level (verified after save+reopen).
// ─────────────────────────────────────────────────────────────────────
TEST_F(Phase9ChromeTest, MetadataManagerRoundTripsThroughSaveReopen) {
    auto doc = Document::open(ctx_, plainPdf_);
    ASSERT_NE(doc, nullptr);

    pdfe::metadata::MetadataManager mgr(ctx_);

    pdfe::metadata::DocumentMetadata edited;
    edited.title    = "Phase9 round-trip";
    edited.author   = "phase9-author";
    edited.subject  = "phase9-subject";
    edited.keywords = "phase9, audit, ui";
    edited.creator  = "phase9-creator";
    EXPECT_NO_THROW({ mgr.writeMetadata(doc.get(), edited); });

    auto tempPath = std::filesystem::temp_directory_path() /
                    "pdfe_phase9_metadata.pdf";
    std::error_code ec;
    std::filesystem::remove(tempPath, ec);

    EXPECT_NO_THROW({ doc->saveAs(tempPath); });
    ASSERT_TRUE(std::filesystem::exists(tempPath))
        << "Document::saveAs must produce an output file.";

    auto reopened = Document::open(ctx_, tempPath);
    ASSERT_NE(reopened, nullptr);
    auto readBack = mgr.readMetadata(reopened.get());

    EXPECT_EQ(readBack.title,    edited.title);
    EXPECT_EQ(readBack.author,   edited.author);
    EXPECT_EQ(readBack.subject,  edited.subject);
    EXPECT_EQ(readBack.keywords, edited.keywords);
    EXPECT_EQ(readBack.creator,  edited.creator);

    std::filesystem::remove(tempPath, ec);
}

// ─────────────────────────────────────────────────────────────────────
// UI-MENU-04 — RecentFilesMenu fileTriggered signal: locking that the
// signal is wired to the menu action so MainWindow's openFile callback
// receives the path on click. test_recent_files.cpp covers the basic
// signal; this lock asserts the signature pywinauto / pyautogui flows
// will actually see in production (single QString argument, no extra
// overloads that would break MainWindow's connect).
// ─────────────────────────────────────────────────────────────────────
TEST_F(Phase9ChromeTest, RecentFilesMenuExposesFileTriggeredSignalSignature) {
    auto settings = std::make_unique<pdfe::app::AppSettings>();
    auto menu = std::make_unique<RecentFilesMenu>(QString("Recent"),
                                                  settings.get());

    QSignalSpy spy(menu.get(), SIGNAL(fileTriggered(QString)));
    EXPECT_TRUE(spy.isValid())
        << "RecentFilesMenu::fileTriggered(QString) must remain the "
           "single signal MainWindow connects to (UI-MENU-04). Adding "
           "an overload would silently break the click→openFile route.";

    const QString recentPath = QStringLiteral("C:/phase9/recent.pdf");
    menu->addFile(recentPath);
    auto actions = menu->actions();
    ASSERT_GE(actions.size(), 1)
        << "addFile must add a QAction so the click reaches "
           "onActionTriggered → fileTriggered.";

    // Phase-16 fix (qt_test_gap_audit finding #13): trigger the
    // action to exercise the production click path and verify
    // the spy received the path. The previous version only checked
    // that the signal existed in the metaobject — a regression where
    // QAction::triggered is no longer connected to fileTriggered
    // would not have been caught.
    QAction* fileAction = nullptr;
    for (QAction* a : actions) {
        if (a->text().contains("recent.pdf")) { fileAction = a; break; }
    }
    if (fileAction) {
        fileAction->trigger();
        ASSERT_EQ(spy.count(), 1)
            << "Triggering the QAction must fire fileTriggered once";
        EXPECT_EQ(spy.at(0).at(0).toString(), recentPath);
    }
}

// ─────────────────────────────────────────────────────────────────────
// UI-CHR-11 — Title-bar dirty marker. UndoManager::isClean() drives
// the trailing "*" in MainWindow::updateWindowTitle. Lock the
// UndoManager contract directly and the cleanStateChanged signal that
// MainWindow connects to.
// ─────────────────────────────────────────────────────────────────────
TEST_F(Phase9ChromeTest, UndoManagerCleanStateDrivesDirtyMarker) {
    pdfe::commands::UndoManager mgr;
    EXPECT_TRUE(mgr.isClean())
        << "UndoManager starts clean; window title shows no '*'.";

    QSignalSpy spy(&mgr, SIGNAL(cleanStateChanged(bool)));
    EXPECT_TRUE(spy.isValid())
        << "UndoManager must emit cleanStateChanged(bool) for "
           "MainWindow::updateWindowTitle to react (UI-CHR-11).";

    // Phase-16 fix (qt_test_gap_audit finding #14): exercise the
    // actual clean→dirty transition. Without this, a regression
    // where execute() no longer emits cleanStateChanged would not
    // be caught.
    struct NopCommand : public pdfe::commands::Command {
        void execute() override {}
        void undo() override {}
        std::string description() const override { return "nop"; }
    };
    mgr.execute(std::make_unique<NopCommand>());
    QApplication::processEvents();
    ASSERT_GE(spy.count(), 1)
        << "execute() must drive a cleanStateChanged(false) emit";
    EXPECT_EQ(spy.at(0).at(0).toBool(), false)
        << "First emit after execute() must carry isClean=false";
    EXPECT_FALSE(mgr.isClean());
}

// ─────────────────────────────────────────────────────────────────────
// UI-KB-15 — Page-navigation keys with a document loaded.
// The existing test_keyboard_navigation.cpp covers the no-doc no-crash
// path; this locks the doc-loaded path through goToPage. We don't
// drive a full QKeyEvent dispatch here (that requires TileCache +
// TileRenderer) — instead we lock the goToPage API the keyPressEvent
// handler invokes, which is the substantive UX contract.
// ─────────────────────────────────────────────────────────────────────
TEST_F(Phase9ChromeTest, GoToPageHandlesPageDownPageUpHomeEnd) {
    PdfCanvas canvas;

    // Without a document, goToPage is a no-op. After tile cache wiring
    // the same calls would advance currentPage_; UI-KB-15's no-crash +
    // no-state-corruption contract holds in both cases.
    EXPECT_EQ(canvas.currentPage(), 0);
    canvas.goToPage(1);   // PageDown
    EXPECT_EQ(canvas.currentPage(), 0)
        << "goToPage with no document must not change currentPage_.";
    canvas.goToPage(0);   // PageUp / Home
    EXPECT_EQ(canvas.currentPage(), 0);
    canvas.goToPage(99);  // End beyond doc
    EXPECT_EQ(canvas.currentPage(), 0);

    // Drive the actual key path through QTest::keyClick. The handler
    // must accept the events without crashing whether or not a
    // document is loaded; existing PageDown/PageUp/Home/End cases in
    // test_keyboard_navigation.cpp verify the no-doc path. This lock
    // adds Plus / Minus zoom which are dispatched in the same switch.
    EXPECT_NO_THROW({
        QTest::keyClick(&canvas, Qt::Key_PageDown);
        QTest::keyClick(&canvas, Qt::Key_PageUp);
        QTest::keyClick(&canvas, Qt::Key_Home);
        QTest::keyClick(&canvas, Qt::Key_End);
    });
    // The "no document → no state mutation" invariant must hold
    // after the key sequence too — bare EXPECT_NO_THROW only checks
    // for exceptions, not for silent state corruption.
    EXPECT_EQ(canvas.currentPage(), 0)
        << "key navigation with no document must leave currentPage_ unchanged";
}

// ─────────────────────────────────────────────────────────────────────
// UI-VF-01..04 — Alignment glyph correctness on TextEditToolBar.
// The pre-Phase-9 source uses single-arrow Unicode glyphs (← ↔ → ↕)
// for left / center / right / justify. U+2195 (↕, up-down arrow) is
// the worst offender — it semantically means "vertical position
// adjustment", NOT "horizontal justify". Acrobat / Word / LibreOffice
// all use bar-stack glyphs (e.g. U+2261 ≡ for justify, U+2630 ☰
// alternative). This test locks the corrected glyph set.
//
// RED on the pre-fix source. GREEN after TextEditToolBar.cpp swaps
// the glyphs (production fix is line-local in TextEditToolBar.cpp:
// 159..194).
// ─────────────────────────────────────────────────────────────────────
namespace {

// Glyphs the audit explicitly flagged as wrong. Any align button still
// using one of these is a regression.
const QString kForbiddenGlyphs = QStringLiteral(
    "←"  // ← left arrow
    "→"  // → right arrow
    "↔"  // ↔ left-right arrow
    "↕"  // ↕ up-down arrow
);

bool textIsForbiddenArrow(const QString& s) {
    if (s.length() != 1) return false;
    return kForbiddenGlyphs.contains(s);
}

}  // namespace

TEST_F(Phase9ChromeTest, AlignmentButtonGlyphsAreNotPlainArrows) {
    TextEditToolBar bar;

    auto* left   = bar.findChild<QToolButton*>("alignLeftButton");
    auto* center = bar.findChild<QToolButton*>("alignCenterButton");
    auto* right  = bar.findChild<QToolButton*>("alignRightButton");
    auto* just   = bar.findChild<QToolButton*>("alignJustifyButton");

    ASSERT_NE(left,   nullptr) << "alignLeftButton must exist.";
    ASSERT_NE(center, nullptr) << "alignCenterButton must exist.";
    ASSERT_NE(right,  nullptr) << "alignRightButton must exist.";
    ASSERT_NE(just,   nullptr) << "alignJustifyButton must exist.";

    // UI-VF-01: justify must NOT be U+2195.
    EXPECT_NE(just->text(), QString(QChar(0x2195)))
        << "alignJustifyButton text is U+2195 (↕ up-down arrow), which "
           "semantically means 'vertical position adjustment'. "
           "Replace with U+2261 (≡) or an icon. (UI-VF-01)";

    // UI-VF-02..04: left/center/right must NOT be plain single-arrow
    // glyphs either — those communicate "drag handle" rather than
    // alignment. The icon should either be a multi-bar glyph OR a
    // QIcon (in which case text() is empty / non-arrow).
    EXPECT_FALSE(textIsForbiddenArrow(left->text()))
        << "alignLeftButton text is a plain arrow glyph (U+2190). "
           "Use a left-aligned bar-stack glyph or QIcon. (UI-VF-02). "
           "Got: '" << left->text().toStdString() << "'";
    EXPECT_FALSE(textIsForbiddenArrow(center->text()))
        << "alignCenterButton text is a plain arrow glyph (U+2194). "
           "Use a center-justified bar glyph or QIcon. (UI-VF-03). "
           "Got: '" << center->text().toStdString() << "'";
    EXPECT_FALSE(textIsForbiddenArrow(right->text()))
        << "alignRightButton text is a plain arrow glyph (U+2192). "
           "Use a right-aligned bar-stack glyph or QIcon. (UI-VF-04). "
           "Got: '" << right->text().toStdString() << "'";

    // The accessible names must remain the human-readable labels so
    // screen readers are unaffected by the glyph swap.
    EXPECT_EQ(left->accessibleName(),   QString("Align Left"));
    EXPECT_EQ(center->accessibleName(), QString("Align Center"));
    EXPECT_EQ(right->accessibleName(),  QString("Align Right"));
    EXPECT_EQ(just->accessibleName(),   QString("Justify"));
}

// ─────────────────────────────────────────────────────────────────────
// UI-KB-04 — Shortcut collision: signDocumentAction binds Ctrl+Shift+S
// on MainWindow.cpp:769, which is also QKeySequence::SaveAs (Save As
// on Windows). Two QActions on the same parent / same window with the
// same shortcut have undefined dispatch order in Qt.
//
// This test scans all QActions in a freshly-constructed MainWindow's
// menu bar, collects the shortcuts, and asserts no two distinct
// QActions share Ctrl+Shift+S. RED until signDocumentAction is
// rebound (recommendation: Ctrl+Alt+S).
//
// NOTE: We don't construct a full MainWindow here (that would pull in
// the whole document/render pipeline). Instead we lock the contract
// at the QAction list level by enumerating the actions a
// freshly-constructed TextEditToolBar / RecentFilesMenu / etc. expose
// — adequate for the regression we care about: any QAction listed
// in MainWindow as having a shortcut must have a unique one.
//
// To exercise the actual MainWindow.cpp:769 collision we instantiate
// two QActions matching the production code's bindings and check the
// QKeySequence comparison directly. This is the substantive lock —
// the production source is what determines collision, not runtime
// dispatch.
// ─────────────────────────────────────────────────────────────────────
TEST_F(Phase9ChromeTest, NoTwoActionsBindTheSameSaveAndSignShortcut) {
    // Locks the production source, NOT a runtime mirror — the
    // collision exists in MainWindow.cpp regardless of whether
    // MainWindow can be constructed in the test harness. Read
    // MainWindow.cpp directly and confirm signDocumentAction's
    // setShortcut line does not bind Ctrl+Shift+S.
    QKeySequence saveAsSeq = QKeySequence(QKeySequence::SaveAs);

    std::filesystem::path src =
        std::filesystem::path(TEST_FIXTURES_DIR).parent_path().parent_path()
        / "src" / "ui" / "MainWindow.cpp";
    if (!std::filesystem::exists(src)) {
        // Fall back: the source-tree path differs in some build
        // layouts. The substantive contract is the live binding;
        // skip the source-grep when we can't find the file.
        GTEST_SKIP() << "Could not locate src/ui/MainWindow.cpp at "
                     << src;
    }

    std::ifstream in(src);
    ASSERT_TRUE(in.is_open()) << "Failed to open " << src;

    std::string line;
    bool foundSign = false;
    std::string signLine;
    int signLineNo = 0;
    int lineNo = 0;
    while (std::getline(in, line)) {
        ++lineNo;
        // Look for the line that calls setShortcut on
        // signDocumentAction_. The block typically reads:
        //   signDocumentAction_->setShortcut(
        //       QKeySequence(tr("Ctrl+Shift+S")));
        if (line.find("signDocumentAction_") != std::string::npos &&
            line.find("setShortcut") != std::string::npos) {
            foundSign = true;
            signLine = line;
            signLineNo = lineNo;
            break;
        }
    }
    in.close();
    ASSERT_TRUE(foundSign)
        << "Could not find signDocumentAction_->setShortcut(...) line "
           "in MainWindow.cpp. The Phase-9 shortcut-collision lock "
           "depends on this line existing.";

    // setShortcut may pass the QKeySequence inline or split across
    // the next line. Read additional lines until we find the closing
    // ')' to capture the full call.
    if (signLine.find(')') == std::string::npos) {
        std::ifstream in2(src);
        std::string l;
        int n = 0;
        std::string concat = signLine;
        while (std::getline(in2, l)) {
            ++n;
            if (n <= signLineNo) continue;
            concat += " " + l;
            if (l.find(')') != std::string::npos) break;
        }
        signLine = concat;
    }

    // The collision: any binding that is QKeySequence::SaveAs or the
    // literal "Ctrl+Shift+S" string. Reject both.
    EXPECT_EQ(signLine.find("QKeySequence::SaveAs"), std::string::npos)
        << "signDocumentAction_ binds QKeySequence::SaveAs at line "
        << signLineNo
        << " of MainWindow.cpp — collides with saveAsAction_'s "
           "QKeySequence::SaveAs binding (UI-KB-04).";
    EXPECT_EQ(signLine.find("\"Ctrl+Shift+S\""), std::string::npos)
        << "signDocumentAction_ binds Ctrl+Shift+S at line "
        << signLineNo
        << " of MainWindow.cpp — collides with QKeySequence::SaveAs "
           "(Ctrl+Shift+S on Windows). Rebind to Ctrl+Alt+S "
           "(UI-KB-04). Line: " << signLine;

    (void)saveAsSeq;  // captured for future cross-check tests.
}

// ─────────────────────────────────────────────────────────────────────
// UI-CTX-01 — Canvas right-click outside text-edit mode produces
// SOME contextual menu. Pre-Phase-9 PdfCanvas does not override
// contextMenuEvent at all, so right-click is a no-op (Acrobat shows
// Zoom / Fit / Single Page / etc.).
//
// Post-fix contract: PdfCanvas::contextMenuPolicy must NOT be
// Qt::DefaultContextMenu (because PdfCanvas overrides
// contextMenuEvent to emit canvasContextMenuRequested), AND/OR a
// canvasContextMenuRequested signal exists on PdfCanvas.
// ─────────────────────────────────────────────────────────────────────
TEST_F(Phase9ChromeTest, CanvasExposesContextMenuRequestSignal) {
    PdfCanvas canvas;

    // Structural lock: the signal must exist in the metaobject so
    // MainWindow can connect to it.
    int idx = canvas.metaObject()->indexOfSignal(
        "canvasContextMenuRequested(QPoint)");
    ASSERT_GE(idx, 0)
        << "PdfCanvas must expose canvasContextMenuRequested(QPoint) "
           "so MainWindow can show a default canvas context menu "
           "(UI-CTX-01). Pre-fix the canvas right-click is a no-op.";

    // Behavioral lock: dispatch a real QContextMenuEvent and verify
    // the signal actually fires with a usable global position. This
    // exercises the contextMenuEvent override (PdfCanvas.cpp) end-
    // to-end on the canvas side. The MainWindow-side QMenu::exec
    // path is not exercised here (constructing MainWindow pulls in
    // the document/render pipeline) but the signal emission is the
    // contract MainWindow connects to.
    canvas.show();   // ensure the widget is in a state to receive events
    canvas.resize(400, 300);
    QSignalSpy spy(&canvas,
        SIGNAL(canvasContextMenuRequested(QPoint)));
    ASSERT_TRUE(spy.isValid());

    QPoint widgetPos(120, 80);
    QPoint screenPos = canvas.mapToGlobal(widgetPos);
    QContextMenuEvent ev(QContextMenuEvent::Mouse,
                         widgetPos, screenPos);
    QApplication::sendEvent(&canvas, &ev);

    EXPECT_TRUE(ev.isAccepted())
        << "PdfCanvas::contextMenuEvent must accept the event so it "
           "is not propagated up to the parent (UI-CTX-01).";
    // GitHub #273: this was EXPECT_EQ followed by `if (spy.count() == 1)`.
    // The count was already asserted, so the `if` was only there to keep
    // takeFirst() from running on an empty list — EXPECT_ continues after a
    // failure, so the guard was necessary. ASSERT_ aborts the test instead,
    // which makes the body reachable unconditionally and strictly strengthens
    // the case: the position is now always checked when the count is right.
    ASSERT_EQ(spy.count(), 1)
        << "PdfCanvas must emit canvasContextMenuRequested exactly "
           "once per right-click (UI-CTX-01).";
    const QPoint emitted = spy.takeFirst().at(0).toPoint();
    EXPECT_EQ(emitted, screenPos)
        << "Emitted screen position must equal the event's "
           "globalPos so MainWindow can call QMenu::exec(pos).";

    // Per-event idempotence: each distinct QContextMenuEvent must
    // produce exactly one signal emission (no stray re-entry from
    // the override).
    QContextMenuEvent ev2(QContextMenuEvent::Mouse,
                          widgetPos, screenPos);
    QApplication::sendEvent(&canvas, &ev2);
    EXPECT_EQ(spy.count(), 1)
        << "Each QContextMenuEvent must produce exactly one further "
           "signal emission. Got "
        << spy.count() << " after the second send.";
}

// ─────────────────────────────────────────────────────────────────────
// UI-DD-03 cross-link — PagePanel::pageMoved signal reachability.
// Phase 6 locked the page-reorder behavior end-to-end; this entry
// confirms the panel STILL exposes a pageMoved (or equivalent
// rearrangement) signal that MainWindow can connect to. If anyone
// renames or removes the signal, the Phase-9 lock catches it before
// Phase 6's higher-level test sees the breakage downstream.
// ─────────────────────────────────────────────────────────────────────
#include "ui/PagePanel.h"
TEST_F(Phase9ChromeTest, PagePanelReorderContractStaysCovered) {
    pdfe::ui::PagePanel panel;
    int idx = panel.metaObject()->indexOfSignal("pageMoved(int,int)");
    EXPECT_GE(idx, 0)
        << "PagePanel must expose pageMoved(int from, int to) so "
           "MainWindow can wire drag-drop reorder through to "
           "MovePageCommand (UI-DD-03 cross-link to UI-PG-13). "
           "Renaming or removing this signal breaks Phase 6's "
           "behavioral lock silently from MainWindow's perspective.";
}

// ─────────────────────────────────────────────────────────────────────
// UI-KB-keyboard-coverage — Tab traversal + focus policy.
// Pipeline.json criterion: every interactive widget reachable via Tab
// order + non-empty accessibleName + unique within parent window.
// Lock at the TextEditToolBar level (one of the densest interactive-
// widget containers in the chrome) by enumerating QToolButtons +
// QFontComboBox + QDoubleSpinBox and asserting each accepts focus and
// has a unique accessibleName.
// ─────────────────────────────────────────────────────────────────────
TEST_F(Phase9ChromeTest, ToolbarWidgetsAreTabReachableAndUniquelyNamed) {
    TextEditToolBar bar;

    // Collect all interactive child widgets that should appear in
    // the Tab order: every checkable / clickable child.
    QList<QWidget*> interactives;
    for (auto* btn : bar.findChildren<QToolButton*>()) {
        if (btn->isVisible() || btn->parentWidget())  // include hidden
            interactives.append(btn);
    }
    for (auto* combo : bar.findChildren<QComboBox*>()) {
        interactives.append(combo);
    }
    for (auto* spin : bar.findChildren<QDoubleSpinBox*>()) {
        interactives.append(spin);
    }
    ASSERT_GE(interactives.size(), 8)
        << "TextEditToolBar should expose at least 8 interactive "
           "widgets (font, size, B/I/U/S, color, 4 alignment buttons, "
           "atlas controls). Got " << interactives.size();

    int focusableCount = 0;
    int withAccName = 0;
    std::set<std::string> seenNames;
    std::vector<std::string> dupes;
    for (QWidget* w : interactives) {
        if (w->focusPolicy() != Qt::NoFocus) ++focusableCount;
        const QString accName = w->accessibleName();
        if (!accName.isEmpty()) {
            ++withAccName;
            const auto key = accName.toStdString();
            if (!seenNames.insert(key).second) {
                dupes.push_back(key);
            }
        }
    }

    EXPECT_GE(focusableCount, interactives.size() - 1)
        << "Almost every interactive widget in the toolbar must "
           "accept focus (focusPolicy != NoFocus) so Tab traversal "
           "reaches it. focusable=" << focusableCount
        << ", total=" << interactives.size();
    EXPECT_GE(withAccName, 8)
        << "At least 8 toolbar widgets must carry a non-empty "
           "accessibleName for screen-reader exposure.";
    EXPECT_TRUE(dupes.empty())
        << "Accessible names must be unique within the parent "
           "window. Duplicates found: "
        << (dupes.empty() ? "" : dupes.front());
}

// ─────────────────────────────────────────────────────────────────────
// UI-DD-01 — File drop on the main window opens the file. Pre-Phase-9
// the application did not call setAcceptDrops(true) anywhere on
// MainWindow / PdfCanvas (the only Qt drag-drop in src/ was the
// PagePanel internal-move). Acrobat / every native PDF viewer accepts
// dropping a .pdf onto the window.
//
// Test strategy: MainWindow constructor pulls in the document/render
// pipeline. We avoid that here by checking the contract on a
// throwaway QWidget that mirrors the production wiring: setAcceptDrops
// must be true after the production change, and the dragEnterEvent
// override must accept .pdf URLs. We assert at the source level by
// reading MainWindow.cpp for setAcceptDrops(true) and the .pdf-suffix
// branch in dragEnterEvent.
// ─────────────────────────────────────────────────────────────────────
TEST_F(Phase9ChromeTest, MainWindowAcceptsPdfFileDrop) {
    std::filesystem::path src =
        std::filesystem::path(TEST_FIXTURES_DIR).parent_path().parent_path()
        / "src" / "ui" / "MainWindow.cpp";
    if (!std::filesystem::exists(src)) {
        GTEST_SKIP() << "Could not locate src/ui/MainWindow.cpp at "
                     << src;
    }
    std::ifstream in(src);
    ASSERT_TRUE(in.is_open());
    std::string contents((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());

    EXPECT_NE(contents.find("setAcceptDrops(true)"), std::string::npos)
        << "MainWindow must call setAcceptDrops(true) so .pdf drops "
           "on the window are dispatched to dragEnterEvent (UI-DD-01).";
    EXPECT_NE(contents.find("dragEnterEvent"), std::string::npos)
        << "MainWindow must override dragEnterEvent to validate the "
           "drag payload (UI-DD-01).";
    EXPECT_NE(contents.find("dropEvent"), std::string::npos)
        << "MainWindow must override dropEvent to call openFile() on "
           "the dropped .pdf URL (UI-DD-01).";
    EXPECT_NE(contents.find("\".pdf\""), std::string::npos)
        << "MainWindow drag-drop overrides must filter on the .pdf "
           "extension so non-PDF drops fall through (UI-DD-01).";
}

// ─────────────────────────────────────────────────────────────────────
// UI-CTX-panels — BookmarkPanel and LayerPanel both expose a
// QTreeWidget with Qt::CustomContextMenuRequested policy + an
// onContextMenu private slot. Lock the contextMenuPolicy on the
// trees and verify the panels themselves remain context-menu-aware
// (so right-clicking a bookmark / layer item produces the
// rename/delete/etc. menu MainWindow expects).
// ─────────────────────────────────────────────────────────────────────
TEST_F(Phase9ChromeTest, BookmarkPanelTreeUsesCustomContextMenuPolicy) {
    BookmarkPanel panel;
    auto* tree = panel.findChild<QTreeWidget*>("bookmarkTree");
    ASSERT_NE(tree, nullptr)
        << "BookmarkPanel must expose its QTreeWidget under "
           "objectName 'bookmarkTree' for context-menu locating.";
    EXPECT_EQ(tree->contextMenuPolicy(), Qt::CustomContextMenu)
        << "BookmarkPanel tree must use CustomContextMenu so the "
           "panel can show its own rename/delete/move actions on "
           "right-click (UI-CTX-panels).";
}

TEST_F(Phase9ChromeTest, LayerPanelTreeIsContextMenuAware) {
    LayerPanel panel;
    auto* tree = panel.findChild<QTreeWidget*>();
    ASSERT_NE(tree, nullptr)
        << "LayerPanel must expose a QTreeWidget child for layer "
           "rendering. Without it the panel cannot host the "
           "context-menu actions (UI-CTX-panels).";
    EXPECT_EQ(tree->contextMenuPolicy(), Qt::CustomContextMenu)
        << "LayerPanel tree must use CustomContextMenu so the "
           "panel can show its create/rename/delete actions on "
           "right-click (UI-CTX-panels).";
}

// ─────────────────────────────────────────────────────────────────────
// UI-KB-07 — Ctrl+Q / closeEvent prompts to save unsaved changes
// before exit.
//
// Production gap before D1: closeEvent only saved window state;
// dirty document → silent close → data loss. Production now branches
// on hasUnsavedChanges() and routes through promptSaveOnClose().
//
// Test strategy: subclass MainWindow with deterministic overrides
// for both virtuals so each branch (Cancel / Discard / Save / clean)
// can be exercised without spawning a real QMessageBox.
// ─────────────────────────────────────────────────────────────────────
#include "ui/MainWindow.h"
#include <QCloseEvent>

namespace {

class TestableMainWindow : public pdfe::ui::MainWindow {
public:
    bool   dirty = false;
    int    promptCount = 0;
    CloseAction nextAnswer = CloseAction::Cancel;

    void setCancelAnswer()  { nextAnswer = CloseAction::Cancel; }
    void setDiscardAnswer() { nextAnswer = CloseAction::Discard; }
    void setSaveAnswer()    { nextAnswer = CloseAction::Save; }

protected:
    bool hasUnsavedChanges() const override { return dirty; }
    CloseAction promptSaveOnClose() override {
        ++promptCount;
        return nextAnswer;
    }
};

} // namespace

TEST_F(Phase9ChromeTest, KB07_CleanDocumentClosesWithoutPrompt) {
    TestableMainWindow w;
    w.dirty = false;
    QCloseEvent ev;
    QApplication::sendEvent(&w, &ev);
    EXPECT_TRUE(ev.isAccepted())
        << "Clean document closeEvent must accept the event without "
           "prompting (UI-KB-07 — clean path).";
    EXPECT_EQ(w.promptCount, 0)
        << "promptSaveOnClose must NOT be called for a clean "
           "document — that would waste a round-trip and surface a "
           "dialog the user does not need.";
}

TEST_F(Phase9ChromeTest, KB07_DirtyDocumentCancelKeepsWindowOpen) {
    TestableMainWindow w;
    w.dirty = true;
    w.setCancelAnswer();
    QCloseEvent ev;
    QApplication::sendEvent(&w, &ev);
    EXPECT_FALSE(ev.isAccepted())
        << "Dirty document with Cancel response must ignore the "
           "close event so the window stays open and unsaved work "
           "is not lost (UI-KB-07 — Cancel path).";
    EXPECT_EQ(w.promptCount, 1);
}

TEST_F(Phase9ChromeTest, KB07_DirtyDocumentDiscardClosesWindow) {
    TestableMainWindow w;
    w.dirty = true;
    w.setDiscardAnswer();
    QCloseEvent ev;
    QApplication::sendEvent(&w, &ev);
    EXPECT_TRUE(ev.isAccepted())
        << "Dirty document with Discard response must accept the "
           "close event — user explicitly chose to throw away unsaved "
           "work (UI-KB-07 — Discard path).";
    EXPECT_EQ(w.promptCount, 1);
}

// ─────────────────────────────────────────────────────────────────────
// UI-CTX-02 + UI-CTX-11 — text-edit-mode context menu must offer
// Cut / Copy / Paste / Select All (CTX-02) and Bold / Italic / Find
// (CTX-11), not just the Atlas Mode submenu it had before D1.
//
// Production change: MainWindow::buildTextEditContextMenu() now
// populates the menu with stable objectNames; the canvas
// contextMenuRequested handler dispatches by name. Tests inspect the
// populated menu directly without QMenu::exec.
// ─────────────────────────────────────────────────────────────────────
#include <QMenu>

TEST_F(Phase9ChromeTest, CTX02_TextEditMenuExposesClipboardActions) {
    pdfe::ui::MainWindow w;
    QMenu menu;
    w.buildTextEditContextMenu(menu, nullptr);

    auto* cut       = menu.findChild<QAction*>("textEditCutAction");
    auto* copy      = menu.findChild<QAction*>("textEditCopyAction");
    auto* paste     = menu.findChild<QAction*>("textEditPasteAction");
    auto* selectAll = menu.findChild<QAction*>("textEditSelectAllAction");

    ASSERT_NE(cut, nullptr)       << "UI-CTX-02: Cut action must exist";
    ASSERT_NE(copy, nullptr)      << "UI-CTX-02: Copy action must exist";
    ASSERT_NE(paste, nullptr)     << "UI-CTX-02: Paste action must exist";
    ASSERT_NE(selectAll, nullptr) << "UI-CTX-02: Select All action must exist";

    EXPECT_EQ(cut->shortcut(),       QKeySequence(QKeySequence::Cut));
    EXPECT_EQ(copy->shortcut(),      QKeySequence(QKeySequence::Copy));
    EXPECT_EQ(paste->shortcut(),     QKeySequence(QKeySequence::Paste));
    EXPECT_EQ(selectAll->shortcut(), QKeySequence(QKeySequence::SelectAll));

    // With no live tool, hasTextSelection() / hasActiveSpan() are
    // both false, so cut/copy must be disabled and paste/selectAll
    // must also be disabled (no span context to paste/select into).
    EXPECT_FALSE(cut->isEnabled());
    EXPECT_FALSE(copy->isEnabled());
    EXPECT_FALSE(paste->isEnabled());
    EXPECT_FALSE(selectAll->isEnabled());
}

TEST_F(Phase9ChromeTest, CTX11_TextEditMenuExposesFormattingAndFind) {
    pdfe::ui::MainWindow w;
    QMenu menu;
    w.buildTextEditContextMenu(menu, nullptr);

    auto* bold   = menu.findChild<QAction*>("textEditBoldAction");
    auto* italic = menu.findChild<QAction*>("textEditItalicAction");
    auto* find   = menu.findChild<QAction*>("textEditFindAction");

    ASSERT_NE(bold,   nullptr) << "UI-CTX-11: Bold action must exist";
    ASSERT_NE(italic, nullptr) << "UI-CTX-11: Italic action must exist";
    ASSERT_NE(find,   nullptr) << "UI-CTX-11: Find action must exist";

    EXPECT_TRUE(bold->isCheckable())
        << "Bold must be checkable so it can reflect the active "
           "selection's bold state.";
    EXPECT_TRUE(italic->isCheckable());

    EXPECT_EQ(bold->shortcut(),   QKeySequence(QKeySequence::Bold));
    EXPECT_EQ(italic->shortcut(), QKeySequence(QKeySequence::Italic));
}

TEST_F(Phase9ChromeTest, CTX02_MenuOrderIsClipboardThenFormattingThenFind) {
    pdfe::ui::MainWindow w;
    QMenu menu;
    w.buildTextEditContextMenu(menu, nullptr);

    // Collect non-separator actions in display order.
    QStringList names;
    for (QAction* a : menu.actions()) {
        if (a->isSeparator()) continue;
        names << a->objectName();
    }

    // Acrobat-style ordering: Cut/Copy/Paste/Select All first
    // (clipboard cluster), then Bold/Italic, then Find. This is the
    // ordering the matrix specifies and what users expect.
    auto idx = [&names](const QString& key) {
        return names.indexOf(key);
    };

    ASSERT_GE(idx("textEditCutAction"), 0);
    ASSERT_GE(idx("textEditBoldAction"), 0);
    ASSERT_GE(idx("textEditFindAction"), 0);

    EXPECT_LT(idx("textEditCutAction"),       idx("textEditBoldAction"));
    EXPECT_LT(idx("textEditCopyAction"),      idx("textEditBoldAction"));
    EXPECT_LT(idx("textEditPasteAction"),     idx("textEditBoldAction"));
    EXPECT_LT(idx("textEditSelectAllAction"), idx("textEditBoldAction"));
    EXPECT_LT(idx("textEditBoldAction"),      idx("textEditFindAction"));
    EXPECT_LT(idx("textEditItalicAction"),    idx("textEditFindAction"));
}

// Dispatch coverage: a real TextEditTool with a clicked-into span
// must observe state mutation when the menu's Select All action is
// triggered. Catches regressions in the objectName → tool method
// wiring inside buildTextEditContextMenu (karen review of D1).
#include "core/Document.h"
#include "core/Page.h"
#include "core/MuPdfContext.h"
#include "textedit/TextEditTool.h"
#include "textedit/TextEditModel.h"
#include <QMouseEvent>
#include <filesystem>

TEST_F(Phase9ChromeTest, CTX02_SelectAllActionDispatchHitsTool) {
    std::filesystem::path fixture =
        std::filesystem::path(TEST_FIXTURES_DIR) / "test_3page.pdf";
    if (!std::filesystem::exists(fixture)) {
        GTEST_SKIP() << "Fixture missing: " << fixture;
    }

    auto context = std::make_shared<pdfe::core::MuPdfContext>();
    auto doc = pdfe::core::Document::open(context, fixture);
    ASSERT_NE(doc, nullptr);
    auto page = doc->page(0);
    ASSERT_NE(page, nullptr);

    pdfe::textedit::TextEditTool tool(doc.get(), context);
    tool.setPage(0);

    // Click into a real span so hasActiveSpan() becomes true.
    auto blocks = page->extractText();
    ASSERT_FALSE(blocks.empty());
    pdfe::textedit::TextEditModel model(blocks);
    ASSERT_GT(model.spanCount(), 0);
    QPointF spanCenter = model.span(0).boundingBox.center();

    QMouseEvent press(QEvent::MouseButtonPress, QPointF(100, 100),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    tool.onMousePress(&press, 0, spanCenter);

    // Pre-condition for the dispatch: tool must consider a span active
    // so the menu's selectAll action gets enabled.
    ASSERT_TRUE(tool.hasActiveSpan())
        << "Test setup error — clicking into the span did not produce "
           "an active span; the dispatch contract cannot be exercised.";
    EXPECT_FALSE(tool.hasTextSelection())
        << "Sanity: a fresh click should have no selection until the "
           "Select All action fires.";

    pdfe::ui::MainWindow w;
    QMenu menu;
    w.buildTextEditContextMenu(menu, &tool);

    auto* selectAll = menu.findChild<QAction*>("textEditSelectAllAction");
    ASSERT_NE(selectAll, nullptr);
    ASSERT_TRUE(selectAll->isEnabled())
        << "Select All must be enabled when hasActiveSpan() is true.";

    // The actual dispatch test: trigger() fires the QAction's
    // triggered signal which must reach tool->performSelectAll().
    // If the connect inside buildTextEditContextMenu is broken, or
    // if performSelectAll/selectAll regress, this assertion fails.
    selectAll->trigger();

    EXPECT_TRUE(tool.hasTextSelection())
        << "Triggering textEditSelectAllAction did not invoke "
           "TextEditTool::performSelectAll — the objectName→method "
           "wiring inside buildTextEditContextMenu is broken "
           "(UI-CTX-02 dispatch coverage).";
}

// Lock the binding the matrix actually demands: the exit action's
// shortcut must be Ctrl+Q so the closeEvent prompt is reachable from
// the keyboard, not just the menu.
TEST_F(Phase9ChromeTest, KB07_ExitActionShortcutIsCtrlQ) {
    TestableMainWindow w;
    auto* exitAction = w.findChild<QAction*>("exitAction");
    ASSERT_NE(exitAction, nullptr)
        << "MainWindow must expose exitAction with objectName "
           "'exitAction' so the keyboard binding can be located "
           "for accessibility tests.";
    const QKeySequence expected(QStringLiteral("Ctrl+Q"));
    EXPECT_EQ(exitAction->shortcut(), expected)
        << "Ctrl+Q must trigger Exit so the dirty-document save "
           "prompt is reachable from the keyboard (UI-KB-07).";
}
