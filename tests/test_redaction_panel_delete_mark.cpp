// -*- coding: utf-8 -*-
//
// D2 phase-5: UI-RED-06 — RedactionPanel delete button.
//
// Locks the contract that clicking "Delete Selected" on a populated
// RedactionPanel: (a) emits deleteMarkRequested with the correct
// (pageIndex, annotIndex) payload from the selected row, and (b) the
// panel's row count decreases when the caller (MainWindow in
// production) responds by calling setMarks() with a smaller list.
//
// The panel itself does NOT remove rows on delete-click — it only
// emits a signal. Production deletion happens at the MainWindow ↔
// RedactionManager layer; the panel re-populates from the post-delete
// mark list. This test verifies both halves: signal correctness AND
// repopulation correctness.
//
// Test fixtures: none required — the panel is exercised in isolation
// with synthetic RedactionData entries.

#include <gtest/gtest.h>

#include "ui/RedactionPanel.h"
#include "redaction/RedactionData.h"

#include <QApplication>
#include <QPushButton>
#include <QSignalSpy>
#include <QTest>
#include <QTreeWidget>

#include <vector>

using namespace pdfe::ui;
using namespace pdfe::redaction;

namespace {

// Ensure a QApplication exists for widget tests. Reused across cases.
class QAppFixture {
public:
    QAppFixture() {
        if (QCoreApplication::instance() == nullptr) {
            static int argc = 1;
            static char arg0[] = "test";
            static char* argv[] = {arg0, nullptr};
            app_ = std::make_unique<QApplication>(argc, argv);
        }
    }
private:
    std::unique_ptr<QApplication> app_;
};

RedactionData makeMark(int pageIndex, int annotIndex,
                       float x, float y, float w, float h)
{
    RedactionData d;
    d.pageIndex = pageIndex;
    d.annotIndex = annotIndex;
    d.rect = QRectF(x, y, w, h);
    d.fillColor = QColor(Qt::black);
    d.enabled = true;
    return d;
}

class RedactionPanelDeleteMarkTest : public ::testing::Test {
protected:
    QAppFixture qapp_;
    std::unique_ptr<RedactionPanel> panel_;

    void SetUp() override {
        panel_ = std::make_unique<RedactionPanel>();
        // Construction-only — no parent, no show. Internal widgets
        // exist in the unparented panel.
        ASSERT_NE(panel_, nullptr);
    }

    QTreeWidget* tree() {
        return panel_->findChild<QTreeWidget*>("redactionMarksList");
    }

    QPushButton* deleteButton() {
        return panel_->findChild<QPushButton*>("deleteRedactionButton");
    }
};

// ─────────────────────────────────────────────────────────────────────
// UI-RED-06 — clicking "Delete Selected" emits deleteMarkRequested
// with the selected mark's (pageIndex, annotIndex). Panel row count
// decreases when caller responds with a smaller setMarks() list.
// ─────────────────────────────────────────────────────────────────────
TEST_F(RedactionPanelDeleteMarkTest, DeleteHandlerEmitsSignalAndRowCountDecreases)
{
    auto* treeWidget = tree();
    auto* delButton = deleteButton();
    ASSERT_NE(treeWidget, nullptr) << "Internal QTreeWidget not found";
    ASSERT_NE(delButton, nullptr) << "Internal delete QPushButton not found";

    // Populate the panel with three marks across two pages.
    std::vector<RedactionData> marks = {
        makeMark(/*pageIndex=*/0, /*annotIndex=*/0, 100, 100, 50, 20),
        makeMark(/*pageIndex=*/0, /*annotIndex=*/1, 200, 100, 50, 20),
        makeMark(/*pageIndex=*/1, /*annotIndex=*/0,  50, 250, 80, 30),
    };
    panel_->setMarks(marks);
    EXPECT_EQ(treeWidget->topLevelItemCount(), 3)
        << "Panel did not render all 3 supplied marks.";

    // Select the second mark (pageIndex=0, annotIndex=1) AND fire the
    // itemClicked path so the panel's internal state (current item +
    // delete-button-enabled flag) reflects an interactive selection.
    treeWidget->setCurrentItem(treeWidget->topLevelItem(1));
    QMetaObject::invokeMethod(
        panel_.get(), "onItemClicked", Qt::DirectConnection,
        Q_ARG(QTreeWidgetItem*, treeWidget->topLevelItem(1)),
        Q_ARG(int, 0));
    EXPECT_TRUE(delButton->isEnabled())
        << "Delete button should be enabled after an item is clicked.";

    // Spy the deletion signal.
    QSignalSpy deleteSpy(panel_.get(), &RedactionPanel::deleteMarkRequested);
    ASSERT_TRUE(deleteSpy.isValid());

    // Click the delete button — fires onDeleteClicked which emits
    // deleteMarkRequested with the selected row's payload.
    QTest::mouseClick(delButton, Qt::LeftButton);

    // Verify signal payload.
    ASSERT_EQ(deleteSpy.count(), 1) << "deleteMarkRequested fired "
        << deleteSpy.count() << " times, expected 1.";
    const QList<QVariant>& args = deleteSpy.first();
    ASSERT_EQ(args.size(), 2);
    EXPECT_EQ(args.at(0).toInt(), 0)
        << "deleteMarkRequested pageIndex mismatch.";
    EXPECT_EQ(args.at(1).toInt(), 1)
        << "deleteMarkRequested annotIndex mismatch.";

    // Simulate MainWindow's response: caller removes the mark from
    // its list and re-populates the panel with the smaller list.
    std::vector<RedactionData> remaining;
    for (const auto& m : marks) {
        if (m.pageIndex == 0 && m.annotIndex == 1) continue;
        remaining.push_back(m);
    }
    panel_->setMarks(remaining);

    // Row count must decrease.
    EXPECT_EQ(treeWidget->topLevelItemCount(), 2)
        << "Panel row count did not decrease after the caller's "
           "post-delete setMarks() call. Expected 2, got "
        << treeWidget->topLevelItemCount() << ".";
}

// Regression: clicking Delete with no selection MUST NOT emit
// deleteMarkRequested. Without a guard, the panel would emit a
// signal with garbage (default-constructed) (pageIndex, annotIndex).
TEST_F(RedactionPanelDeleteMarkTest, DeleteWithNoSelectionEmitsNothing)
{
    auto* delButton = deleteButton();
    ASSERT_NE(delButton, nullptr);

    panel_->setMarks({});  // empty
    QSignalSpy deleteSpy(panel_.get(), &RedactionPanel::deleteMarkRequested);

    // Click delete with no selection. Button is disabled in this
    // state, but make sure the underlying handler is also safe even
    // if the disabled state is overridden.
    QMetaObject::invokeMethod(panel_.get(), "onDeleteClicked",
                              Qt::DirectConnection);

    EXPECT_EQ(deleteSpy.count(), 0)
        << "deleteMarkRequested fired with no selection — panel must "
           "guard against this to avoid emitting bogus payloads.";
}

// Disabled marks (RedactionData::enabled=false) MUST be skipped by
// setMarks() — they exist in the in-memory list but are not yet
// active for redaction. The panel should reflect only the active
// (enabled) marks; otherwise the user sees rows that don't actually
// participate in apply-all and gets confused about which marks are
// pending.
TEST_F(RedactionPanelDeleteMarkTest, DisabledMarksAreSkippedFromTree)
{
    auto* treeWidget = tree();
    ASSERT_NE(treeWidget, nullptr);

    std::vector<RedactionData> marks = {
        makeMark(0, 0, 100, 100, 50, 20),  // enabled
        makeMark(0, 1, 200, 100, 50, 20),  // will be disabled
        makeMark(1, 0,  50, 250, 80, 30),  // enabled
    };
    marks[1].enabled = false;

    panel_->setMarks(marks);

    EXPECT_EQ(treeWidget->topLevelItemCount(), 2)
        << "Disabled mark was not filtered out of the tree. Expected "
           "2 visible rows (skipping the disabled one), got "
        << treeWidget->topLevelItemCount() << ".";
}

}  // namespace
