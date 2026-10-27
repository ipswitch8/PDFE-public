// GitHub #303 — see TestPipeFocusGuard.h for the measurement and the reasoning.

#include "TestPipeFocusGuard.h"

#include <QApplication>

namespace pdfe::ui {

void TestPipeFocusGuard::attach()
{
    // `QApplication::focusChanged` is the only signal that reports the widget
    // focus came FROM. `QWidget::focusInEvent` on the pipe would tell us focus
    // arrived but not where to send it back to, and by then the previous
    // widget is already unobtainable.
    connect(qApp, &QApplication::focusChanged, this,
            [this](QWidget* previous, QWidget* now) {
                onFocusChanged(previous, now);
            });
}

QWidget* TestPipeFocusGuard::onFocusChanged(QWidget* previous, QWidget* now)
{
    // Leave every ordinary focus change strictly alone. This is the whole
    // safety argument for installing the guard unconditionally: in a build
    // with no test pipes, `isTestPipe` never matches and this returns here
    // every time.
    if (restoring_ || !isTestPipe(now)) return nullptr;

    QWidget* target = restoreTarget(previous, fallback_.data());
    if (!target) {
        // Nothing sensible to restore to. Leave focus where it is rather than
        // forcing it somewhere arbitrary -- a guard that invents a target
        // replaces one wrong answer with another, and a test asserting against
        // it would be measuring the guard rather than the product.
        return nullptr;
    }

    restoring_ = true;
    // Qt::OtherFocusReason: this is not a click, a tab or a shortcut, and
    // mislabelling it could change how a focus-reason-sensitive widget
    // behaves. The restore should be invisible to everything except the
    // keystroke that follows it.
    target->setFocus(Qt::OtherFocusReason);
    restoring_ = false;
    return target;
}

}  // namespace pdfe::ui
