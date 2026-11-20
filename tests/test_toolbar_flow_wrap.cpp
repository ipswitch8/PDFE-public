// -*- coding: utf-8 -*-
// form-field-bugs phase-10 (bug 7): toolbar buttons that exceed the toolbar
// width must WRAP to additional rows (all buttons visible by default), not be
// hidden behind a QToolBar overflow/extension chevron.
//
// These are headless GoogleTest geometry oracles over the wrapping primitives
// FlowLayout / FlowToolBar (src/ui/FlowLayout.*, src/ui/FlowToolBar.*). They
// construct a FlowToolBar, add buttons that exceed a single-row width, shrink
// the width, and assert with REAL geometry (distinct top-edge Y rows; per-row
// button height growth) that:
//   (1) when wide, everything sits on one row;
//   (2) when narrowed, additional rows are created (rowCount > 1) and the
//       required height grows by roughly one button-height per wrap;
//   (3) EVERY expected button remains present and visible — none is hidden
//       behind an extension/chevron (addressed by its stable objectName).
//
// No chevron is poked and no coordinates are guessed: geometry is read straight
// off the widgets after a layout pass, which is exactly what a stock QToolBar
// cannot provide (its overflow collapses into an extension button instead).
#include <gtest/gtest.h>

#include "ui/FlowLayout.h"
#include "ui/FlowToolBar.h"
#include "ui/WrappingToolBar.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QObject>
#include <QPushButton>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QToolButton>
#include <QWidget>

#include <memory>

using namespace pdfe::ui;

namespace {

void ensureApp() {
    if (!QCoreApplication::instance()) {
        static int argc = 1;
        static const char* argv0 = "test_toolbar_flow_wrap";
        static char* argv[] = {const_cast<char*>(argv0), nullptr};
        // Leaked intentionally for the life of the test process.
        new QApplication(argc, argv);
    }
}

// Build a FlowToolBar with a fixed set of named, text-beside-icon buttons that
// mirrors the form toolbar's eight field buttons (the real overflow case this
// phase fixes). Returns the bar (owned by the caller via unique_ptr) and the
// list of button object names added.
struct BuiltBar {
    std::unique_ptr<FlowToolBar> bar;
    QStringList buttonObjectNames; // the "<name>Button" ids
};

BuiltBar buildBar() {
    BuiltBar out;
    out.bar = std::make_unique<FlowToolBar>();
    out.bar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);

    const QStringList labels = {
        "Text Field", "Checkbox",    "Radio Button", "Combo Box",
        "List Box",   "Push Button", "Signature",    "Delete Field"};
    const QStringList objNames = {
        "formTextFieldAction", "formCheckboxAction", "formRadioButtonAction",
        "formComboBoxAction",  "formListBoxAction",  "formPushButtonAction",
        "formSignatureAction", "formDeleteFieldAction"};

    for (int i = 0; i < labels.size(); ++i) {
        auto* act = new QAction(labels[i], out.bar.get());
        act->setObjectName(objNames[i]);
        out.bar->addAction(act);
        out.buttonObjectNames.push_back(objNames[i] + "Button");
    }
    return out;
}

// A width comfortably larger than the sum of all button widths, so everything
// fits on a single row.
int wideEnoughFor(const FlowToolBar& bar) {
    int sum = 40;
    for (QToolButton* b : bar.toolButtons())
        sum += b->sizeHint().width() + 8;
    return sum;
}

} // namespace

class ToolbarFlowWrapTest : public ::testing::Test {
protected:
    void SetUp() override { ensureApp(); }
};

// (1) Wide: every button on a single row, in ascending X, sharing one Y.
TEST_F(ToolbarFlowWrapTest, SingleRowWhenWide) {
    BuiltBar built = buildBar();
    FlowToolBar* bar = built.bar.get();
    ASSERT_EQ(bar->toolButtons().size(), 8);

    bar->resize(wideEnoughFor(*bar), 400);
    bar->relayout();

    EXPECT_EQ(bar->rowCount(), 1)
        << "with ample width all buttons should sit on one row";

    const auto btns = bar->toolButtons();
    for (int i = 1; i < btns.size(); ++i) {
        EXPECT_EQ(btns[i]->geometry().y(), btns[0]->geometry().y())
            << "button " << i << " should share the first row's Y when wide";
        EXPECT_GT(btns[i]->geometry().x(), btns[i - 1]->geometry().x())
            << "buttons should advance left-to-right on a single row";
    }
}

// (2) Narrow: buttons wrap to more than one row — distinct top-edge Y values.
TEST_F(ToolbarFlowWrapTest, WrapsToMultipleRowsWhenNarrow) {
    BuiltBar built = buildBar();
    FlowToolBar* bar = built.bar.get();

    // Roughly two buttons wide forces several rows.
    const int twoButtons = bar->toolButtons()[0]->sizeHint().width() * 2 + 24;
    bar->resize(twoButtons, 800);
    bar->relayout();

    EXPECT_GT(bar->rowCount(), 1)
        << "narrowing below the single-row width must create additional rows "
           "instead of hiding buttons behind a chevron";

    // Prove the rows are genuinely distinct Y bands, not a reported count.
    QSet<int> distinctYs;
    for (QToolButton* b : bar->toolButtons())
        distinctYs.insert(b->geometry().y());
    EXPECT_GT(distinctYs.size(), 1)
        << "wrapped buttons must occupy more than one distinct Y row";
}

// (2b) Required height grows by ~one button-height per wrapped row.
TEST_F(ToolbarFlowWrapTest, HeightGrowsWhenButtonsWrap) {
    BuiltBar built = buildBar();
    FlowToolBar* bar = built.bar.get();

    const int wide = wideEnoughFor(*bar);
    const int narrow = bar->toolButtons()[0]->sizeHint().width() * 2 + 24;

    const int hWide = bar->heightForWidth(wide);
    const int hNarrow = bar->heightForWidth(narrow);
    const int rowH = bar->toolButtons()[0]->sizeHint().height();

    ASSERT_GT(rowH, 0);
    EXPECT_GE(hNarrow, hWide + rowH)
        << "a wrap to >=2 rows should add at least one button-height to the "
           "bar's required height (wide=" << hWide << " narrow=" << hNarrow
        << " rowH=" << rowH << ")";
}

// (3) After wrapping, EVERY expected button is still present and visible — none
// dropped behind an extension/chevron. Checked by stable objectName and real
// geometry inside the widget bounds.
TEST_F(ToolbarFlowWrapTest, AllButtonsRemainVisibleAfterWrap) {
    BuiltBar built = buildBar();
    FlowToolBar* bar = built.bar.get();

    const int narrow = bar->toolButtons()[0]->sizeHint().width() + 20; // 1/row
    bar->resize(narrow, 2000);
    bar->relayout();

    // Every button resolves by its automation objectName (none hidden/dropped).
    for (const QString& id : built.buttonObjectNames) {
        QToolButton* b = bar->findChild<QToolButton*>(id);
        ASSERT_NE(b, nullptr)
            << "button '" << id.toStdString()
            << "' is missing — it would be the chevron-hidden case this phase "
               "fixes";
        EXPECT_FALSE(b->isHidden())
            << "button '" << id.toStdString() << "' must not be hidden";
        // Its laid-out rect must sit within the bar's width (not pushed off
        // into an overflow area).
        EXPECT_GE(b->geometry().left(), 0);
        EXPECT_LE(b->geometry().right(), bar->width() + 1)
            << "button '" << id.toStdString()
            << "' spilled outside the bar width instead of wrapping";
    }

    // Count matches what we added — nothing silently removed.
    EXPECT_EQ(bar->toolButtons().size(), built.buttonObjectNames.size());
}

// (3b) Each produced button carries an accessibleName (automation-friendly per
// CLAUDE.md) so pywinauto/UIA can address it without a chevron gesture.
TEST_F(ToolbarFlowWrapTest, ButtonsAreAutomationAddressable) {
    BuiltBar built = buildBar();
    FlowToolBar* bar = built.bar.get();
    for (QToolButton* b : bar->toolButtons()) {
        EXPECT_FALSE(b->accessibleName().isEmpty())
            << "every wrapping button needs an accessibleName for UIA";
        ASSERT_NE(b->defaultAction(), nullptr)
            << "button must carry its QAction as defaultAction (the deferred "
               "naming pass and command wiring rely on it)";
    }
}

// ---------------------------------------------------------------------------
// WrappingToolBar — the real QToolBar subclass used for the annotation (and
// main) toolbars: it owns real QActions (so capture/registry/customization keep
// working) yet wraps its buttons to rows with NO extension chevron.
// ---------------------------------------------------------------------------

namespace {

// Mirror the annotation toolbar: ~16 checkable tool actions in an action group,
// a couple of separators, and a colour-picker widget added via addWidget().
struct BuiltWrap {
    std::unique_ptr<WrappingToolBar> bar;
    QStringList actionObjectNames; // the raw action object names
    QStringList buttonObjectNames; // the "<name>Button" ids
};

BuiltWrap buildWrap() {
    BuiltWrap out;
    out.bar = std::make_unique<WrappingToolBar>(QStringLiteral("Annotation"));
    out.bar->setObjectName("annotationToolBar"); // the real production name
    out.bar->setMovable(false);
    out.bar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);

    auto* group = new QActionGroup(out.bar.get());

    const QStringList labels = {
        "Select",   "Sticky Note", "Highlight",  "Underline", "Strike Out",
        "Free Text", "Rectangle",  "Circle",     "Line",      "Ink",
        "Link",     "Image",       "Text Edit",  "Redact",    "Form",
        "Stamp"};
    const QStringList objNames = {
        "selectToolAction",    "stickyNoteToolAction", "highlightToolAction",
        "underlineToolAction", "strikeOutToolAction",  "freeTextToolAction",
        "rectangleToolAction", "circleToolAction",     "lineToolAction",
        "inkToolAction",       "linkToolAction",       "imageToolAction",
        "textEditToolAction",  "redactToolAction",     "formToolAction",
        "stampToolAction"};

    for (int i = 0; i < labels.size(); ++i) {
        auto* act = new QAction(labels[i], out.bar.get());
        act->setObjectName(objNames[i]);
        act->setCheckable(true);
        act->setStatusTip(labels[i]);
        group->addAction(act);
        out.bar->addAction(act);
        if (i == 4 || i == 9)
            out.bar->addSeparator();
        out.actionObjectNames.push_back(objNames[i]);
        out.buttonObjectNames.push_back(objNames[i] + "Button");
    }

    // Colour-picker widget, exactly as createAnnotationToolBar() adds it.
    auto* colorBtn = new QPushButton();
    colorBtn->setObjectName("annotColorPickerButton");
    colorBtn->setFixedSize(32, 32);
    out.bar->addWidget(colorBtn);

    return out;
}

bool hasVisibleExtensionChevron(const WrappingToolBar* bar) {
    // QToolBar's extension button is a private "QToolBarExtension" child. We
    // bypass the base QToolBarLayout, so it must never become visible.
    for (QObject* child : bar->children()) {
        auto* w = qobject_cast<QWidget*>(child);
        if (!w)
            continue;
        if (QString::fromLatin1(w->metaObject()->className())
                == QLatin1String("QToolBarExtension")
            && w->isVisible()) {
            return true;
        }
    }
    return false;
}

} // namespace

// The real QActions survive on the toolbar — this is what the customization /
// registry machinery (captureFromToolbar reads actions()) depends on.
TEST_F(ToolbarFlowWrapTest, WrappingToolBarKeepsItsRealActions) {
    BuiltWrap built = buildWrap();
    WrappingToolBar* bar = built.bar.get();

    int named = 0;
    for (QAction* a : bar->actions()) {
        if (a && !a->objectName().isEmpty() && !a->isSeparator())
            ++named;
    }
    // 16 tool actions resolve by objectName on the toolbar itself (the colour
    // picker's QWidgetAction and the separators are unnamed, as in production).
    EXPECT_EQ(named, built.actionObjectNames.size())
        << "every tool QAction must remain on the QToolBar's action list so "
           "ToolbarLayoutManager::captureFromToolbar() still sees it";
}

// The annotation toolbar wraps to >1 row when narrow, with every button visible
// and NO extension chevron.
TEST_F(ToolbarFlowWrapTest, WrappingToolBarWrapsAnnotationActionsToRows) {
    BuiltWrap built = buildWrap();
    WrappingToolBar* bar = built.bar.get();
    FlowToolBar* flow = bar->flowBarForTesting();
    ASSERT_NE(flow, nullptr);

    // Roughly three buttons wide forces several rows for 16 buttons.
    const int threeButtons =
        flow->toolButtons().isEmpty()
            ? 300
            : flow->toolButtons()[0]->sizeHint().width() * 3 + 40;
    bar->resize(threeButtons, 1200);

    EXPECT_GT(flow->rowCount(), 1)
        << "the annotation toolbar must wrap its buttons to additional rows "
           "when narrow, not hide them behind a chevron";

    QSet<int> distinctYs;
    for (QToolButton* b : flow->toolButtons())
        distinctYs.insert(b->geometry().y());
    EXPECT_GT(distinctYs.size(), 1)
        << "wrapped annotation buttons must occupy >1 distinct Y row";

    EXPECT_FALSE(hasVisibleExtensionChevron(bar))
        << "no QToolBarExtension chevron may be visible — overflow must wrap";
}

// Every annotation action button remains present, visible, and within the bar
// width after wrapping (none dropped behind an extension area).
TEST_F(ToolbarFlowWrapTest, WrappingToolBarShowsEveryButtonAfterWrap) {
    BuiltWrap built = buildWrap();
    WrappingToolBar* bar = built.bar.get();
    FlowToolBar* flow = bar->flowBarForTesting();
    ASSERT_NE(flow, nullptr);

    const int oneButton =
        flow->toolButtons()[0]->sizeHint().width() + 24; // 1 per row
    bar->resize(oneButton, 4000);
    flow->relayout();

    for (const QString& id : built.buttonObjectNames) {
        QToolButton* b = bar->findChild<QToolButton*>(id);
        ASSERT_NE(b, nullptr)
            << "annotation button '" << id.toStdString()
            << "' missing — the chevron-hidden case bug 7 fixes";
        EXPECT_FALSE(b->isHidden())
            << "annotation button '" << id.toStdString() << "' must be visible";
        EXPECT_LE(b->geometry().right(), flow->width() + 1)
            << "annotation button '" << id.toStdString()
            << "' spilled outside the bar instead of wrapping";
    }

    // The colour-picker widget is hosted too (added via addWidget()).
    EXPECT_NE(bar->findChild<QPushButton*>("annotColorPickerButton"), nullptr)
        << "the colour-picker widget must be hosted in the wrapping flow";

    EXPECT_FALSE(hasVisibleExtensionChevron(bar));
}
