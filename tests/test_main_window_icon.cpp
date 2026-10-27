// -*- coding: utf-8 -*-
//
// Phase-5 criterion 2(c): MainWindow::windowIcon() returns a non-null
// QIcon after construction. This locks the invariant that the explicit
// setWindowIcon(pdfeAppIcon()) call lives in MainWindow's constructor
// and runs before the first show() — the fix for the title-bar icon
// regression investigated and recorded in Phase-4.
//
// We test the Qt object state directly here (independent of the live-
// process Win32 path covered by tests/test_ui_titlebar_icon.py) so the
// invariant is locked even when the pywinauto path skips under RDP.

#include <gtest/gtest.h>

#include <QApplication>
#include <QIcon>
#include <QPixmap>
#include <QSize>
#include <memory>

#include "ui/MainWindow.h"

class MainWindowIconTest : public ::testing::Test {
};

TEST_F(MainWindowIconTest, WindowIconIsNonNullAfterConstruction) {
    // Construct a MainWindow on the heap; QApplication is owned by
    // test_main.cpp's GoogleTest/Qt::Test bootstrap, so we don't
    // touch app lifecycle here.
    auto window = std::make_unique<pdfe::ui::MainWindow>();
    QIcon ic = window->windowIcon();
    EXPECT_FALSE(ic.isNull())
        << "MainWindow has a null windowIcon — title bar will fall "
           "back to IDI_APPLICATION. Constructor must call "
           "setWindowIcon(pdfeAppIcon()) before the first show().";
    QPixmap pm = ic.pixmap(QSize(16, 16));
    EXPECT_FALSE(pm.isNull())
        << "MainWindow windowIcon cannot render at 16x16 — DWM "
           "uses the small icon for the title-bar render.";
}

TEST_F(MainWindowIconTest, AppIconHelperMatchesWindowIcon) {
    // Sanity: the helper used inside the constructor and the
    // window's resolved icon should produce the same rendering at
    // the title-bar size. This catches a divergence between
    // main.cpp's QApplication::setWindowIcon and the per-window
    // setWindowIcon — they should both come from pdfeAppIcon().
    auto window = std::make_unique<pdfe::ui::MainWindow>();
    QIcon helper = pdfe::ui::pdfeAppIcon();
    QIcon win = window->windowIcon();
    ASSERT_FALSE(helper.isNull());
    ASSERT_FALSE(win.isNull());

    QPixmap helperPm = helper.pixmap(QSize(32, 32));
    QPixmap winPm = win.pixmap(QSize(32, 32));
    ASSERT_FALSE(helperPm.isNull());
    ASSERT_FALSE(winPm.isNull());
    EXPECT_EQ(helperPm.toImage(), winPm.toImage())
        << "MainWindow's resolved windowIcon does not render the "
           "same pixels as pdfeAppIcon() — the constructor's "
           "setWindowIcon call may have used a different source "
           "or the helper was changed without updating the ctor.";
}
