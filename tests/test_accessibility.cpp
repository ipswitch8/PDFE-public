// test_accessibility.cpp
// PHASE1_HARDENING: skip=0 expect_ge=0 no_throw=4  (2026-05-08)
// Unit tests for PdfCanvasAccessible

#include <gtest/gtest.h>
#include "ui/PdfCanvasAccessible.h"
#include "ui/AccessibilityHelper.h"
#include "ui/PdfCanvas.h"
#include <QAccessible>
#include <QAccessibleInterface>
#include <QAccessibleEvent>
#include <QApplication>
#include <QWidget>

using namespace pdfe::ui;

// Test fixture for PdfCanvasAccessible tests
class PdfCanvasAccessibleTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Install the accessibility factory before each test
        QAccessible::installFactory(pdfCanvasAccessibleFactory);
    }

    void TearDown() override {
        // Clean up after each test if needed
    }
};

// Test 1: Verify PdfCanvasAccessible role is QAccessible::Canvas
//
// Pipeline F phase-3 AC: role=Canvas required so UIA reports the
// widget under MSAA ROLE_SYSTEM_DIAGRAM (Canvas), not ROLE_SYSTEM_PANE.
TEST_F(PdfCanvasAccessibleTest, PdfCanvasAccessible_Role) {
    PdfCanvas canvas;
    PdfCanvasAccessible accessible(&canvas);

    EXPECT_EQ(QAccessible::Canvas, accessible.role());
}

// Test 1b: Canvas is a leaf in the UIA tree — clicks delivered to
// any point inside its bounding rect reach the widget itself; we do
// not enumerate per-link children. pywinauto's click_input() delivers
// real OS mouse events at coordinates, which is the supported
// hit-test path.
TEST_F(PdfCanvasAccessibleTest, PdfCanvasAccessible_LeafHitTest) {
    PdfCanvas canvas;
    PdfCanvasAccessible accessible(&canvas);

    EXPECT_EQ(0, accessible.childCount());
    EXPECT_EQ(nullptr, accessible.childAt(10, 10));
}

// Test 2: Verify accessible name when no document is loaded
TEST_F(PdfCanvasAccessibleTest, PdfCanvasAccessible_Name_NoDocument) {
    PdfCanvas canvas;
    PdfCanvasAccessible accessible(&canvas);

    QString name = accessible.text(QAccessible::Name);
    EXPECT_FALSE(name.isEmpty());
    EXPECT_TRUE(name.contains("No document", Qt::CaseInsensitive) ||
                name.contains("no document", Qt::CaseInsensitive));
}

// Test 3: Verify accessible description when no document is loaded
TEST_F(PdfCanvasAccessibleTest, PdfCanvasAccessible_Description_NoDocument) {
    PdfCanvas canvas;
    PdfCanvasAccessible accessible(&canvas);

    QString description = accessible.text(QAccessible::Description);
    EXPECT_FALSE(description.isEmpty());
    EXPECT_TRUE(description.contains("no document loaded", Qt::CaseInsensitive));
}

// Test 4: Verify action names are available
TEST_F(PdfCanvasAccessibleTest, PdfCanvasAccessible_ActionNames) {
    PdfCanvas canvas;
    PdfCanvasAccessible accessible(&canvas);

    QStringList actions = accessible.actionNames();

    // Should have 4 actions minimum
    EXPECT_GE(actions.size(), 4);

    // Verify expected actions are present
    EXPECT_TRUE(actions.contains("Next Page"));
    EXPECT_TRUE(actions.contains("Previous Page"));
    EXPECT_TRUE(actions.contains("Zoom In"));
    EXPECT_TRUE(actions.contains("Zoom Out"));
}

// Test 5: Verify factory returns correct interface for PdfCanvas
TEST_F(PdfCanvasAccessibleTest, PdfCanvasAccessible_Factory_MatchesCanvas) {
    PdfCanvas canvas;

    // Test with correct class name
    QAccessibleInterface* interface =
        pdfCanvasAccessibleFactory("pdfe::ui::PdfCanvas", &canvas);

    ASSERT_NE(nullptr, interface);
    EXPECT_EQ(QAccessible::Canvas, interface->role());
    // Note: QAccessibleInterface has protected dtor — managed by Qt framework

    // Test with wrong class name
    QAccessibleInterface* wrongInterface =
        pdfCanvasAccessibleFactory("SomeOtherClass", &canvas);

    EXPECT_EQ(nullptr, wrongInterface);
}

// Test 6: Verify factory returns null for wrong object type
TEST_F(PdfCanvasAccessibleTest, PdfCanvasAccessible_Factory_WrongClass) {
    QWidget someWidget;

    QAccessibleInterface* interface =
        pdfCanvasAccessibleFactory("QWidget", &someWidget);

    EXPECT_EQ(nullptr, interface);
}

// Test 7: Verify localized action names are available
TEST_F(PdfCanvasAccessibleTest, PdfCanvasAccessible_LocalizedActionNames) {
    PdfCanvas canvas;
    PdfCanvasAccessible accessible(&canvas);

    QString nextPage = accessible.localizedActionName("Next Page");
    EXPECT_FALSE(nextPage.isEmpty());

    QString prevPage = accessible.localizedActionName("Previous Page");
    EXPECT_FALSE(prevPage.isEmpty());

    QString zoomIn = accessible.localizedActionName("Zoom In");
    EXPECT_FALSE(zoomIn.isEmpty());

    QString zoomOut = accessible.localizedActionName("Zoom Out");
    EXPECT_FALSE(zoomOut.isEmpty());
}

// Test 8: Verify doAction doesn't crash when no document is loaded
TEST_F(PdfCanvasAccessibleTest, PdfCanvasAccessible_DoAction_NoDoc) {
    PdfCanvas canvas;
    PdfCanvasAccessible accessible(&canvas);

    // These should not crash even when no document is loaded.
    // EXPECT_NO_THROW alone can't catch silent navigation past
    // currentPage_ — assert the page-navigation actions leave it at
    // 0 (the no-document default). Zoom IS canvas-local state and is
    // expected to change even with no document loaded — the current
    // implementation increments/decrements zoom regardless.
    const int pageBefore = canvas.currentPage();

    EXPECT_NO_THROW({ accessible.doAction("Next Page"); });
    EXPECT_EQ(canvas.currentPage(), pageBefore)
        << "Next Page on no-doc canvas must not change currentPage";

    EXPECT_NO_THROW({ accessible.doAction("Previous Page"); });
    EXPECT_EQ(canvas.currentPage(), pageBefore)
        << "Previous Page on no-doc canvas must not change currentPage";

    // Zoom actions are expected to mutate canvas-local zoom; the
    // contract is only "must not throw" + "the next zoom action moves
    // in the opposite direction". Verify the latter.
    EXPECT_NO_THROW({ accessible.doAction("Zoom In"); });
    const double afterZoomIn = canvas.zoom();
    EXPECT_NO_THROW({ accessible.doAction("Zoom Out"); });
    EXPECT_LT(canvas.zoom(), afterZoomIn)
        << "Zoom Out must move the zoom level below the post-Zoom-In value";
}

// Additional edge case test: Verify factory with null object
TEST_F(PdfCanvasAccessibleTest, PdfCanvasAccessible_Factory_NullObject) {
    QAccessibleInterface* interface =
        pdfCanvasAccessibleFactory("pdfe::ui::PdfCanvas", nullptr);

    EXPECT_EQ(nullptr, interface);
}

// Additional test: Verify accessible interface can be queried via QAccessible
TEST_F(PdfCanvasAccessibleTest, PdfCanvasAccessible_QueryInterface) {
    PdfCanvas canvas;

    QAccessibleInterface* iface = QAccessible::queryAccessibleInterface(&canvas);

    ASSERT_NE(nullptr, iface);
    EXPECT_EQ(QAccessible::Canvas, iface->role());
    // Note: Do NOT delete — QAccessible manages the interface lifecycle internally
}

// ═══════════════════════════════════════════════════════════════════════
// GitHub #82 (WIN-51) — an accessibility UPDATE-EVENT recorder.
//
// The tests above assert the accessible TREE is correct: roles, names,
// descriptions, hit-testing. That is the static half. What none of them
// covers is whether the application actually NOTIFIES assistive technology
// when something changes.
//
// The distinction matters. A screen reader builds its model from the tree
// once and then relies on `QAccessible::updateAccessibility` events to learn
// that focus moved, a value changed, or a widget appeared. An application
// with a perfect tree and no events is silent to a screen-reader user after
// the first read: everything is described correctly and nothing is ever
// announced again.
//
// `QAccessible::installUpdateHandler` is the seam that makes this testable
// headlessly — it intercepts every event the framework would deliver, so no
// live assistive technology, and no live UI session, is required.
//
// The handler is process-global, so it is installed and REMOVED around the
// assertion; leaving it installed would make every later test in the binary
// feed this recorder.
// ═══════════════════════════════════════════════════════════════════════

namespace {

int g_a11yEventCount = 0;
QAccessible::Event g_lastA11yEvent = QAccessible::ObjectCreated;

void recordingUpdateHandler(QAccessibleEvent* ev)
{
    if (!ev) return;
    ++g_a11yEventCount;
    g_lastA11yEvent = ev->type();
}

/// RAII so the global handler cannot leak into unrelated tests even if an
/// assertion fails and unwinds early.
class A11yRecorder {
public:
    A11yRecorder() {
        g_a11yEventCount = 0;
        previous_ = QAccessible::installUpdateHandler(recordingUpdateHandler);
    }
    ~A11yRecorder() { QAccessible::installUpdateHandler(previous_); }

    int count() const { return g_a11yEventCount; }

private:
    QAccessible::UpdateHandler previous_ = nullptr;
};

}  // namespace

TEST_F(PdfCanvasAccessibleTest, ScriptedUiActionEmitsAnAccessibilityEvent)
{
    // Accessibility must be active or the framework short-circuits event
    // delivery entirely and this would assert nothing.
    QAccessible::setActive(true);
    ASSERT_TRUE(QAccessible::isActive())
        << "accessibility is inactive, so no events would be delivered and "
           "this test could not distinguish a silent application from a "
           "working one";

    A11yRecorder recorder;

    // A REAL scripted UI action first: build a focusable widget and move focus
    // to it. This is the half that tests the FRAMEWORK/PRODUCT rather than the
    // recorder -- Qt raises the accessibility event itself, nobody hands it in.
    //
    // Kept separate from the hand-raised events below, and asserted separately,
    // because conflating them would let the recorder's own correctness stand in
    // for the product actually emitting anything.
    QWidget host;
    host.resize(400, 300);
    host.setFocusPolicy(Qt::StrongFocus);
    host.show();
    host.setFocus(Qt::OtherFocusReason);
    QApplication::processEvents();

    const int afterRealAction = recorder.count();
    EXPECT_GE(afterRealAction, 1)
        << "a real focus change produced NO accessibility event. Qt raises "
           "these itself, so this is the product/framework being silent rather "
           "than the recorder failing -- a screen reader would not learn that "
           "focus moved.";

    QAccessibleEvent focusEvent(&host, QAccessible::Focus);
    QAccessible::updateAccessibility(&focusEvent);

    // NOTE: a plain QAccessibleEvent may NOT carry StateChanged -- Qt asserts
    // on it (qaccessible.h:312, "m_type != QAccessible::StateChanged"),
    // because that type requires the subclass that also says WHICH state
    // changed. A first version of this test used the base class and aborted
    // the whole test binary on a CRT assert, taking the run's summary with it.
    // Recorded because an aborted process reads like a hang, not like a bug in
    // one test.
    QAccessible::State changed;
    changed.focused = true;
    QAccessibleStateChangeEvent stateEvent(&host, changed);
    QAccessible::updateAccessibility(&stateEvent);

    EXPECT_GE(recorder.count(), 1)
        << "no accessibility update event was delivered during a scripted UI "
           "action. The accessible tree may be correct, but a screen reader "
           "would never be told anything changed -- the application is "
           "silent after its first read.";

    EXPECT_EQ(g_lastA11yEvent, QAccessible::StateChanged)
        << "events are being delivered but not the one that was raised, so "
           "the recorder is observing something other than what the product "
           "emits";
}

// The recorder must be capable of observing ZERO, or the assertion above
// could pass against a handler that counted unconditionally. Pins that the
// count reflects real deliveries rather than being incremented by setup.
TEST_F(PdfCanvasAccessibleTest, TheAccessibilityRecorderCanObserveNoEvents)
{
    QAccessible::setActive(true);
    A11yRecorder recorder;
    EXPECT_EQ(recorder.count(), 0)
        << "the recorder reported events before any were raised, so a "
           "non-zero count proves nothing";
}

// ============================================================================
// GitHub #225 -- a regression guard for kHighContrastFallbackRatio that does
// not depend on the OS High-Contrast setting of the machine running it.
//
// WHY THERE WAS NONE. isHighContrastMode() consults SystemParametersInfoW
// first, and on Windows that call SUCCEEDS, so the function returns before the
// palette fallback. The threshold constant is therefore unreachable in
// production on the only platform PDFE currently ships, and a mutation of it
// reddened nothing.
//
// The existing setHighContrastOverrideForTesting() cannot close this: it
// short-circuits the WHOLE function and returns a fixed bool, so it skips the
// very branch in question.
//
// Two entry points are covered below -- the pure helper, and the real
// isHighContrastMode() with the OS query forced unavailable -- because a test
// that only exercised the helper would not notice if production stopped
// calling it.
// ============================================================================

TEST(HighContrastFallback, NearMaximalPairIsHighContrastAndOrdinaryPalettesAreNot) {
    using pdfe::ui::AccessibilityHelper;

    // A real Windows High-Contrast theme: pure black and pure white, which is
    // the WCAG formula's ceiling of 21.0.
    EXPECT_TRUE(AccessibilityHelper::isHighContrastPalette(
        QColor(0, 0, 0), QColor(255, 255, 255)))
        << "pure black/white is ratio 21.0 -- the actual Windows HC themes -- "
           "and must classify as high contrast";
    EXPECT_TRUE(AccessibilityHelper::isHighContrastPalette(
        QColor(255, 255, 255), QColor(0, 0, 0)))
        << "the same pair reversed must classify identically";

    // The palettes PDFE itself ships. These are legible and well above WCAG
    // AAA, which is precisely why the old 7.0 threshold called everything
    // high-contrast; they must NOT be classified as HC now. The ratios are
    // quoted from the measurement recorded in AccessibilityHelper.cpp.
    EXPECT_FALSE(AccessibilityHelper::isHighContrastPalette(
        QColor(0xef, 0xef, 0xef), QColor(0, 0, 0)))
        << "Fusion light (black on #efefef, ratio ~18.3) is an ordinary "
           "readable palette, not a High-Contrast theme";
    EXPECT_FALSE(AccessibilityHelper::isHighContrastPalette(
        QColor(0x35, 0x35, 0x35), QColor(255, 255, 255)))
        << "PDFE dark (white on #353535, ratio ~12.3) is an ordinary readable "
           "palette, not a High-Contrast theme";

    // The discrimination this test exists for. 18.3 and 21.0 are BOTH high
    // ratios; a threshold anywhere below ~18.4 stops telling them apart, which
    // is the exact defect #217 found and #225 asks to be guarded. Mutating
    // kHighContrastFallbackRatio down to 7.0, 12.0 or 18.0 makes the two
    // EXPECT_FALSE cases above fail; mutating it above 21.0 makes the two
    // EXPECT_TRUE cases fail. The constant is pinned from both sides.
}

TEST(HighContrastFallback, IsHighContrastModeReachesTheFallbackWhenTheOsQueryIsUnavailable) {
    using pdfe::ui::AccessibilityHelper;
    if (!QApplication::instance()) {
        GTEST_SKIP() << "no QApplication: the fallback reads QApplication::palette()";
    }

    // Make the OS query behave as if it were unavailable, which is the only
    // way the palette branch is reachable on Windows. Without this the
    // assertions below would be measuring SystemParametersInfoW's answer --
    // i.e. the tester's own Windows setting -- rather than the constant.
    AccessibilityHelper::setOsQueryUnavailableForTesting(true);
    const QPalette saved = QApplication::palette();

    struct Restore {
        QPalette p;
        ~Restore() {
            QApplication::setPalette(p);
            pdfe::ui::AccessibilityHelper::clearOsQueryUnavailableForTesting();
        }
    } restore{saved};

    QPalette hc;
    hc.setColor(QPalette::Window, QColor(0, 0, 0));
    hc.setColor(QPalette::WindowText, QColor(255, 255, 255));
    QApplication::setPalette(hc);
    EXPECT_TRUE(AccessibilityHelper::isHighContrastMode())
        << "with the OS query unavailable and a pure black/white palette, the "
           "fallback must report high contrast";

    QPalette ordinary;
    ordinary.setColor(QPalette::Window, QColor(0xef, 0xef, 0xef));
    ordinary.setColor(QPalette::WindowText, QColor(0, 0, 0));
    QApplication::setPalette(ordinary);
    EXPECT_FALSE(AccessibilityHelper::isHighContrastMode())
        << "an ordinary readable palette must NOT be reported as high "
           "contrast -- that always-true behaviour was GitHub #217";
}
