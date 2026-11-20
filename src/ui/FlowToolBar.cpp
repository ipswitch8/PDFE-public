// -*- coding: utf-8 -*-
// See FlowToolBar.h.
#include "ui/FlowToolBar.h"
#include "ui/FlowLayout.h"

#include <QAction>
#include <QFrame>
#include <QSet>
#include <QToolButton>

namespace pdfe::ui {

FlowToolBar::FlowToolBar(QWidget* parent) : QWidget(parent) {
    flow_ = new FlowLayout(this, /*margin=*/2, /*hSpacing=*/2, /*vSpacing=*/2);
    setLayout(flow_);
    // Expanding horizontally so a host QToolBar hands the bar its full row
    // width (FlowLayout::minimumSize is only one button wide, so the host never
    // decides the bar "overflows" and never shows its own chevron); Minimum
    // vertically so the bar grows downward by a row each time its buttons wrap.
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
}

QToolButton* FlowToolBar::addAction(QAction* action) {
    auto* btn = new QToolButton(this);
    btn->setDefaultAction(action);
    btn->setToolButtonStyle(buttonStyle_);
    btn->setAutoRaise(true);
    // Automation-friendly, mirroring the QToolBar deferred-naming convention so
    // pywinauto/UIA can address the button without expanding a chevron.
    const QString name = action->text().remove(QLatin1Char('&'));
    if (!name.isEmpty())
        btn->setAccessibleName(name);
    if (!action->statusTip().isEmpty())
        btn->setAccessibleDescription(action->statusTip());
    if (!action->objectName().isEmpty())
        btn->setObjectName(action->objectName() + "Button");
    flow_->addWidget(btn);
    buttons_.append(btn);
    return btn;
}

QAction* FlowToolBar::addAction(const QString& text) {
    auto* action = new QAction(text, this);
    addAction(action);
    return action;
}

QWidget* FlowToolBar::addSeparator() {
    auto* sep = new QFrame(this);
    sep->setObjectName("flowToolBarSeparator");
    sep->setFrameShape(QFrame::VLine);
    sep->setFrameShadow(QFrame::Sunken);
    flow_->addWidget(sep);
    return sep;
}

void FlowToolBar::addWidget(QWidget* widget) {
    if (!widget)
        return;
    widget->setParent(this);
    flow_->addWidget(widget);
}

void FlowToolBar::removeHostedWidget(QWidget* widget) {
    if (!widget || !flow_)
        return;
    for (int i = 0; i < flow_->count(); ++i) {
        QLayoutItem* item = flow_->itemAt(i);
        if (item && item->widget() == widget) {
            delete flow_->takeAt(i);  // deletes the layout item, not the widget
            break;
        }
    }
    buttons_.removeAll(qobject_cast<QToolButton*>(widget));
    widget->setParent(nullptr);
}

void FlowToolBar::setToolButtonStyle(Qt::ToolButtonStyle style) {
    buttonStyle_ = style;
    for (QToolButton* btn : buttons_)
        btn->setToolButtonStyle(style);
}

QList<QToolButton*> FlowToolBar::toolButtons() const {
    return buttons_;
}

void FlowToolBar::relayout() {
    if (flow_)
        flow_->activate();
}

int FlowToolBar::rowCount() const {
    const_cast<FlowToolBar*>(this)->relayout();
    QSet<int> rows;
    for (QToolButton* btn : buttons_) {
        if (!btn || btn->isHidden())
            continue;
        rows.insert(btn->geometry().y());
    }
    return rows.size();
}

} // namespace pdfe::ui
