// -*- coding: utf-8 -*-
// Pipeline G follow-up: lock the MainWindow ↔ AttachmentPanel
// integration that was missing at Pipeline G phase-12 closeout.
// Before this work the AttachmentPanel widget existed in
// src/ui/AttachmentPanel.{h,cpp} but was never instantiated in
// MainWindow, so pywinauto couldn't reach it via UIA.
//
// These tests verify the wiring at the construction layer:
//   - MainWindow exposes attachmentPanel_ (objectName "attachmentPanel")
//   - MainWindow exposes attachmentPanelDock_ (objectName "attachmentPanelDock")
//   - MainWindow exposes the View > Attachments Panel toggle action
//     (objectName "toggleAttachmentPanelAction")
//   - The dock starts hidden by default (auto-shows on doc-open if
//     /EmbeddedFiles non-empty; otherwise toggled via View menu).
#include <gtest/gtest.h>

#include "ui/MainWindow.h"
#include "ui/AttachmentPanel.h"

#include <QAction>
#include <QDockWidget>

using namespace pdfe::ui;

TEST(AttachmentPanelMainWindowIntegration, PanelIsConstructedWithObjectName)
{
    MainWindow w;
    auto* panel = w.findChild<AttachmentPanel*>("attachmentPanel");
    EXPECT_NE(panel, nullptr)
        << "MainWindow must instantiate AttachmentPanel and set its "
           "objectName so pywinauto can reach it via UIA AutomationId";
}

TEST(AttachmentPanelMainWindowIntegration, DockHasStableObjectName)
{
    MainWindow w;
    auto* dock = w.findChild<QDockWidget*>("attachmentPanelDock");
    ASSERT_NE(dock, nullptr);
    // Dock starts hidden — auto-shows on doc-open if /EmbeddedFiles
    // is non-empty, or via View > Attachments Panel.
    EXPECT_FALSE(dock->isVisible());
}

TEST(AttachmentPanelMainWindowIntegration, ViewMenuToggleActionExists)
{
    MainWindow w;
    auto* a = w.findChild<QAction*>("toggleAttachmentPanelAction");
    ASSERT_NE(a, nullptr);
    EXPECT_TRUE(a->isCheckable());
    // The dock starts hidden, so the toggle should start unchecked
    // — visibilityChanged(false) syncs through to setChecked(false).
    EXPECT_FALSE(a->isChecked());
}

TEST(AttachmentPanelMainWindowIntegration, DockVisibilityChangedSyncsToggleAction)
{
    // Verify the reverse-sync wiring: dock->visibilityChanged is
    // connected to the toggle action's setChecked so the menu
    // checkbox stays consistent when the dock is shown/hidden by
    // any path. This is the wiring the View menu integration relies
    // on. Direct visibility round-tripping under the offscreen Qt
    // platform is platform-specific (the platform doesn't composite,
    // so isVisible() can lag); checking the action checkbox sync is
    // the platform-stable observable.
    MainWindow w;
    w.show();
    auto* dock = w.findChild<QDockWidget*>("attachmentPanelDock");
    auto* a = w.findChild<QAction*>("toggleAttachmentPanelAction");
    ASSERT_NE(dock, nullptr);
    ASSERT_NE(a, nullptr);

    // Force visibilityChanged emissions and observe the action follows.
    dock->setVisible(true);
    EXPECT_TRUE(a->isChecked())
        << "toggle action must follow dock-shown via visibilityChanged "
           "connection";
    dock->setVisible(false);
    EXPECT_FALSE(a->isChecked())
        << "toggle action must follow dock-hidden via visibilityChanged "
           "connection";
}
