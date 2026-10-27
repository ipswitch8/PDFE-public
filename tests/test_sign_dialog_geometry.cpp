// -*- coding: utf-8 -*-
//
// Pipeline a7c2e541 phase-17 (coverage expansion).
//
// AC2-coverage: addresses tests/docs/coverage_gaps.md entry #4.
//
// Verifies the QScrollArea wrapping of SignDialog added in phase-17
// makes the dialog fit on small / RDP displays.  Closes KNOWN_WEAK
// P12-INFRA-2 on the product side (test E2E xfails remain
// strict=False, but the underlying SignDialog geometry issue —
// minimum-height exceeded 600 px — is fixed).
//
// Specifically asserts:
//   1. minimumSizeHint().height() <= 600 px so the dialog fits on
//      tight RDP displays.
//   2. The QScrollArea is reachable via findChild<QScrollArea*>.
//   3. Every formerly-direct child widget is still reachable through
//      findChild (i.e. the reparent did not orphan any objectName).

#include <gtest/gtest.h>
#include "ui/SignDialog.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QLineEdit>
#include <QPushButton>
#include <QLabel>
#include <QScrollArea>
#include <QGroupBox>
#include <memory>

namespace {

class SignDialogGeometryTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!qApp) GTEST_SKIP() << "QApplication not available";
    }

    void TearDown() override {
        dialog.reset();
    }

    std::unique_ptr<pdfe::ui::SignDialog> dialog;
};

} // namespace

TEST_F(SignDialogGeometryTest, MinimumSizeHintFitsSmallDisplay)
{
    dialog = std::make_unique<pdfe::ui::SignDialog>(nullptr);
    // Force layout to settle.
    dialog->adjustSize();

    QSize hint = dialog->minimumSizeHint();
    // KNOWN_WEAK P12-INFRA-2 was triggered when the dialog's effective
    // minimum-height exceeded 600 px on tight RDP rigs.  After wrapping
    // in QScrollArea + setMinimumHeight(300), the dialog must report a
    // minimum height well under 600 px so the OS can place it.
    EXPECT_LE(hint.height(), 600)
        << "SignDialog minimumSizeHint().height() = " << hint.height()
        << " — must be <= 600 px to fit on small / RDP displays";
}

TEST_F(SignDialogGeometryTest, ScrollAreaIsPresent)
{
    dialog = std::make_unique<pdfe::ui::SignDialog>(nullptr);
    auto* scroll = dialog->findChild<QScrollArea*>("signDialogScrollArea");
    ASSERT_NE(scroll, nullptr)
        << "Expected QScrollArea with objectName 'signDialogScrollArea'";
    EXPECT_TRUE(scroll->widgetResizable());
}

TEST_F(SignDialogGeometryTest, AllChildWidgetsStillReachableAfterReparent)
{
    dialog = std::make_unique<pdfe::ui::SignDialog>(nullptr);

    // Each formerly-direct child must still be reachable through the
    // findChild walk after being reparented into the QScrollArea
    // content widget.  This is the same audit set the pre-existing
    // test_sign_dialog.cpp ObjectNames test runs, replicated here so
    // a regression in the wrapping change is caught locally.
    EXPECT_NE(nullptr, dialog->findChild<QLineEdit*>("pfxPathEdit"));
    EXPECT_NE(nullptr, dialog->findChild<QPushButton*>("browseButton"));
    EXPECT_NE(nullptr, dialog->findChild<QLineEdit*>("pfxPasswordEdit"));
    EXPECT_NE(nullptr, dialog->findChild<QLineEdit*>("reasonEdit"));
    EXPECT_NE(nullptr, dialog->findChild<QLineEdit*>("locationEdit"));
    EXPECT_NE(nullptr, dialog->findChild<QCheckBox*>("showLabelsCheck"));
    EXPECT_NE(nullptr, dialog->findChild<QCheckBox*>("showNameCheck"));
    EXPECT_NE(nullptr, dialog->findChild<QCheckBox*>("showDateCheck"));
    EXPECT_NE(nullptr, dialog->findChild<QCheckBox*>("showDnCheck"));
    EXPECT_NE(nullptr, dialog->findChild<QCheckBox*>("showLogoCheck"));
    EXPECT_NE(nullptr, dialog->findChild<QCheckBox*>("tsaCheckBox"));
    EXPECT_NE(nullptr, dialog->findChild<QLineEdit*>("tsaUrlEdit"));
    EXPECT_NE(nullptr, dialog->findChild<QCheckBox*>("invisibleCheckBox"));
    EXPECT_NE(nullptr, dialog->findChild<QComboBox*>("certificationLevelCombo"));
    EXPECT_NE(nullptr, dialog->findChild<QLabel*>("previewLabel"));
    EXPECT_NE(nullptr, dialog->findChild<QLabel*>("validationLabel"));
    EXPECT_NE(nullptr, dialog->findChild<QPushButton*>("okButton"));
    EXPECT_NE(nullptr, dialog->findChild<QPushButton*>("cancelButton"));

    // GroupBoxes too — UIA driver enumerates them when navigating
    // through the dialog by Tab.
    EXPECT_NE(nullptr, dialog->findChild<QGroupBox*>("certificateGroup"));
    EXPECT_NE(nullptr, dialog->findChild<QGroupBox*>("signatureInfoGroup"));
    EXPECT_NE(nullptr, dialog->findChild<QGroupBox*>("appearanceGroup"));
    EXPECT_NE(nullptr, dialog->findChild<QGroupBox*>("advancedGroup"));
}

TEST_F(SignDialogGeometryTest, ResizesBelowFullContentHeightWithoutClipping)
{
    // The dialog must be resizable to 400 px height without the
    // QScrollArea complaining (the scroll bar should appear).  We
    // can't easily verify the scroll bar visually in a headless test
    // but we can verify the resize() call does not change the
    // minimumSizeHint contract (no spinning re-layout).
    dialog = std::make_unique<pdfe::ui::SignDialog>(nullptr);
    dialog->resize(450, 400);
    QSize hintBefore = dialog->minimumSizeHint();
    dialog->resize(450, 300);
    QSize hintAfter = dialog->minimumSizeHint();
    EXPECT_EQ(hintBefore.width(), hintAfter.width());
    EXPECT_EQ(hintBefore.height(), hintAfter.height());
}

TEST_F(SignDialogGeometryTest, MinimumWidthPreserved)
{
    // The pre-existing setMinimumWidth(450) call must still take
    // effect through the new QScrollArea wrapper.
    dialog = std::make_unique<pdfe::ui::SignDialog>(nullptr);
    EXPECT_GE(dialog->minimumWidth(), 450);
}
