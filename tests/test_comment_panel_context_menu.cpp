// -*- coding: utf-8 -*-
// Pipeline G phase-5: CommentPanel context-menu UIA invariants.
//
// The Reply / Delete actions are constructed dynamically inside
// CommentPanel::onContextMenu() at right-click time. This phase
// extracted the populateCommentContextMenu(QMenu*) helper so the
// menu structure can be exercised without driving an actual
// right-click. UIA reads QAction::text() as the Name property and
// objectName() as the AutomationId — those are the strings the
// pywinauto matrix relies on.

#include <gtest/gtest.h>
#include "ui/CommentPanel.h"

#include <QAction>
#include <QMenu>
#include <QString>

using namespace pdfe::ui;

class CommentContextMenuTest : public ::testing::Test {
protected:
    void SetUp() override {
        panel = std::make_unique<CommentPanel>();
    }

    void TearDown() override {
        panel.reset();
    }

    QAction* findActionByObjectName(const QMenu& menu, const QString& name)
    {
        for (QAction* a : menu.actions()) {
            if (a && a->objectName() == name) return a;
        }
        return nullptr;
    }

    std::unique_ptr<CommentPanel> panel;
};

TEST_F(CommentContextMenuTest, MenuObjectNameAndAccessibleName) {
    QMenu menu;
    panel->populateCommentContextMenu(&menu);
    EXPECT_EQ(menu.objectName(), QString("commentContextMenu"));
    EXPECT_EQ(menu.accessibleName(), QString("Comment Context Menu"));
}

TEST_F(CommentContextMenuTest, MenuHasReplyAndDeleteActions) {
    QMenu menu;
    panel->populateCommentContextMenu(&menu);
    EXPECT_NE(findActionByObjectName(menu, "commentReplyAction"), nullptr);
    EXPECT_NE(findActionByObjectName(menu, "commentDeleteAction"), nullptr);
    // Exactly these two actions, no extras.
    EXPECT_EQ(menu.actions().size(), 2);
}

TEST_F(CommentContextMenuTest, ReplyActionText) {
    QMenu menu;
    panel->populateCommentContextMenu(&menu);
    auto* a = findActionByObjectName(menu, "commentReplyAction");
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(a->text(), QString("Reply"));
}

TEST_F(CommentContextMenuTest, DeleteActionText) {
    QMenu menu;
    panel->populateCommentContextMenu(&menu);
    auto* a = findActionByObjectName(menu, "commentDeleteAction");
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(a->text(), QString("Delete"));
}
