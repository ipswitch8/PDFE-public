// -*- coding: utf-8 -*-
// FlowToolBar — a QToolBar-like button container whose buttons WRAP onto
// additional rows when the width is exceeded, instead of being hidden behind a
// QToolBar extension ("chevron") button.
//
// form-field-bugs phase-10 (bug 7): the reported defect is that toolbar buttons
// which exceed the toolbar width are hidden behind the stock QToolBar overflow
// chevron rather than remaining visible. A stock QToolBar has no native
// row-wrapping. FlowToolBar hosts its QToolButtons in a FlowLayout (see
// FlowLayout.h) so every button stays visible by default and the toolbar grows
// in height (extra rows) as it narrows.
//
// API parity with the slice of QToolBar used by MainWindow's contextual
// toolbars: addAction(QAction*)/addAction(text), addSeparator(), addWidget(),
// setToolButtonStyle(). Buttons are real QToolButtons carrying the QAction as
// their defaultAction(), so the existing deferred accessible-name pass
// (findChildren<QToolButton*>() -> defaultAction()) and the automation-id
// conventions (objectName = action objectName + "Button") keep working.
//
// Automation-friendly per CLAUDE.md: every produced QToolButton gets a stable
// accessibleName (the action text) and objectName (action objectName) so
// pywinauto/UIA can address it without a chevron gesture.
#pragma once

#include <QWidget>
#include <QList>
#include <Qt>

class QAction;
class QToolButton;

namespace pdfe::ui {

class FlowLayout;

class FlowToolBar : public QWidget {
    Q_OBJECT

public:
    explicit FlowToolBar(QWidget* parent = nullptr);

    /// Host an existing QAction as a wrapping QToolButton. Returns the button.
    QToolButton* addAction(QAction* action);
    /// Convenience mirror of QToolBar::addAction(text): creates a QAction
    /// (parented to this bar) and its button, and returns the action.
    QAction* addAction(const QString& text);
    /// A vertical separator that participates in wrapping like a button.
    /// Returns the separator widget so a host can track/remove it.
    QWidget* addSeparator();
    /// Host an arbitrary widget (e.g. a colour-picker button) in the flow.
    void addWidget(QWidget* widget);
    /// Remove a hosted widget from the flow WITHOUT deleting it (the caller
    /// owns the lifetime). Erases it from the tool-button bookkeeping too.
    void removeHostedWidget(QWidget* widget);

    void setToolButtonStyle(Qt::ToolButtonStyle style);
    Qt::ToolButtonStyle toolButtonStyle() const { return buttonStyle_; }

    /// Every tool button currently hosted, in insertion order.
    QList<QToolButton*> toolButtons() const;

    /// Number of distinct rows the hosted buttons currently occupy (distinct
    /// top-edge Y coordinates). 1 when everything fits on a single row; grows
    /// as the bar narrows and buttons wrap. Forces a layout pass first so the
    /// value is current for the bar's present width.
    int rowCount() const;

    /// Force the flow layout to re-run for the current width. Useful in a
    /// headless test after resize() when the widget is never shown.
    void relayout();

private:
    FlowLayout* flow_ = nullptr;
    Qt::ToolButtonStyle buttonStyle_ = Qt::ToolButtonTextBesideIcon;
    QList<QToolButton*> buttons_;
};

} // namespace pdfe::ui
