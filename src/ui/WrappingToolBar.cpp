// -*- coding: utf-8 -*-
// See WrappingToolBar.h.
#include "ui/WrappingToolBar.h"
#include "ui/FlowToolBar.h"

#include <QActionEvent>
#include <QResizeEvent>
#include <QToolButton>
#include <QWidgetAction>

namespace pdfe::ui {

WrappingToolBar::WrappingToolBar(const QString& title, QWidget* parent)
    : QToolBar(title, parent) {
    init();
}

WrappingToolBar::WrappingToolBar(QWidget* parent) : QToolBar(parent) {
    init();
}

void WrappingToolBar::init() {
    flow_ = new FlowToolBar(this);
    flow_->setObjectName("wrappingToolBarFlow");
    // The host area hands the toolbar a width; the flow fills it and wraps.
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
}

void WrappingToolBar::actionEvent(QActionEvent* event) {
    // Deliberately NOT forwarding to QToolBar::actionEvent: that would populate
    // the base QToolBarLayout (single row + extension chevron). We host the
    // buttons in our FlowToolBar instead. QWidget::actions() is already updated
    // by the time this handler runs, so the customization/registry code that
    // reads actions() is unaffected.
    QAction* action = event->action();
    switch (event->type()) {
    case QEvent::ActionAdded: {
        QWidget* w = nullptr;
        if (auto* wa = qobject_cast<QWidgetAction*>(action)) {
            w = wa->defaultWidget();
            if (w)
                flow_->addWidget(w);
        } else if (action->isSeparator()) {
            w = flow_->addSeparator();
        } else {
            w = flow_->addAction(action);
        }
        if (w)
            widgetForAction_.insert(action, w);
        layoutFlow();
        break;
    }
    case QEvent::ActionRemoved: {
        QWidget* w = widgetForAction_.take(action);
        if (w) {
            flow_->removeHostedWidget(w);
            if (qobject_cast<QWidgetAction*>(action)) {
                // The widget is owned by the QWidgetAction; just detach it.
                w->hide();
            } else {
                // Our own QToolButton / separator QFrame.
                w->deleteLater();
            }
        }
        layoutFlow();
        break;
    }
    case QEvent::ActionChanged:
        layoutFlow();
        break;
    default:
        break;
    }
}

void WrappingToolBar::layoutFlow() {
    if (!flow_)
        return;
    flow_->setGeometry(contentsRect());
    // The wrapped height may have changed — ask the parent area to re-fit so the
    // toolbar row grows/shrinks to show every row of buttons.
    updateGeometry();
}

void WrappingToolBar::resizeEvent(QResizeEvent* event) {
    QToolBar::resizeEvent(event);
    layoutFlow();
}

int WrappingToolBar::heightForWidth(int width) const {
    if (!flow_)
        return QToolBar::sizeHint().height();
    const QMargins m = contentsMargins();
    const int inner = qMax(1, width - m.left() - m.right());
    return flow_->heightForWidth(inner) + m.top() + m.bottom();
}

QSize WrappingToolBar::sizeHint() const {
    if (!flow_)
        return QToolBar::sizeHint();
    const QMargins m = contentsMargins();
    const QSize natural = flow_->sizeHint();
    // Height is computed for the width we currently have, so a narrower toolbar
    // reports the taller, wrapped height and the host area allocates the rows.
    const int w = width() > 0 ? width()
                              : natural.width() + m.left() + m.right();
    const int h = heightForWidth(w);
    return QSize(natural.width() + m.left() + m.right(), h);
}

QSize WrappingToolBar::minimumSizeHint() const {
    if (!flow_)
        return QToolBar::minimumSizeHint();
    // One button wide: the host never decides the bar "overflows".
    const QMargins m = contentsMargins();
    const QSize minInner = flow_->minimumSizeHint();
    return QSize(minInner.width() + m.left() + m.right(),
                heightForWidth(width() > 0 ? width()
                                           : minInner.width()));
}

} // namespace pdfe::ui
