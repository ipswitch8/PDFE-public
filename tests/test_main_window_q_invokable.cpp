// -*- coding: utf-8 -*-
//
// Pipeline b3f19d72 phase-1.
//
// Verifies the Q_INVOKABLE test-automation hook added to
// MainWindow in phase-1:
//
//   1. getCurrentAnnotationAltText() -> QString
//
// The hook exposes the SelectTool <-> AltText round-trip to
// pywinauto / WinAppDriver via the Qt meta-object system, so a
// test harness can verify alt-text application without driving
// the AltTextDialog read-back path manually.
//
// Pipeline f2a8c3e1 phase-1 fix: MainWindow is a large class; allocating
// it as a local (stack) variable inside a TEST() body corrupts the CRT
// stack guard (Run-Time Check Failure #2). Root cause: MainWindow's
// constructor allocates dozens of member widgets, QAction instances, and
// dock widgets that together exceed the test thread's default stack-frame
// guard region. Fix: use a TEST_F fixture that owns a heap-allocated
// unique_ptr<MainWindow>, matching the established pattern in
// test_main_window_icon.cpp and test_alignment_e2e.cpp.
// The `if (!qApp) GTEST_SKIP()` guards are removed: test_main.cpp
// constructs QApplication before RUN_ALL_TESTS(), so qApp is always
// non-null inside test bodies.

#include <gtest/gtest.h>

#include <QApplication>
#include <QGenericReturnArgument>
#include <QMetaMethod>
#include <QMetaObject>
#include <QString>
#include <QTimer>

#include "ui/MainWindow.h"

#include <filesystem>
#include <memory>

using namespace pdfe::ui;

namespace {

// Locate the Q_INVOKABLE method by name on the live meta-object.
// Returns -1 if the method is not exposed (which would mean the
// Q_INVOKABLE keyword was lost / moc didn't regenerate).
int findInvokableIndex(const QObject* obj, const char* name) {
    const QMetaObject* mo = obj->metaObject();
    for (int i = 0; i < mo->methodCount(); ++i) {
        QMetaMethod m = mo->method(i);
        if (m.methodType() == QMetaMethod::Method
            && QString(m.name()) == QString(name)) {
            return i;
        }
    }
    return -1;
}

} // namespace

// Shared fixture: constructs MainWindow on the heap so its large member set
// does not overflow the test thread's stack-frame guard region.
// QApplication is owned by test_main.cpp and lives for the entire process.
class MainWindowQInvokableTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Heap-allocate MainWindow — same pattern as test_main_window_icon.cpp
        // and test_alignment_e2e.cpp. Stack-allocating MainWindow triggers CRT
        // Run-Time Check Failure #2 because the class is too large for the
        // default stack guard region.
        mw_ = std::make_unique<MainWindow>();
    }
    void TearDown() override {
        mw_.reset();
        QApplication::processEvents();
    }
    std::unique_ptr<MainWindow> mw_;
};

// ─── getCurrentAnnotationAltText ─────────────────────────────────────────

TEST_F(MainWindowQInvokableTest, GetCurrentAnnotationAltText_RegisteredOnMetaObject)
{
    // Lock the Q_INVOKABLE exposure on the meta-object so pywinauto
    // and WinAppDriver harnesses can dispatch the method by name.
    // If a future refactor loses the Q_INVOKABLE keyword (or moc
    // skips MainWindow.h), this assertion catches it.
    int idx = findInvokableIndex(mw_.get(), "getCurrentAnnotationAltText");
    EXPECT_GE(idx, 0)
        << "getCurrentAnnotationAltText must be registered as a "
           "Q_INVOKABLE on MainWindow's meta-object";
}

TEST_F(MainWindowQInvokableTest, GetCurrentAnnotationAltText_EmptyWhenNoDocument)
{
    // No document loaded -> empty.
    QString alt = mw_->getCurrentAnnotationAltText();
    EXPECT_TRUE(alt.isEmpty())
        << "Expected empty alt text with no document loaded; got: "
        << alt.toStdString();
}

TEST_F(MainWindowQInvokableTest, GetCurrentAnnotationAltText_EmptyWhenNoSelection)
{
    // Pipeline b3f19d72 phase-1 AC5 — empty-string contract: when a
    // document IS loaded but no annotation has been selected via
    // SelectTool, the method returns the empty string (not the alt
    // text of annotation 0 by accident, not a thrown exception).

    std::filesystem::path fixtures = TEST_FIXTURES_DIR;
    auto pdfPath = fixtures / "test_with_freetext_annot.pdf";
    if (!std::filesystem::exists(pdfPath))
        GTEST_SKIP() << "test_with_freetext_annot.pdf fixture unavailable";

    mw_->openFile(QString::fromStdString(pdfPath.string()));

    // Loaded document, but no SelectTool selection has been made —
    // selectedAnnotationPageIndex_ / selectedAnnotationIndex_ are
    // still at their constructor / closeDocument defaults of -1, so
    // the method must short-circuit to empty.
    QString alt = mw_->getCurrentAnnotationAltText();
    EXPECT_TRUE(alt.isEmpty())
        << "Expected empty alt text with no annotation selected; "
           "got: " << alt.toStdString();
}

TEST_F(MainWindowQInvokableTest, GetCurrentAnnotationAltText_ReturnsExpectedText)
{
    // Pipeline b3f19d72 phase-1 AC5 — positive round-trip:
    //   1. Open the fixture that has a /FreeText annotation on page 0.
    //   2. Programmatically write a known /Alt value via the test
    //      seam (exercises the real AnnotationManager::setAltText
    //      MuPDF dict-write path).
    //   3. Set the SelectTool selection state via the test seam
    //      (exercises the same private-state path SelectTool::
    //      annotationSelected uses).
    //   4. Invoke getCurrentAnnotationAltText through the Qt meta-
    //      object dispatch chain (the exact path pywinauto uses).
    //   5. Assert the returned QString matches the value we wrote.

    std::filesystem::path fixtures = TEST_FIXTURES_DIR;
    auto pdfPath = fixtures / "test_with_freetext_annot.pdf";
    if (!std::filesystem::exists(pdfPath))
        GTEST_SKIP() << "test_with_freetext_annot.pdf fixture unavailable";

    mw_->openFile(QString::fromStdString(pdfPath.string()));

    const QString kExpectedAlt =
        QStringLiteral("Pipeline b3f19d72 phase-1 alt-text round-trip");

    // (2) Write the known alt text via the live AnnotationManager.
    ASSERT_TRUE(mw_->setAnnotationAltTextForTesting(0, 0, kExpectedAlt))
        << "Failed to write alt text via AnnotationManager — fixture "
           "is expected to have a /FreeText annotation at index 0 of "
           "page 0.";

    // (3) Mark annotation 0 of page 0 as the SelectTool current
    // selection. Same private-state path that SelectTool::
    // annotationSelected uses in the live UI.
    mw_->setSelectedAnnotationForTesting(0, 0);

    // (4) Dispatch through QMetaObject::invokeMethod, which is the
    // exact dispatch path pywinauto / WinAppDriver uses for
    // Q_INVOKABLE methods via the UIA/COM IDispatch bridge. If
    // Q_INVOKABLE registration is broken or the return-type binding
    // is wrong, invokeMethod returns false.
    QString out;
    bool ok = QMetaObject::invokeMethod(
        mw_.get(), "getCurrentAnnotationAltText",
        Qt::DirectConnection,
        Q_RETURN_ARG(QString, out));
    ASSERT_TRUE(ok)
        << "QMetaObject::invokeMethod failed to dispatch to "
           "getCurrentAnnotationAltText.";

    // (5) Assert the round-trip value matches what we wrote.
    EXPECT_EQ(out, kExpectedAlt);

    // And the direct call must agree with the meta-object dispatch.
    EXPECT_EQ(mw_->getCurrentAnnotationAltText(), kExpectedAlt);
}

// ─── activateToolByObjectName ─────────────────────────────────────────────

TEST_F(MainWindowQInvokableTest, ActivateToolByObjectName_RegisteredOnMetaObject)
{
    // Pipeline b3f19d72 phase-11: lock the Q_INVOKABLE registration so
    // future refactors that lose the keyword are caught immediately.
    int idx = findInvokableIndex(mw_.get(), "activateToolByObjectName");
    EXPECT_GE(idx, 0)
        << "activateToolByObjectName must be registered as a Q_INVOKABLE "
           "on MainWindow's meta-object (needed by the Python test seam).";
}

TEST_F(MainWindowQInvokableTest, ActivateToolByObjectName_ReturnsFalseForUnknownAction)
{
    bool result = mw_->activateToolByObjectName(
        QStringLiteral("nonExistentActionXYZ_b3f19d72_ph11"));
    EXPECT_FALSE(result)
        << "activateToolByObjectName must return false for unknown action names.";
}

TEST_F(MainWindowQInvokableTest, ActivateToolByObjectName_ReturnsTrueForKnownAction)
{
    // Verify that a well-known annotation-toolbar action (selectToolAction)
    // can be triggered by name. This exercises the findChildren<QAction*>
    // traversal and the trigger() dispatch without requiring a real document.
    // selectToolAction is always enabled after construction (no document
    // required — it's the default select mode). So it should return true.
    bool result = mw_->activateToolByObjectName(
        QStringLiteral("selectToolAction"));
    EXPECT_TRUE(result)
        << "activateToolByObjectName(\"selectToolAction\") must return true "
           "— selectToolAction is always enabled after MainWindow construction.";
}

// ─── AltTextAction selection-aware fix (GAP-27) ──────────────────────────
//
// Before GAP-27 the altTextAction_ lambda hardcoded annotIndex=0, so the
// AltTextDialog always operated on the first annotation regardless of which
// one the user had selected.  The fix reads selectedAnnotationIndex_ when it
// is valid for the current page and falls back to 0 only when no selection
// exists.  These tests lock that invariant.

class AltTextActionTest : public ::testing::Test {
protected:
    void SetUp() override {
        mw_ = std::make_unique<MainWindow>();

        std::filesystem::path fixtures = TEST_FIXTURES_DIR;
        fixturePath_ = fixtures / "phase3_annotations_all_types.pdf";
    }
    void TearDown() override {
        // Close any modal dialog that altTextAction_ may have opened
        // before the MainWindow destructor runs.  AltTextDialog is
        // WA_DeleteOnClose so closing it also frees the heap allocation.
        if (auto* modal = QApplication::activeModalWidget()) {
            modal->close();
            QApplication::processEvents();
        }
        mw_.reset();
        QApplication::processEvents();
    }
    std::unique_ptr<MainWindow> mw_;
    std::filesystem::path fixturePath_;
};

TEST_F(AltTextActionTest, UsesSelectedAnnotationIndexNotZero)
{
    // GAP-27: altTextAction_ must use selectedAnnotationIndex_ (when it is
    // valid for the current page) rather than always operating on annotation 0.
    //
    // Approach:
    //   1. Open a fixture with at least 2 annotations on page 0.
    //   2. Write distinct /Alt values to annotation 0 and annotation 1
    //      via the setAnnotationAltTextForTesting seam.
    //   3. Pre-select annotation 1 via setSelectedAnnotationForTesting.
    //   4. Trigger altTextAction_ (opens AltTextDialog with show(), non-blocking).
    //   5. Immediately close the dialog (before it can modify anything).
    //   6. Verify selectedAnnotationIndex_ was set to 1 (not 0) by checking
    //      that getCurrentAnnotationAltText() returns annotation 1's alt text.

    if (!std::filesystem::exists(fixturePath_))
        GTEST_SKIP() << "phase3_annotations_all_types.pdf fixture unavailable";

    mw_->openFile(QString::fromStdString(fixturePath_.string()));

    // Ensure at least 2 annotations exist before proceeding.
    // We write alt text speculatively; setAnnotationAltTextForTesting()
    // returns false if the index is out of range.
    const QString kAlt0 = QStringLiteral("GAP-27-annot-zero");
    const QString kAlt1 = QStringLiteral("GAP-27-annot-one");

    bool wrote0 = mw_->setAnnotationAltTextForTesting(0, 0, kAlt0);
    bool wrote1 = mw_->setAnnotationAltTextForTesting(0, 1, kAlt1);

    if (!wrote0 || !wrote1)
        GTEST_SKIP() << "fixture does not have at least 2 annotations on page 0";

    // Pre-select annotation 1 (the non-default one) on page 0.
    mw_->setSelectedAnnotationForTesting(0, 1);

    // Schedule immediate dialog close so the triggered show() doesn't
    // block test teardown.  The timer fires during processEvents() below.
    QTimer::singleShot(0, []() {
        if (auto* modal = QApplication::activeModalWidget()) {
            modal->close();
        }
    });

    // Trigger altTextAction_.  The lambda: resolves annotIndex from
    // selectedAnnotationIndex_ (should pick 1), sets selectedAnnotationIndex_
    // to that value, then calls dlg->show() (non-blocking).
    QAction* act = mw_->findChild<QAction*>(QStringLiteral("altTextAction"));
    ASSERT_NE(act, nullptr)
        << "altTextAction not found as a child QAction of MainWindow";
    act->trigger();

    // Give Qt's event loop a chance to open and auto-close the dialog.
    QApplication::processEvents();
    QApplication::processEvents();  // two rounds to ensure close propagates

    // After the trigger the selectedAnnotationIndex_ must be 1, not 0.
    // getCurrentAnnotationAltText() reads selectedAnnotationIndex_ directly
    // so it returns kAlt1 iff the fix is in place.
    QString got = mw_->getCurrentAnnotationAltText();
    EXPECT_EQ(got, kAlt1)
        << "altTextAction_ must use selectedAnnotationIndex_ (1) when a "
           "valid annotation is pre-selected, not hardcode 0. "
           "Got: \"" << got.toStdString() << "\", "
           "expected: \"" << kAlt1.toStdString() << "\"";
}

TEST_F(AltTextActionTest, FallsBackToZeroWhenNoSelection)
{
    // GAP-27 regression guard: when selectedAnnotationIndex_ == -1 (no
    // SelectTool selection has been made), altTextAction_ must still operate
    // on annotation 0 as the safe fallback.

    if (!std::filesystem::exists(fixturePath_))
        GTEST_SKIP() << "phase3_annotations_all_types.pdf fixture unavailable";

    mw_->openFile(QString::fromStdString(fixturePath_.string()));

    const QString kAlt0 = QStringLiteral("GAP-27-fallback-zero");
    bool wrote0 = mw_->setAnnotationAltTextForTesting(0, 0, kAlt0);
    if (!wrote0)
        GTEST_SKIP() << "fixture does not have an annotation at index 0 on page 0";

    // Explicitly clear the selection (default state after open, but be
    // explicit so the test doesn't depend on constructor order).
    mw_->setSelectedAnnotationForTesting(-1, -1);

    QTimer::singleShot(0, []() {
        if (auto* modal = QApplication::activeModalWidget()) {
            modal->close();
        }
    });

    QAction* act = mw_->findChild<QAction*>(QStringLiteral("altTextAction"));
    ASSERT_NE(act, nullptr)
        << "altTextAction not found as a child QAction of MainWindow";
    act->trigger();

    QApplication::processEvents();
    QApplication::processEvents();

    // selectedAnnotationIndex_ should now be 0 (the fallback).
    // getCurrentAnnotationAltText() returns kAlt0 iff fallback path works.
    QString got = mw_->getCurrentAnnotationAltText();
    EXPECT_EQ(got, kAlt0)
        << "altTextAction_ must fall back to annotation 0 when no annotation "
           "is selected. Got: \"" << got.toStdString() << "\", "
           "expected: \"" << kAlt0.toStdString() << "\"";
}

// ─── Cross-domain action seam (pipeline c4a9e271 phase-3) ────────────────────
//
// Phase-3 harness audit (tests/docs/harness_audit_phase3.md, criterion 2)
// concludes that NO new C++ pipe seam is needed to drive the domains that the
// Python helpers historically reached only via pywinauto menu_select
// (compliance, encryption, signatures, preferences, search-panel visibility,
// page insert/delete/rotate). activateToolByObjectName() — exposed to Python
// through the testToolActivatePipe QLineEdit — already triggers ANY QAction
// child of MainWindow by objectName, so a SINGLE seam covers every domain.
//
// These smokes LOCK that conclusion so a future rename/reparent can't silently
// break the seam for a domain: the representative menu action each phase-5..7
// batch will drive must (a) exist as a child QAction with the exact objectName
// the harness writes, and (b) route correctly through activateToolByObjectName,
// honouring QAction::isEnabled(). The existing ReturnsTrueForKnownAction smoke
// only covers a checkable TOOL action (selectToolAction); these add the
// non-tool MENU actions the audit's cross-domain claim depends on.

TEST_F(MainWindowQInvokableTest, MenuOnlyDomainActions_ExistWithExpectedObjectNames)
{
    // The objectNames the phase-3 audit certifies as the activation handle for
    // each previously-"menu_select-only" domain. Each must be findable as a
    // child QAction so testToolActivatePipe -> activateToolByObjectName can
    // resolve it.
    const char* kDomainActions[] = {
        "preferencesAction",          // preferences
        "complianceAction",           // compliance
        "saveWithEncryptionAction",   // encryption
        "signDocumentAction",         // signatures (sign)
        "verifySignaturesAction",     // signatures (verify)
        "toggleSearchPanelAction",    // view / search-panel visibility
        "insertPagesFromFileAction",  // page-ops (insert)
        "deletePageAction",           // page-ops (delete)
        "rotatePageCWAction",         // page-ops (rotate)
    };
    for (const char* name : kDomainActions) {
        QAction* act = mw_->findChild<QAction*>(QString::fromLatin1(name));
        EXPECT_NE(act, nullptr)
            << "Menu-only-domain QAction '" << name << "' must exist as a child "
               "of MainWindow with that exact objectName so the "
               "testToolActivatePipe seam can drive it (phase-3 audit).";
    }
}

TEST_F(MainWindowQInvokableTest, ActivateToolByObjectName_FiresAlwaysEnabledMenuAction)
{
    // preferencesAction is a NON-tool menu action that is always enabled after
    // construction (it needs no open document). activateToolByObjectName must
    // resolve and accept it — proving the seam reaches menu actions, not just
    // the checkable tool actions in annotToolGroup_.
    //
    // The trigger is deferred via QTimer::singleShot(0); we deliberately do NOT
    // call processEvents() here, so PreferencesDialog::exec() never runs and
    // this stays a non-blocking smoke.
    ASSERT_NE(mw_->findChild<QAction*>(QStringLiteral("preferencesAction")),
              nullptr);
    EXPECT_TRUE(mw_->activateToolByObjectName(QStringLiteral("preferencesAction")))
        << "activateToolByObjectName must accept the always-enabled "
           "preferencesAction menu action.";
}

TEST_F(MainWindowQInvokableTest, ActivateToolByObjectName_ResultMatchesEnabledState)
{
    // The seam must honour QAction::isEnabled(): for a found, non-checkable
    // action its result equals isEnabled(). saveWithEncryptionAction is a
    // document-gated menu action, so with no document loaded it is disabled and
    // the seam returns false — a test that fires it without a document gets an
    // honest false, never a no-op "true". (If a build leaves it enabled, the
    // invariant still holds: result == isEnabled().)
    QAction* act =
        mw_->findChild<QAction*>(QStringLiteral("saveWithEncryptionAction"));
    ASSERT_NE(act, nullptr);
    ASSERT_FALSE(act->isCheckable())
        << "saveWithEncryptionAction is expected to be non-checkable; the "
           "result==isEnabled() invariant only holds for non-checkable actions.";
    EXPECT_EQ(mw_->activateToolByObjectName(
                  QStringLiteral("saveWithEncryptionAction")),
              act->isEnabled())
        << "Seam result must equal the action's enabled state for a "
           "non-checkable menu action.";
}
