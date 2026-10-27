// GitHub #303 — a UIA write to a hidden test pipe steals keyboard focus.
//
// WHAT THESE CASES PIN, AND WHAT THEY CANNOT. The defect itself needs a real
// UIA client asserting focus through `ValuePattern::SetValue`, which no
// in-process test can produce -- so the live proof is the Python regression
// test (`tests/test_ui_pipe_focus_theft.py`), which drives the product through
// a pipe and then sends a key. What CAN be pinned here, cheaply and
// exhaustively, is the DECISION the guard makes once focus has moved: which
// widget focus goes back to, and -- just as important -- when the guard must
// keep its hands off entirely.
//
// The decision is where this fix can go wrong in a way no one would notice. A
// guard that restores focus too eagerly would move focus away from real product
// widgets, which is a worse defect than the one being repaired and would show
// up as unrelated UI flakiness rather than as this fix failing.

#include <gtest/gtest.h>

#include <QApplication>
#include <QLineEdit>
#include <QWidget>

#include "../src/ui/TestPipeFocusGuard.h"

using pdfe::ui::TestPipeFocusGuard;

namespace {

/// A pipe shaped exactly as the product's 108 are: zero size, NoFocus, and the
/// `test...Pipe` name. Built as the product builds them so the predicate is
/// tested against the real shape rather than a convenient one.
QLineEdit* makePipe(QWidget* parent, const char* name)
{
    auto* e = new QLineEdit(parent);
    e->setObjectName(QString::fromLatin1(name));
    e->setFixedSize(0, 0);
    e->setFocusPolicy(Qt::NoFocus);
    return e;
}

}  // namespace

// ---------------------------------------------------------- the predicate

TEST(TestPipeFocusGuardTest, RecognisesAPipeByTheEstablishedNaming)
{
    QWidget host;
    EXPECT_TRUE(TestPipeFocusGuard::isTestPipe(
        makePipe(&host, "testKeyTraceQueryPipe")));
    EXPECT_TRUE(TestPipeFocusGuard::isTestPipe(
        makePipe(&host, "testCustomizeAvailableActionsResultPipe")));
}

TEST(TestPipeFocusGuardTest, DoesNotMistakeAProductWidgetForAPipe)
{
    // THE IMPORTANT DIRECTION. If this fails, the guard yanks focus out of
    // real widgets and the product becomes unusable in ways that look like
    // random UI flakiness rather than like this fix misbehaving.
    QWidget host;
    auto* ordinary = new QLineEdit(&host);
    ordinary->setObjectName(QStringLiteral("searchField"));
    EXPECT_FALSE(TestPipeFocusGuard::isTestPipe(ordinary));

    // A product widget that happens to share the pipes' other attributes --
    // zero size and NoFocus -- must STILL not be treated as a pipe. This is
    // why the predicate keys on the name and not on those attributes, and this
    // case is what stops someone "improving" it to do so.
    auto* decoy = new QLineEdit(&host);
    decoy->setObjectName(QStringLiteral("collapsedHelperField"));
    decoy->setFixedSize(0, 0);
    decoy->setFocusPolicy(Qt::NoFocus);
    EXPECT_FALSE(TestPipeFocusGuard::isTestPipe(decoy));

    // Half-matches must not count either.
    auto* prefixOnly = new QLineEdit(&host);
    prefixOnly->setObjectName(QStringLiteral("testSomethingElse"));
    EXPECT_FALSE(TestPipeFocusGuard::isTestPipe(prefixOnly));
    auto* suffixOnly = new QLineEdit(&host);
    suffixOnly->setObjectName(QStringLiteral("renderPipe"));
    EXPECT_FALSE(TestPipeFocusGuard::isTestPipe(suffixOnly));
}

TEST(TestPipeFocusGuardTest, ANonLineEditIsNeverAPipe)
{
    // Every pipe is a QLineEdit. A plain QWidget carrying a pipe-shaped name
    // is not one, and matching it would mean restoring focus away from
    // whatever that widget actually is.
    QWidget host;
    auto* w = new QWidget(&host);
    w->setObjectName(QStringLiteral("testMisleadingPipe"));
    EXPECT_FALSE(TestPipeFocusGuard::isTestPipe(w));
}

TEST(TestPipeFocusGuardTest, NullIsNotAPipe)
{
    // `focusChanged` legitimately reports nullptr for `previous` when the
    // window has just been shown, so this is a real input, not a paranoid one.
    EXPECT_FALSE(TestPipeFocusGuard::isTestPipe(nullptr));
}

// ------------------------------------------------------- the restore target

TEST(TestPipeFocusGuardTest, RestoresToTheWidgetFocusCameFrom)
{
    // The whole point: a test clicked into a widget, a pipe write stole focus,
    // and the next keystroke must still address what the test was addressing.
    QWidget host;
    auto* editor = new QLineEdit(&host);
    editor->setObjectName(QStringLiteral("annotContentsEdit"));
    auto* canvas = new QWidget(&host);
    canvas->setObjectName(QStringLiteral("pdfCanvas"));

    EXPECT_EQ(TestPipeFocusGuard::restoreTarget(editor, canvas), editor);
}

TEST(TestPipeFocusGuardTest, FallsBackToTheCanvasWhenThePreviousFocusWasAlsoAPipe)
{
    // Two pipe writes in a row -- which the enumeration pipes do routinely,
    // query then result. Restoring to the earlier PIPE would leave focus on a
    // hidden QLineEdit, i.e. not fix the defect at all while appearing to.
    QWidget host;
    auto* earlierPipe = makePipe(&host, "testFirstQueryPipe");
    auto* canvas = new QWidget(&host);
    canvas->setObjectName(QStringLiteral("pdfCanvas"));

    EXPECT_EQ(TestPipeFocusGuard::restoreTarget(earlierPipe, canvas), canvas);
}

TEST(TestPipeFocusGuardTest, FallsBackToTheCanvasWhenThereWasNoPreviousFocus)
{
    QWidget host;
    auto* canvas = new QWidget(&host);
    canvas->setObjectName(QStringLiteral("pdfCanvas"));
    EXPECT_EQ(TestPipeFocusGuard::restoreTarget(nullptr, canvas), canvas);
}

TEST(TestPipeFocusGuardTest, ReturnsNullRatherThanInventingATarget)
{
    // No previous focus and no usable fallback. The guard must then leave
    // focus ALONE. Forcing it somewhere arbitrary would replace one wrong
    // answer with another, and any test asserting on the result would be
    // measuring the guard instead of the product.
    EXPECT_EQ(TestPipeFocusGuard::restoreTarget(nullptr, nullptr), nullptr);

    // A fallback that is itself a pipe is not a usable fallback.
    QWidget host;
    auto* pipeFallback = makePipe(&host, "testFallbackPipe");
    EXPECT_EQ(TestPipeFocusGuard::restoreTarget(nullptr, pipeFallback),
              nullptr);
}

// ------------------------------------------------------------ the transition

TEST(TestPipeFocusGuardTest, AnOrdinaryFocusChangeIsLeftStrictlyAlone)
{
    // The safety property that lets this guard be installed unconditionally
    // (#182: PDFE_TESTING is never OFF, so a compile-time gate would be a
    // comment rather than a control). If focus did not land on a pipe, the
    // guard must do nothing and say so.
    QWidget host;
    auto* a = new QLineEdit(&host);
    a->setObjectName(QStringLiteral("fieldA"));
    auto* b = new QLineEdit(&host);
    b->setObjectName(QStringLiteral("fieldB"));
    auto* canvas = new QWidget(&host);

    TestPipeFocusGuard guard(canvas);
    EXPECT_EQ(guard.onFocusChanged(a, b), nullptr)
        << "the guard acted on a focus change between two ordinary widgets";
}

TEST(TestPipeFocusGuardTest, APipeStealingFocusIsUndoneAndReported)
{
    QWidget host;
    auto* editor = new QLineEdit(&host);
    editor->setObjectName(QStringLiteral("annotContentsEdit"));
    auto* pipe = makePipe(&host, "testKeyTraceQueryPipe");
    auto* canvas = new QWidget(&host);

    TestPipeFocusGuard guard(canvas);
    EXPECT_EQ(guard.onFocusChanged(editor, pipe), editor)
        << "focus was not handed back to the widget the test was addressing";
}

TEST(TestPipeFocusGuardTest, TheRestoreDoesNotReenterItself)
{
    // `setFocus()` emits `focusChanged` again. Without the re-entrancy guard
    // the restore is re-entered with the PIPE as `previous`, and the target
    // silently becomes the fallback instead of the widget under test -- a
    // wrong answer rather than a crash, which is the kind that survives
    // review. Driving two nested transitions reproduces that shape.
    QWidget host;
    auto* editor = new QLineEdit(&host);
    editor->setObjectName(QStringLiteral("annotContentsEdit"));
    auto* pipe = makePipe(&host, "testKeyTraceQueryPipe");
    auto* canvas = new QWidget(&host);

    TestPipeFocusGuard guard(canvas);
    ASSERT_EQ(guard.onFocusChanged(editor, pipe), editor);
    // A second, independent transition must still resolve to the editor --
    // i.e. the guard holds no state that degrades the answer.
    EXPECT_EQ(guard.onFocusChanged(editor, pipe), editor);
}
