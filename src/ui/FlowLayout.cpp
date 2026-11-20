// -*- coding: utf-8 -*-
// See FlowLayout.h. Adapted from the Qt "Flow Layout" example.
#include "ui/FlowLayout.h"

#include <QWidget>

namespace pdfe::ui {

FlowLayout::FlowLayout(QWidget* parent, int margin, int hSpacing, int vSpacing)
    : QLayout(parent), hSpace_(hSpacing), vSpace_(vSpacing) {
    setContentsMargins(margin, margin, margin, margin);
}

FlowLayout::FlowLayout(int margin, int hSpacing, int vSpacing)
    : hSpace_(hSpacing), vSpace_(vSpacing) {
    setContentsMargins(margin, margin, margin, margin);
}

FlowLayout::~FlowLayout() {
    QLayoutItem* item;
    while ((item = takeAt(0)))
        delete item;
}

void FlowLayout::addItem(QLayoutItem* item) {
    itemList_.append(item);
}

int FlowLayout::horizontalSpacing() const {
    if (hSpace_ >= 0)
        return hSpace_;
    return smartSpacing(QStyle::PM_LayoutHorizontalSpacing);
}

int FlowLayout::verticalSpacing() const {
    if (vSpace_ >= 0)
        return vSpace_;
    return smartSpacing(QStyle::PM_LayoutVerticalSpacing);
}

int FlowLayout::count() const {
    return itemList_.size();
}

QLayoutItem* FlowLayout::itemAt(int index) const {
    return itemList_.value(index);
}

QLayoutItem* FlowLayout::takeAt(int index) {
    if (index >= 0 && index < itemList_.size())
        return itemList_.takeAt(index);
    return nullptr;
}

Qt::Orientations FlowLayout::expandingDirections() const {
    return {};
}

bool FlowLayout::hasHeightForWidth() const {
    return true;
}

int FlowLayout::heightForWidth(int width) const {
    return doLayout(QRect(0, 0, width, 0), true);
}

void FlowLayout::setGeometry(const QRect& rect) {
    QLayout::setGeometry(rect);
    doLayout(rect, false);
}

QSize FlowLayout::sizeHint() const {
    // Preferred = everything on a single row. The owning widget therefore asks
    // for the full one-row width; when it is given less, heightForWidth() wraps
    // to extra rows. (minimumSize() stays one item wide, so a host toolbar never
    // decides the bar "overflows" and never shows a chevron.)
    int w = 0;
    int h = 0;
    const int spacing = qMax(0, horizontalSpacing());
    for (const QLayoutItem* item : itemList_) {
        const QSize hint = item->sizeHint();
        if (w > 0)
            w += spacing;
        w += hint.width();
        h = qMax(h, hint.height());
    }
    const QMargins margins = contentsMargins();
    return QSize(w + margins.left() + margins.right(),
                 h + margins.top() + margins.bottom());
}

QSize FlowLayout::minimumSize() const {
    QSize size;
    for (const QLayoutItem* item : itemList_)
        size = size.expandedTo(item->minimumSize());

    const QMargins margins = contentsMargins();
    size += QSize(margins.left() + margins.right(),
                  margins.top() + margins.bottom());
    return size;
}

int FlowLayout::doLayout(const QRect& rect, bool testOnly) const {
    int left, top, right, bottom;
    getContentsMargins(&left, &top, &right, &bottom);
    const QRect effectiveRect = rect.adjusted(left, top, -right, -bottom);
    int x = effectiveRect.x();
    int y = effectiveRect.y();
    int lineHeight = 0;

    for (QLayoutItem* item : itemList_) {
        const QWidget* wid = item->widget();
        int spaceX = horizontalSpacing();
        if (spaceX == -1 && wid)
            spaceX = wid->style()->layoutSpacing(
                QSizePolicy::ToolButton, QSizePolicy::ToolButton,
                Qt::Horizontal);
        int spaceY = verticalSpacing();
        if (spaceY == -1 && wid)
            spaceY = wid->style()->layoutSpacing(
                QSizePolicy::ToolButton, QSizePolicy::ToolButton,
                Qt::Vertical);

        int nextX = x + item->sizeHint().width() + spaceX;
        if (nextX - spaceX > effectiveRect.right() + 1 && lineHeight > 0) {
            x = effectiveRect.x();
            y = y + lineHeight + spaceY;
            nextX = x + item->sizeHint().width() + spaceX;
            lineHeight = 0;
        }

        if (!testOnly)
            item->setGeometry(QRect(QPoint(x, y), item->sizeHint()));

        x = nextX;
        lineHeight = qMax(lineHeight, item->sizeHint().height());
    }
    return y + lineHeight - rect.y() + bottom;
}

int FlowLayout::smartSpacing(QStyle::PixelMetric pm) const {
    QObject* parent = this->parent();
    if (!parent) {
        return -1;
    } else if (parent->isWidgetType()) {
        QWidget* pw = static_cast<QWidget*>(parent);
        return pw->style()->pixelMetric(pm, nullptr, pw);
    } else {
        return static_cast<QLayout*>(parent)->spacing();
    }
}

} // namespace pdfe::ui
