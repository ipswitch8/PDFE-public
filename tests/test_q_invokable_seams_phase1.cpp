// -*- coding: utf-8 -*-
//
// Pipeline b9e14d07 phase-1: Q_INVOKABLE seam verification via
// QMetaObject::invokeMethod for SEAM-79, SEAM-82, SEAM-84.
//
// These tests verify the Q_INVOKABLE decorator is present and the
// methods are callable through Qt's meta-object system, which is the
// contract pywinauto/WinAppDriver tests rely on.

#include <gtest/gtest.h>

#include "ui/BookmarkPanel.h"
#include "ui/PdfCanvas.h"
#include "ui/SearchPanel.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"

#include <QApplication>
#include <QMetaObject>

#include <filesystem>
#include <memory>

namespace {
std::filesystem::path fixturesDir()
{
    return std::filesystem::path(TEST_FIXTURES_DIR);
}
} // namespace

// =====================================================================
// SEAM-79: SearchPanel::performReplace via QMetaObject::invokeMethod
// =====================================================================

TEST(QInvokableSeams, SearchPanel_PerformReplace_IsInvokable)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";

    pdfe::ui::SearchPanel panel(nullptr);

    int result = -1;
    bool ok = QMetaObject::invokeMethod(
        &panel, "performReplace",
        Q_RETURN_ARG(int, result),
        Q_ARG(QString, QString()),
        Q_ARG(QString, QString("replacement")),
        Q_ARG(bool, false));

    EXPECT_TRUE(ok)
        << "performReplace must be callable via QMetaObject::invokeMethod "
           "(Q_INVOKABLE contract)";
    EXPECT_EQ(result, 0)
        << "Empty find string must return 0 (no-op)";
}

TEST(QInvokableSeams, SearchPanel_PerformReplace_EmitsSignalOnNonEmptyFind)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";

    pdfe::ui::SearchPanel panel(nullptr);

    int result = -1;
    bool ok = QMetaObject::invokeMethod(
        &panel, "performReplace",
        Q_RETURN_ARG(int, result),
        Q_ARG(QString, QString("hello")),
        Q_ARG(QString, QString("world")),
        Q_ARG(bool, false));

    EXPECT_TRUE(ok);
    EXPECT_EQ(result, 1)
        << "Non-empty find must return 1 (signal emitted)";
}

// =====================================================================
// SEAM-82: BookmarkPanel::insertBookmark via QMetaObject::invokeMethod
// =====================================================================

TEST(QInvokableSeams, BookmarkPanel_InsertBookmark_IsInvokable)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";

    pdfe::ui::BookmarkPanel panel(nullptr);

    bool result = true;
    bool ok = QMetaObject::invokeMethod(
        &panel, "insertBookmark",
        Q_RETURN_ARG(bool, result),
        Q_ARG(int, 0),
        Q_ARG(QString, QString("Test Bookmark")));

    EXPECT_TRUE(ok)
        << "insertBookmark must be callable via QMetaObject::invokeMethod "
           "(Q_INVOKABLE contract)";
    // Without a document loaded, insertBookmark returns false.
    EXPECT_FALSE(result)
        << "insertBookmark without document must return false";
}

TEST(QInvokableSeams, BookmarkPanel_InsertBookmark_EmptyTitleRejected)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";

    pdfe::ui::BookmarkPanel panel(nullptr);

    bool result = true;
    bool ok = QMetaObject::invokeMethod(
        &panel, "insertBookmark",
        Q_RETURN_ARG(bool, result),
        Q_ARG(int, 0),
        Q_ARG(QString, QString()));

    EXPECT_TRUE(ok);
    EXPECT_FALSE(result)
        << "Empty title must be rejected";
}

TEST(QInvokableSeams, BookmarkPanel_InsertBookmark_SucceedsWithContext)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";

    auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
    pdfe::ui::BookmarkPanel panel(nullptr);
    panel.setContext(ctx);

    bool result = false;
    bool ok = QMetaObject::invokeMethod(
        &panel, "insertBookmark",
        Q_RETURN_ARG(bool, result),
        Q_ARG(int, 0),
        Q_ARG(QString, QString("Phase-1 Test Bookmark")));

    EXPECT_TRUE(ok)
        << "insertBookmark must be callable via QMetaObject::invokeMethod";
    EXPECT_TRUE(result)
        << "insertBookmark with context and valid title must return true";
}

// =====================================================================
// SEAM-84: PdfCanvas::viewRotation via QMetaObject::invokeMethod
// =====================================================================

TEST(QInvokableSeams, PdfCanvas_ViewRotation_IsInvokable)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";

    pdfe::ui::PdfCanvas canvas(nullptr);

    int rotation = -1;
    bool ok = QMetaObject::invokeMethod(
        &canvas, "viewRotation",
        Q_RETURN_ARG(int, rotation));

    EXPECT_TRUE(ok)
        << "viewRotation must be callable via QMetaObject::invokeMethod "
           "(Q_INVOKABLE contract)";
    EXPECT_EQ(rotation, 0)
        << "Default view rotation must be 0";
}

TEST(QInvokableSeams, PdfCanvas_ViewRotation_ReflectsRotateViewCW)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";

    pdfe::ui::PdfCanvas canvas(nullptr);
    canvas.rotateViewCW();

    int rotation = -1;
    bool ok = QMetaObject::invokeMethod(
        &canvas, "viewRotation",
        Q_RETURN_ARG(int, rotation));

    EXPECT_TRUE(ok);
    EXPECT_EQ(rotation, 90)
        << "After one CW rotation, viewRotation must return 90";
}
