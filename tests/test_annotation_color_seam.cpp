// -*- coding: utf-8 -*-
//
// Pipeline e4c71f3a phase-1 (GAP-103): Q_INVOKABLE annotation color
// seams that bypass QColorDialog::exec(). Unblocks 4 pywinauto
// annotation color tests that previously hung on modal dialog.

#include <gtest/gtest.h>

#include "annotations/AnnotationData.h"
#include "ui/AnnotationPropertiesWidget.h"

#include <QApplication>
#include <QColor>
#include <QMetaObject>
#include <QSignalSpy>

using namespace pdfe::ui;
using namespace pdfe::annotations;

TEST(AnnotationColorSeam, SetAnnotationColor_IsInvokable)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";

    AnnotationPropertiesWidget widget(nullptr);

    AnnotationData data;
    data.color = QColor(255, 0, 0);
    widget.setAnnotation(data);

    bool ok = QMetaObject::invokeMethod(
        &widget, "setAnnotationColor",
        Q_ARG(QColor, QColor(0, 128, 255)));

    EXPECT_TRUE(ok)
        << "setAnnotationColor must be callable via QMetaObject::invokeMethod";
    EXPECT_EQ(widget.currentData().color, QColor(0, 128, 255))
        << "Color must be updated after invokeMethod";
}

TEST(AnnotationColorSeam, SetAnnotationColor_EmitsPropertiesChanged)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";

    AnnotationPropertiesWidget widget(nullptr);

    AnnotationData data;
    data.color = QColor(255, 0, 0);
    widget.setAnnotation(data);

    QSignalSpy spy(&widget, &AnnotationPropertiesWidget::propertiesChanged);
    widget.setAnnotationColor(QColor(0, 255, 0));

    EXPECT_EQ(spy.count(), 1)
        << "setAnnotationColor must emit propertiesChanged exactly once";
}

TEST(AnnotationColorSeam, SetAnnotationColor_NoEmitOnSameColor)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";

    AnnotationPropertiesWidget widget(nullptr);

    AnnotationData data;
    data.color = QColor(100, 100, 100);
    widget.setAnnotation(data);

    QSignalSpy spy(&widget, &AnnotationPropertiesWidget::propertiesChanged);
    widget.setAnnotationColor(QColor(100, 100, 100));

    EXPECT_EQ(spy.count(), 0)
        << "Same color must not emit propertiesChanged";
}

TEST(AnnotationColorSeam, SetAnnotationTextColor_IsInvokable)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";

    AnnotationPropertiesWidget widget(nullptr);

    AnnotationData data;
    data.daColor = QColor(0, 0, 0);
    widget.setAnnotation(data);

    bool ok = QMetaObject::invokeMethod(
        &widget, "setAnnotationTextColor",
        Q_ARG(QColor, QColor(255, 0, 128)));

    EXPECT_TRUE(ok)
        << "setAnnotationTextColor must be callable via QMetaObject::invokeMethod";
    EXPECT_EQ(widget.currentData().daColor, QColor(255, 0, 128))
        << "Text color must be updated after invokeMethod";
}

TEST(AnnotationColorSeam, SetAnnotationTextColor_EmitsPropertiesChanged)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";

    AnnotationPropertiesWidget widget(nullptr);

    AnnotationData data;
    data.daColor = QColor(0, 0, 0);
    widget.setAnnotation(data);

    QSignalSpy spy(&widget, &AnnotationPropertiesWidget::propertiesChanged);
    widget.setAnnotationTextColor(QColor(255, 255, 0));

    EXPECT_EQ(spy.count(), 1)
        << "setAnnotationTextColor must emit propertiesChanged exactly once";
}

TEST(AnnotationColorSeam, SetAnnotationColor_InvalidColorIgnored)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";

    AnnotationPropertiesWidget widget(nullptr);

    AnnotationData data;
    data.color = QColor(50, 50, 50);
    widget.setAnnotation(data);

    QSignalSpy spy(&widget, &AnnotationPropertiesWidget::propertiesChanged);
    widget.setAnnotationColor(QColor());

    EXPECT_EQ(spy.count(), 0)
        << "Invalid color must be ignored";
    EXPECT_EQ(widget.currentData().color, QColor(50, 50, 50))
        << "Color must remain unchanged for invalid input";
}
