// -*- coding: utf-8 -*-
//
// Phase F8 / gap WIN-33 — the text-edit overlay must be sized for the monitor
// the EDITOR WINDOW is on, not for QGuiApplication::primaryScreen().
//
// Before the fix, TextEditTool::buildRunFont read the device-pixel ratio from
// primaryScreen() unconditionally, so on a multi-monitor desktop with mixed
// scaling (e.g. a 200% laptop panel as primary plus a 100% external monitor)
// the overlay font on the secondary monitor was sized with the wrong ratio —
// visibly too large or too small against the rendered page.
//
// HONESTY NOTE ON "TWO SYNTHETIC SCREENS":
//   Qt does not permit a test to fabricate a QScreen. QScreen instances are
//   created by the QPA plugin from real platform screens; the class has no
//   public constructor and no registration API, so an in-process test cannot
//   stand up a second synthetic display. What IS therefore tested here:
//     (a) the pure sizing helper, fed TWO synthetic screen ratios standing for
//         two monitors, produces the two different pixel sizes those monitors
//         imply  (OverlayPixelSizeFollowsTheRatioItIsGiven);
//     (b) the screen the production code resolves is the one reached through
//         the widget's OWN top-level window, asserted against
//         widget->window()->windowHandle()->screen() — and that the primary
//         screen is used only on the no-window-handle fallback
//         (ScreenComesFromTheWidgetsOwnWindow / FallsBackToPrimaryOnly...);
//     (c) buildRunFont's pixel size equals the size implied by the WIDGET'S
//         screen ratio  (BuildRunFontSizesForTheWidgetsScreen);
//     (d) when the host actually has two or more displays, the resolution is
//         additionally asserted against the real second screen
//         (RealSecondScreenIsFollowedWhenPresent — an in-test conditional, not
//         a skip; the test always runs and always asserts).
//   No claim is made that (a)-(c) constitute a genuine two-display run.

#include <gtest/gtest.h>

#include "textedit/TextEditTool.h"
#include "textedit/TextEditModel.h"

#include <QApplication>
#include <QGuiApplication>
#include <QScreen>
#include <QWidget>
#include <QWindow>

using namespace pdfe::textedit;

namespace {

TextSpan makeSpan(const std::string& fontName, float fontSize)
{
    TextSpan span;
    span.fontName = fontName;
    span.fontSize = fontSize;
    span.text = U"Test";
    span.originalText = U"Test";
    span.boundingBox = QRectF(0, 0, 100, 20);
    return span;
}

FormattingRun makeRun(float sizePt)
{
    FormattingRun run;
    run.charStart = 0;
    run.charEnd = 4;
    run.fontSize = sizePt;
    return run;
}

} // namespace

// (a) Two synthetic screen ratios standing in for two monitors: the sizing
//     arithmetic must follow whichever ratio it is handed. A 24pt run at 100%
//     zoom is 24 device-independent pixels on a 1.0x monitor and 12 on a 2.0x
//     one (the ratio is compensated for because the canvas already renders at
//     device resolution).
TEST(TextEditScreenDpr, OverlayPixelSizeFollowsTheRatioItIsGiven)
{
    constexpr float kPrimaryLikeDpr   = 2.0f;  // "monitor A" — 200% scaling
    constexpr float kSecondaryLikeDpr = 1.0f;  // "monitor B" — 100% scaling

    EXPECT_EQ(TextEditTool::overlayPixelSizeForDpr(24.0f, 1.0f, kSecondaryLikeDpr), 24);
    EXPECT_EQ(TextEditTool::overlayPixelSizeForDpr(24.0f, 1.0f, kPrimaryLikeDpr), 12);
    EXPECT_NE(TextEditTool::overlayPixelSizeForDpr(24.0f, 1.0f, kSecondaryLikeDpr),
              TextEditTool::overlayPixelSizeForDpr(24.0f, 1.0f, kPrimaryLikeDpr))
        << "the two monitors must not produce the same overlay size";

    // Zoom composes with the ratio.
    EXPECT_EQ(TextEditTool::overlayPixelSizeForDpr(24.0f, 2.0f, kPrimaryLikeDpr), 24);
    // Degenerate ratios are treated as 1.0, and the size never reaches zero.
    EXPECT_EQ(TextEditTool::overlayPixelSizeForDpr(10.0f, 1.0f, 0.0f), 10);
    EXPECT_EQ(TextEditTool::overlayPixelSizeForDpr(0.01f, 1.0f, 1.0f), 1);
}

// devicePixelRatioForScreen is pure over a QScreen* and clamps sanely.
TEST(TextEditScreenDpr, DevicePixelRatioForScreenIsScreenDrivenAndClamped)
{
    EXPECT_FLOAT_EQ(TextEditTool::devicePixelRatioForScreen(nullptr), 1.0f)
        << "a missing screen must degrade to 1.0, never to 0";

    for (QScreen* s : QGuiApplication::screens()) {
        ASSERT_NE(s, nullptr);
        EXPECT_FLOAT_EQ(TextEditTool::devicePixelRatioForScreen(s),
                        static_cast<float>(s->devicePixelRatio()))
            << "the helper must report the ratio of the screen it is given";
    }
}

// (b) The production resolution goes through the widget's OWN window.
TEST(TextEditScreenDpr, ScreenComesFromTheWidgetsOwnWindow)
{
    QWidget top;
    top.setObjectName("dprTestWindow");
    top.resize(200, 120);
    auto* child = new QWidget(&top);
    child->setObjectName("dprTestCanvas");
    child->setGeometry(10, 10, 100, 60);
    top.show();
    QApplication::processEvents();

    QWindow* handle = top.windowHandle();
    ASSERT_NE(handle, nullptr) << "top-level window has no platform handle";
    QScreen* windowScreen = handle->screen();
    ASSERT_NE(windowScreen, nullptr);

    // Resolved from the child widget: it must walk up to the top-level window
    // and report THAT window's screen.
    EXPECT_EQ(TextEditTool::screenForWidget(child), windowScreen)
        << "the screen must be the one the widget's own window is on";
    EXPECT_EQ(TextEditTool::screenForWidget(&top), windowScreen);
    EXPECT_FLOAT_EQ(TextEditTool::devicePixelRatioForWidget(child),
                    static_cast<float>(windowScreen->devicePixelRatio()));

    top.hide();
}

// (b, other branch) The primary screen is the FALLBACK only.
TEST(TextEditScreenDpr, FallsBackToPrimaryOnlyWhenThereIsNoWindow)
{
    EXPECT_EQ(TextEditTool::screenForWidget(nullptr), nullptr)
        << "no widget means no window screen — the caller must fall back";

    QWidget neverShown;              // constructed but never shown
    EXPECT_EQ(TextEditTool::screenForWidget(&neverShown), nullptr)
        << "a widget with no platform window yet has no screen of its own";

    const float primaryDpr =
        TextEditTool::devicePixelRatioForScreen(QGuiApplication::primaryScreen());
    EXPECT_FLOAT_EQ(TextEditTool::devicePixelRatioForWidget(nullptr), primaryDpr);
    EXPECT_FLOAT_EQ(TextEditTool::devicePixelRatioForWidget(&neverShown), primaryDpr);
}

// (c) buildRunFont sizes against the WIDGET's screen ratio, not the primary's.
TEST(TextEditScreenDpr, BuildRunFontSizesForTheWidgetsScreen)
{
    QWidget top;
    top.setObjectName("dprRunFontWindow");
    top.resize(200, 120);
    top.show();
    QApplication::processEvents();
    ASSERT_NE(top.windowHandle(), nullptr);
    QScreen* windowScreen = top.windowHandle()->screen();
    ASSERT_NE(windowScreen, nullptr);

    const TextSpan span = makeSpan("Arial", 12.0f);
    const FormattingRun run = makeRun(18.0f);
    constexpr float kZoom = 1.0f;

    const QFont font = TextEditTool::buildRunFont(
        run, span, kZoom, QStringLiteral("Arial"), 0.0f, &top);

    const float widgetDpr =
        TextEditTool::devicePixelRatioForScreen(windowScreen);
    EXPECT_EQ(font.pixelSize(),
              TextEditTool::overlayPixelSizeForDpr(18.0f, kZoom, widgetDpr))
        << "the run font must be sized for the ratio of the window's screen";

    top.hide();
}

// (d) When the host really has more than one display, assert against the real
//     second screen. This is an in-test conditional, never a skip: the test
//     body always executes and always asserts something.
TEST(TextEditScreenDpr, RealSecondScreenIsFollowedWhenPresent)
{
    const QList<QScreen*> screens = QGuiApplication::screens();
    ASSERT_FALSE(screens.isEmpty()) << "no screens at all";
    QScreen* primary = QGuiApplication::primaryScreen();
    ASSERT_NE(primary, nullptr);

    QScreen* other = nullptr;
    for (QScreen* s : screens) {
        if (s != primary) { other = s; break; }
    }

    QWidget top;
    top.setObjectName("dprSecondScreenWindow");
    top.resize(200, 120);
    top.show();
    QApplication::processEvents();
    ASSERT_NE(top.windowHandle(), nullptr);

    if (other) {
        // Move the window onto the non-primary display and confirm the
        // resolution follows it there.
        top.windowHandle()->setScreen(other);
        top.setGeometry(QRect(other->geometry().topLeft() + QPoint(20, 20),
                              QSize(200, 120)));
        QApplication::processEvents();

        EXPECT_EQ(TextEditTool::screenForWidget(&top), other)
            << "a window moved to the secondary display must resolve to it";
        EXPECT_FLOAT_EQ(TextEditTool::devicePixelRatioForWidget(&top),
                        static_cast<float>(other->devicePixelRatio()));
    } else {
        // Single-display host: the honest assertion available is that the
        // resolved screen is the window's own screen object, reached through
        // the window rather than by asking for the primary screen.
        EXPECT_EQ(TextEditTool::screenForWidget(&top),
                  top.windowHandle()->screen());
    }

    // Holds on both hosts: an unrealised window never claims a screen.
    QWidget unrealised;
    EXPECT_EQ(TextEditTool::screenForWidget(&unrealised), nullptr);

    top.hide();
}
