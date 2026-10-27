/**
 * @file test_signature_panel.cpp
 * @brief Unit tests for SignaturePanel.
 */

#include <gtest/gtest.h>
#include "ui/SignaturePanel.h"
#include "signatures/SignatureData.h"

#include <QApplication>
#include <QTreeWidget>
#include <QPushButton>
#include <QLabel>
#include <QTest>
#include <QSignalSpy>
#include <memory>

class SignaturePanelTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!qApp) {
            GTEST_SKIP() << "QApplication not available";
        }
    }

    void TearDown() override {
        panel.reset();
    }

    std::unique_ptr<pdfe::ui::SignaturePanel> panel;
};

// Test 1: Construct and destroy
TEST_F(SignaturePanelTest, ConstructDestroy) {
    panel = std::make_unique<pdfe::ui::SignaturePanel>(nullptr);
    ASSERT_NE(panel, nullptr);
    EXPECT_EQ("signaturePanel", panel->objectName().toStdString());
}

// Test 2: ObjectNames
TEST_F(SignaturePanelTest, ObjectNames) {
    panel = std::make_unique<pdfe::ui::SignaturePanel>(nullptr);

    EXPECT_NE(nullptr, panel->findChild<QTreeWidget*>("signatureTreeWidget"));
    EXPECT_NE(nullptr, panel->findChild<QPushButton*>("verifyAllButton"));
    EXPECT_NE(nullptr, panel->findChild<QPushButton*>("signSelectedButton"));
    EXPECT_NE(nullptr, panel->findChild<QPushButton*>("viewDetailsButton"));
    EXPECT_NE(nullptr, panel->findChild<QLabel*>("signatureSummaryLabel"));
}

// Test 3: Set signatures populates tree
TEST_F(SignaturePanelTest, SetSignatures) {
    panel = std::make_unique<pdfe::ui::SignaturePanel>(nullptr);

    std::vector<pdfe::signatures::SignatureData> sigs;
    pdfe::signatures::SignatureData sig1;
    sig1.fieldName = "Sig1";
    sig1.signer.commonName = "Alice";
    sig1.pageIndex = 0;
    sig1.widgetIndex = 0;
    sig1.isSigned = true;
    sigs.push_back(sig1);

    pdfe::signatures::SignatureData sig2;
    sig2.fieldName = "Sig2";
    sig2.pageIndex = 1;
    sig2.widgetIndex = 0;
    sig2.isSigned = false;
    sigs.push_back(sig2);

    panel->setSignatures(sigs);

    auto* tree = panel->findChild<QTreeWidget*>("signatureTreeWidget");
    ASSERT_NE(tree, nullptr);
    EXPECT_EQ(2, tree->topLevelItemCount());

    // Check first item
    auto* item0 = tree->topLevelItem(0);
    EXPECT_EQ("Sig1", item0->text(0).toStdString());
    EXPECT_EQ("Alice", item0->text(1).toStdString());
    EXPECT_EQ("1", item0->text(2).toStdString()); // 1-based page
    EXPECT_EQ("Signed", item0->text(3).toStdString());

    // Check second item
    auto* item1 = tree->topLevelItem(1);
    EXPECT_EQ("Sig2", item1->text(0).toStdString());
    EXPECT_EQ("Unsigned", item1->text(3).toStdString());

    // Check summary
    auto* label = panel->findChild<QLabel*>("signatureSummaryLabel");
    EXPECT_FALSE(label->text().isEmpty());
}

// Test 4: Clear removes items
TEST_F(SignaturePanelTest, Clear) {
    panel = std::make_unique<pdfe::ui::SignaturePanel>(nullptr);

    std::vector<pdfe::signatures::SignatureData> sigs;
    pdfe::signatures::SignatureData sig;
    sig.fieldName = "ClearMe";
    sig.pageIndex = 0;
    sig.widgetIndex = 0;
    sigs.push_back(sig);

    panel->setSignatures(sigs);
    panel->clear();

    auto* tree = panel->findChild<QTreeWidget*>("signatureTreeWidget");
    EXPECT_EQ(0, tree->topLevelItemCount());

    auto* label = panel->findChild<QLabel*>("signatureSummaryLabel");
    EXPECT_TRUE(label->text().isEmpty());
}

// Test 5: Verify all signal
TEST_F(SignaturePanelTest, VerifyAllSignal) {
    panel = std::make_unique<pdfe::ui::SignaturePanel>(nullptr);

    QSignalSpy spy(panel.get(), &pdfe::ui::SignaturePanel::verifyAllRequested);
    ASSERT_TRUE(spy.isValid());

    auto* btn = panel->findChild<QPushButton*>("verifyAllButton");
    ASSERT_NE(btn, nullptr);

    // Enable button first (it may be disabled)
    btn->setEnabled(true);
    QTest::mouseClick(btn, Qt::LeftButton);

    EXPECT_EQ(1, spy.count());
}

// Test 6: Signature selected signal
TEST_F(SignaturePanelTest, SignatureSelectedSignal) {
    panel = std::make_unique<pdfe::ui::SignaturePanel>(nullptr);

    QSignalSpy spy(panel.get(), &pdfe::ui::SignaturePanel::signatureSelected);
    ASSERT_TRUE(spy.isValid());

    std::vector<pdfe::signatures::SignatureData> sigs;
    pdfe::signatures::SignatureData sig;
    sig.fieldName = "ClickMe";
    sig.pageIndex = 2;
    sig.widgetIndex = 3;
    sigs.push_back(sig);

    panel->setSignatures(sigs);

    auto* tree = panel->findChild<QTreeWidget*>("signatureTreeWidget");
    ASSERT_NE(tree, nullptr);
    ASSERT_EQ(1, tree->topLevelItemCount());

    // Phase-16 fix (qt_test_gap_audit finding #25): drive a real
    // QTest::mouseClick on the tree-widget viewport so the panel's
    // slot fires through Qt's natural selection chain — catches a
    // regression that switched the connection to currentItemChanged.
    QTreeWidgetItem* item = tree->topLevelItem(0);
    tree->setCurrentItem(item);
    QRect itemRect = tree->visualItemRect(item);
    panel->show();
    (void)QTest::qWaitForWindowExposed(panel.get());
    QTest::mouseClick(tree->viewport(), Qt::LeftButton,
                      Qt::NoModifier, itemRect.center());
    QApplication::processEvents();

    ASSERT_EQ(1, spy.count());
    EXPECT_EQ(2, spy.at(0).at(0).toInt()); // pageIndex
    EXPECT_EQ(3, spy.at(0).at(1).toInt()); // widgetIndex
}

// Test 7: Empty list disables buttons
TEST_F(SignaturePanelTest, EmptyList_DisablesButtons) {
    panel = std::make_unique<pdfe::ui::SignaturePanel>(nullptr);

    panel->clear();

    auto* signBtn = panel->findChild<QPushButton*>("signSelectedButton");
    auto* detailsBtn = panel->findChild<QPushButton*>("viewDetailsButton");

    EXPECT_FALSE(signBtn->isEnabled());
    EXPECT_FALSE(detailsBtn->isEnabled());
}

// Test 8: Tree has 4 columns
TEST_F(SignaturePanelTest, TreeHas4Columns) {
    panel = std::make_unique<pdfe::ui::SignaturePanel>(nullptr);

    auto* tree = panel->findChild<QTreeWidget*>("signatureTreeWidget");
    ASSERT_NE(tree, nullptr);
    EXPECT_EQ(4, tree->columnCount());
}
