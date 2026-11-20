// -*- coding: utf-8 -*-
// FlowLayout — a QLayout that arranges its child items left-to-right and wraps
// them onto additional rows when the available width is exceeded, growing in
// height instead of clipping or hiding overflow.
//
// form-field-bugs phase-10 (bug 7): a stock QToolBar collapses overflow into an
// extension ("chevron") button, hiding buttons the window is too narrow to show
// in one row. This layout is the wrapping primitive used by FlowToolBar to show
// every button by default, flowing to extra rows rather than hiding any.
//
// This is the well-known Qt "Flow Layout" example (BSD-licensed, shipped with
// Qt) adapted into the pdfe::ui namespace with project conventions. It reports
// hasHeightForWidth()/heightForWidth() so a width shrink reflows to more rows
// and the owning widget's height grows by one row-height per wrap — which is
// exactly what the headless geometry tests assert.
#pragma once

#include <QLayout>
#include <QLayoutItem>
#include <QRect>
#include <QSize>
#include <QStyle>
#include <QList>

namespace pdfe::ui {

class FlowLayout : public QLayout {
    Q_OBJECT

public:
    explicit FlowLayout(QWidget* parent, int margin = -1,
                        int hSpacing = -1, int vSpacing = -1);
    explicit FlowLayout(int margin = -1, int hSpacing = -1, int vSpacing = -1);
    ~FlowLayout() override;

    void addItem(QLayoutItem* item) override;
    int horizontalSpacing() const;
    int verticalSpacing() const;
    Qt::Orientations expandingDirections() const override;
    bool hasHeightForWidth() const override;
    int heightForWidth(int width) const override;
    int count() const override;
    QLayoutItem* itemAt(int index) const override;
    QLayoutItem* takeAt(int index) override;
    QSize minimumSize() const override;
    void setGeometry(const QRect& rect) override;
    QSize sizeHint() const override;

private:
    int doLayout(const QRect& rect, bool testOnly) const;
    int smartSpacing(QStyle::PixelMetric pm) const;

    QList<QLayoutItem*> itemList_;
    int hSpace_;
    int vSpace_;
};

} // namespace pdfe::ui
