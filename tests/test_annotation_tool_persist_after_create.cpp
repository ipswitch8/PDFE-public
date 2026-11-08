// -*- coding: utf-8 -*-
// GitHub #366 — toolbar annotation tools deselected after one use.
//
// THE DEFECT. MainWindow::onAnnotationCreatedByTool() force-switched to the
// Select tool after EVERY annotation creation:
//
//     selectToolAction_->setChecked(true);
//     onAnnotationToolSelected(selectToolAction_);
//
// The annotation tools are checkable members of the EXCLUSIVE annotToolGroup_,
// so forcing Select checked unchecked the originating tool. The toolbar button
// "deselected after one use" and every tool behaved as a one-shot: the user had
// to re-click the tool before each additional annotation.
//
// THE FIX. The slot no longer switches tools; it keeps the originating tool
// active/checked and populates the Properties panel directly from the new
// annotation's data (decoupled from any tool change).
//
// THE ORACLE. Activate the Highlight tool, drive the REAL slot via the
// triggerAnnotationCreatedByToolForTesting seam against an already-created
// annotation, and assert the group's checked action is UNCHANGED — still the
// Highlight tool, not the Select tool. Pre-fix the checked action would be
// selectToolAction after the call.
#include <gtest/gtest.h>

#include "annotations/AnnotationManager.h"
#include "core/Document.h"
#include "core/Page.h"
#include "ui/MainWindow.h"

#include <QAction>
#include <QApplication>
#include <QCoreApplication>
#include <QRectF>
#include <QString>

#include <memory>

using pdfe::ui::MainWindow;

namespace {

void settle() {
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();
}

QString fixture(const char* name) {
    return QStringLiteral(TEST_FIXTURES_DIR "/") + QString::fromUtf8(name);
}

} // namespace

class AnnotationToolPersistTest : public ::testing::Test {};

TEST_F(AnnotationToolPersistTest, OriginatingToolStaysCheckedAfterCreation) {
    MainWindow mw;
    mw.openFile(fixture("test_single.pdf"));
    settle();
    ASSERT_NE(mw.documentForTesting(), nullptr) << "fixture failed to open";
    ASSERT_GT(mw.documentForTesting()->pageCount(), 0);

    auto* highlight = mw.findChild<QAction*>(QStringLiteral("highlightToolAction"));
    auto* select    = mw.findChild<QAction*>(QStringLiteral("selectToolAction"));
    ASSERT_NE(highlight, nullptr);
    ASSERT_NE(select, nullptr);

    // Activate the Highlight tool through the real exclusive-group path.
    highlight->trigger();
    settle();
    ASSERT_TRUE(highlight->isChecked())
        << "precondition: the Highlight tool must be active before creation";
    ASSERT_EQ(mw.annotToolGroupCheckedActionForTesting(), highlight)
        << "precondition: the group's checked action must be the Highlight tool";

    // Create a real annotation so onAnnotationCreatedByTool can snapshot it by
    // index (it reads annotationManager_->getAnnotation(page, data.annotIndex)).
    auto* manager = mw.annotationManagerForTesting();
    ASSERT_NE(manager, nullptr);
    std::unique_ptr<pdfe::core::Page> page = mw.documentForTesting()->page(0);
    ASSERT_NE(page, nullptr);

    const QRectF rect(72.0, 72.0, 180.0, 24.0);
    pdfe::annotations::AnnotationData data =
        manager->createAnnotation(page.get(),
                                  pdfe::annotations::AnnotationType::Highlight,
                                  rect);
    data.pageIndex = 0;

    // Drive the production slot exactly as PdfCanvas would after a tool created
    // an annotation.
    mw.triggerAnnotationCreatedByToolForTesting(0, data);
    settle();

    // THE ASSERTIONS #366 is about: the originating tool is still active and the
    // group's checked action did not flip to Select.
    EXPECT_TRUE(highlight->isChecked())
        << "GitHub #366: the Highlight tool deselected after one creation — a "
           "toolbar annotation tool must stay active so the user can place more "
           "than one annotation without re-clicking the tool.";
    EXPECT_FALSE(select->isChecked())
        << "GitHub #366: the slot force-switched to the Select tool after "
           "creation, which is the one-shot behaviour being removed.";
    EXPECT_EQ(mw.annotToolGroupCheckedActionForTesting(), highlight)
        << "the exclusive group's checked action changed after an annotation "
           "was created; it must remain the originating annotation tool.";
    EXPECT_NE(mw.annotToolGroupCheckedActionForTesting(), select);
}
