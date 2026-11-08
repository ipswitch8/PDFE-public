// -*- coding: utf-8 -*-
//
// GitHub #356 — Add Watermark prompts for an image even in Text mode.
//
// The "Image:" row (path field + Browse button) was added to the form
// unconditionally and only *disabled* in Text mode; the mode toggle flipped
// enabled-state, not visibility, and did not fire for the default index — so
// the dialog still presented an image picker in Text mode. The fix HIDES the
// image row in Text mode (and sets the correct INITIAL state for the default
// mode), showing it only in Image mode.
//
// This builds the REAL WatermarkDialog and queries widget visibility — it does
// not re-implement any logic. Visibility is checked with isVisibleTo(dialog),
// which reflects an explicit hide() without requiring the dialog to be shown
// on a display (headless-safe).

#include <gtest/gtest.h>

#include <QApplication>
#include <QComboBox>
#include <QLineEdit>
#include <QPushButton>

#include "ui/WatermarkDialog.h"
#include "core/MuPdfContext.h"

#include <memory>

using namespace pdfe::ui;

TEST(WatermarkDialogModeVisibility, ImageRowHiddenInTextModeShownInImageMode) {
    ASSERT_NE(qApp, nullptr)
        << "needs a QApplication (provided by the suite main)";

    auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
    // A null document is fine: setupUi() does not touch it (onApply guards it),
    // and this test only inspects the widget tree.
    WatermarkDialog dlg(ctx, nullptr, nullptr);

    auto* mode      = dlg.findChild<QComboBox*>("watermarkDialogModeCombo");
    auto* imagePath = dlg.findChild<QLineEdit*>("watermarkDialogImagePath");
    auto* browse    = dlg.findChild<QPushButton*>("watermarkDialogImageBrowse");
    auto* textEdit  = dlg.findChild<QLineEdit*>("watermarkDialogTextEdit");
    ASSERT_NE(mode, nullptr);
    ASSERT_NE(imagePath, nullptr);
    ASSERT_NE(browse, nullptr);
    ASSERT_NE(textEdit, nullptr);

    // Default mode is Text (index 0). The image row must start HIDDEN — the
    // #356 bug is that the default index never triggered the toggle, so the
    // image row was shown (merely disabled) in Text mode.
    ASSERT_EQ(mode->currentData().toString(), QStringLiteral("text"))
        << "default watermark mode is expected to be Text";
    EXPECT_FALSE(imagePath->isVisibleTo(&dlg))
        << "GitHub #356: the image-path field must be HIDDEN in Text mode, not "
           "merely disabled";
    EXPECT_FALSE(browse->isVisibleTo(&dlg))
        << "GitHub #356: the Browse button must be HIDDEN in Text mode";
    EXPECT_TRUE(textEdit->isVisibleTo(&dlg))
        << "the watermark text field must be visible in Text mode";

    // Switch to Image mode → the image row must become visible.
    const int imageIndex = mode->findData(QStringLiteral("image"));
    ASSERT_GE(imageIndex, 0);
    mode->setCurrentIndex(imageIndex);
    EXPECT_TRUE(imagePath->isVisibleTo(&dlg))
        << "the image-path field must be SHOWN in Image mode";
    EXPECT_TRUE(browse->isVisibleTo(&dlg))
        << "the Browse button must be SHOWN in Image mode";

    // Switch back to Text → hidden again (proves the toggle fires on change).
    mode->setCurrentIndex(0);
    EXPECT_FALSE(imagePath->isVisibleTo(&dlg))
        << "returning to Text mode must hide the image row again";
    EXPECT_FALSE(browse->isVisibleTo(&dlg))
        << "returning to Text mode must hide the Browse button again";
}
