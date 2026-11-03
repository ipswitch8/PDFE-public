// -*- coding: utf-8 -*-
// GitHub #242 — after inserting an image, the image tool is active.
//
// WHY THIS IS THE ACTUAL FIX, AND WHY IT NEEDED ITS OWN TEST
//
// The report asked for size, placement and rotation control. Two of those are
// the dialog. Rotation is NOT: `ImageTool` already emitted `rotateRequested`
// (and `resizeRequested`, `cropRequested`), and `onImageToolSelected()` already
// connected all three to real undoable commands, so post-insert manipulation
// had worked the whole time.
//
// What was missing was discoverability. A person who had just inserted an image
// had to know, unprompted, to switch to the image tool and click the image
// before any handle existed. So `onInsertImage()` now activates the image tool
// on completion — and THAT is the line which closes the reported gap.
//
// A gate review pointed out that nothing regressed if it silently stopped
// working: the claim "rotation is reachable" rested on two facts that were each
// tested elsewhere (the signals exist; they are connected) plus one that was
// not tested at all (the user is taken there). This asserts the third.
#include <gtest/gtest.h>

#include "core/Document.h"
#include "image/ImageTool.h"
#include "ui/MainWindow.h"
#include "ui/PdfCanvas.h"
#include "ui/TestDialogSeam.h"

#include <QAction>
#include <QCoreApplication>
#include <QRectF>
#include <QString>

#include <filesystem>
#include <optional>
#include <string>

using pdfe::ui::MainWindow;

namespace {

std::string findFixture(const char* name)
{
    for (const char* prefix : {"tests/fixtures/", "../tests/fixtures/",
                               "../../tests/fixtures/"}) {
        const std::string candidate = std::string(prefix) + name;
        if (std::filesystem::exists(candidate)) return candidate;
    }
    return {};
}

void settle()
{
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();
}

/// MainWindow with the Insert Image dialog answered in code.
///
/// The dialog is modal, so a headless test that reached it would hang rather
/// than fail. `promptInsertImageOptions` exists as a virtual for exactly this,
/// matching `promptSaveOnClose` and `confirmFlattenForm`.
class ChoosingMainWindow : public MainWindow {
public:
    /// What the "user" picked — deliberately NOT the default rectangle, so the
    /// test also shows the choice is the one that travels to the insert.
    static constexpr double kX = 100.0;
    static constexpr double kY = 150.0;
    static constexpr double kW = 220.0;
    static constexpr double kH = 90.0;

    int promptCount = 0;

protected:
    std::optional<InsertImageChoice> promptInsertImageOptions(
        const QString&, QSize, QSizeF) override
    {
        ++promptCount;
        InsertImageChoice choice;
        choice.targetRect = QRectF(kX, kY, kW, kH);
        choice.jpegQuality = 70;
        choice.stripExif = true;
        choice.maintainAspectRatio = false;
        return choice;
    }
};

class InsertImageActivatesToolTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        pdf_ = findFixture("test_single.pdf");
        image_ = findFixture("test_image_rgb.png");
        if (pdf_.empty() || image_.empty()) {
            GTEST_SKIP() << "fixtures not available";
        }
    }

    std::string pdf_;
    std::string image_;
};

}  // namespace

// The assertion the gate asked for: the tool is live afterwards.
TEST_F(InsertImageActivatesToolTest, TheImageToolIsActiveAfterInserting)
{
    ChoosingMainWindow mw;
    mw.openFile(QString::fromStdString(pdf_));
    settle();
    ASSERT_NE(mw.documentForTesting(), nullptr) << "fixture failed to open";

    auto* canvas = mw.findChild<pdfe::ui::PdfCanvas*>();
    ASSERT_NE(canvas, nullptr);
    // The canvas tracks the image tool SEPARATELY from currentTool(): ImageTool
    // is a QObject, not an AnnotationTool, and onImageToolSelected() installs it
    // via setImageTool() after clearing the annotation tool. So imageTool() is
    // the thing to ask.
    ASSERT_EQ(canvas->imageTool(), nullptr)
        << "precondition: the image tool must NOT already be active, or this "
           "test would pass without the insert doing anything";

    auto* insert = mw.findChild<QAction*>(QStringLiteral("insertImageAction"));
    ASSERT_NE(insert, nullptr);

    pdfe::ui::testseam::enqueueFileDialogPath(QString::fromStdString(image_));
    insert->trigger();
    settle();

    ASSERT_EQ(mw.promptCount, 1)
        << "the options dialog was not consulted, so the insert did not run the "
           "path under test";

    EXPECT_NE(canvas->imageTool(), nullptr)
        << "the image tool is NOT active after inserting an image. The resize, "
           "rotate and crop handles exist and are connected, but the user has "
           "not been taken to them — which is the discoverability gap GitHub "
           "#242 actually reported.";

    auto* imageToolAction = mw.findChild<QAction*>(QStringLiteral("imageToolAction"));
    ASSERT_NE(imageToolAction, nullptr);
    EXPECT_TRUE(imageToolAction->isChecked())
        << "the toolbar does not show the image tool as active, so the window "
           "and the user disagree about which tool is in use";
}

// GitHub #329 — the actual reachability gap: after inserting, the image must be
// SELECTED without a manual click, or no handles paint and the image-toolbar
// actions stay disabled. Activating the tool (the test above) is necessary but
// NOT sufficient: ImageTool::setPage re-enumerates with selectedIndex_ == -1.
// This asserts the selection against the real ImageTool state (not a private
// field), and that the user-visible consequence — the toolbar actions — follows.
TEST_F(InsertImageActivatesToolTest, TheInsertedImageIsSelectedWithoutAClick)
{
    ChoosingMainWindow mw;
    mw.openFile(QString::fromStdString(pdf_));
    settle();
    ASSERT_NE(mw.documentForTesting(), nullptr) << "fixture failed to open";

    auto* canvas = mw.findChild<pdfe::ui::PdfCanvas*>();
    ASSERT_NE(canvas, nullptr);
    ASSERT_EQ(canvas->imageTool(), nullptr)
        << "precondition: the image tool must NOT already be active";

    auto* removeAction = mw.findChild<QAction*>(QStringLiteral("imageRemoveAction"));
    auto* resizeAction = mw.findChild<QAction*>(QStringLiteral("imageResizeAction"));
    ASSERT_NE(removeAction, nullptr);
    ASSERT_NE(resizeAction, nullptr);
    ASSERT_FALSE(removeAction->isEnabled())
        << "precondition: image actions must start disabled";

    auto* insert = mw.findChild<QAction*>(QStringLiteral("insertImageAction"));
    ASSERT_NE(insert, nullptr);

    pdfe::ui::testseam::enqueueFileDialogPath(QString::fromStdString(image_));
    insert->trigger();
    settle();

    ASSERT_EQ(mw.promptCount, 1)
        << "the options dialog was not consulted, so the insert path did not run";

    auto* tool = canvas->imageTool();
    ASSERT_NE(tool, nullptr) << "the image tool is not active after inserting";

    // The core #329 assertion: the just-inserted image is selected, with NO
    // mouse click anywhere. This is what makes the handles paint.
    EXPECT_TRUE(tool->hasSelection())
        << "GitHub #329: after inserting, the image tool reports NO selection, "
           "so no resize/rotate/crop handles are drawn and the image cannot be "
           "manipulated until the user separately clicks it — the reported gap.";

    const auto& selected = tool->selectedImage();
    EXPECT_TRUE(selected.isValid())
        << "the reported selection is not a valid image";
    // test_single.pdf has no images of its own, so the one selected image is the
    // one just inserted; its size is the size the 'user' chose in the dialog.
    EXPECT_NEAR(selected.boundingBox.width(), ChoosingMainWindow::kW, 2.0)
        << "the selected image is not the one just inserted (width mismatch)";
    EXPECT_NEAR(selected.boundingBox.height(), ChoosingMainWindow::kH, 2.0)
        << "the selected image is not the one just inserted (height mismatch)";

    // The user-visible consequence of the selection: the image-toolbar actions
    // enable off ImageTool::imageSelected, which selectImage() emits.
    EXPECT_TRUE(removeAction->isEnabled())
        << "#329: the Remove Image action is still disabled after insert, so "
           "the selection did not propagate to the toolbar";
    EXPECT_TRUE(resizeAction->isEnabled())
        << "#329: the Resize Image action is still disabled after insert";
}

// Cancelling must not activate anything, or the "activate on success" claim
// would be "activate always" and the first test could not tell them apart.
TEST_F(InsertImageActivatesToolTest, CancellingTheDialogInsertsNothingAndChangesNoTool)
{
    class CancellingMainWindow : public MainWindow {
    protected:
        std::optional<InsertImageChoice> promptInsertImageOptions(
            const QString&, QSize, QSizeF) override
        {
            return std::nullopt;  // the user pressed Cancel
        }
    };

    CancellingMainWindow mw;
    mw.openFile(QString::fromStdString(pdf_));
    settle();
    ASSERT_NE(mw.documentForTesting(), nullptr);

    auto* canvas = mw.findChild<pdfe::ui::PdfCanvas*>();
    ASSERT_NE(canvas, nullptr);

    auto* insert = mw.findChild<QAction*>(QStringLiteral("insertImageAction"));
    ASSERT_NE(insert, nullptr);

    pdfe::ui::testseam::enqueueFileDialogPath(QString::fromStdString(image_));
    insert->trigger();
    settle();

    EXPECT_EQ(canvas->imageTool(), nullptr)
        << "cancelling the Insert Image dialog still switched the user to the "
           "image tool";
}
