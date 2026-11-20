// -*- coding: utf-8 -*-
// WrappingToolBar — a QToolBar that WRAPS its buttons onto additional rows when
// they exceed the toolbar width, instead of hiding the overflow behind the
// stock QToolBar extension ("chevron") button.
//
// form-field-bugs phase-10 (bug 7): the annotation toolbar (~16 actions + a
// colour picker) is the toolbar that actually shows the chevron in ordinary
// use. A stock QToolBar has no native row-wrapping and no public hook to
// replace its single-row QToolBarLayout. This subclass therefore keeps being a
// real QToolBar that OWNS its real QActions — so QToolBar::actions()/clear()/
// addAction(), ToolbarLayoutManager::captureFromToolbar/applyLayoutToToolbar,
// buildToolbarActionRegistry() and findChild<QToolBar*>(name) all keep working
// UNCHANGED — but it intercepts actionEvent() and routes the generated buttons
// into an internal FlowToolBar (FlowLayout) child that reflows them to rows.
//
// Because actionEvent() is handled here and NOT forwarded to the base, the
// base QToolBarLayout stays empty: there is never an extension button, so
// nothing is ever hidden behind a chevron. The QAction list (QWidget::actions())
// is maintained by QWidget::addAction/removeAction BEFORE the event is
// delivered, so it stays correct for the customization/registry machinery.
//
// Separators and QWidgetAction widgets (e.g. the colour picker) are hosted too,
// preserving order, separators, objectNames and accessibleNames.
#pragma once

#include <QToolBar>
#include <QHash>

class QAction;
class QActionEvent;
class QResizeEvent;
class QWidget;

namespace pdfe::ui {

class FlowToolBar;

class WrappingToolBar : public QToolBar {
    Q_OBJECT

public:
    explicit WrappingToolBar(const QString& title, QWidget* parent = nullptr);
    explicit WrappingToolBar(QWidget* parent = nullptr);

    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int width) const override;
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

    /// Test seam: the internal wrapping container.
    FlowToolBar* flowBarForTesting() const { return flow_; }

protected:
    void actionEvent(QActionEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    void init();
    void layoutFlow();

    FlowToolBar* flow_ = nullptr;
    QHash<QAction*, QWidget*> widgetForAction_;
};

} // namespace pdfe::ui
