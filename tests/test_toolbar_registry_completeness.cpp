// GitHub #381 — regression tests for the two defects the v0.5.1 UI sweep
// surfaced in the File Attachment / Stamp annotation tools.
//
// Part A (runtime): buildToolbarActionRegistry() omitted stampToolAction and
// fileAttachmentToolAction. Both are created as local-scope QActions in
// createAnnotationToolBar() and added to annotationToolBar_, but were absent
// from the registry, so on any machine with a saved toolbar layout
// applyLayoutToToolbar() (= saved layout ∩ registry) DROPPED them from the bar
// and emitted "ToolbarLayoutManager: unknown action id" every launch. The
// guard below constructs a MainWindow, captures every live toolbar, and
// asserts every on-toolbar action id resolves in the registry — i.e. nothing
// on a toolbar can be silently dropped. It fails against the pre-fix tree on
// exactly those two ids and passes after the registry adds them. This is the
// class guard the pre-existing test_toolbar_customization.cpp could not be:
// that suite feeds ToolbarLayoutManager a FABRICATED registry that already
// includes the two ids, so it is structurally blind to the real omission.
//
// Part B (source-level): the attachment-create access-violation was a
// use-after-free of a raw QAction* captured by value in the stamp/attach
// triggered-lambdas, dereferenced via setChecked(false) after a modal
// QInputDialog returned (the window torn down mid-modal frees the action). The
// fix captures a QPointer<QAction> (auto-nulls on destruction) and guards every
// post-dialog setChecked. The faithful end-to-end exercise of the create flow
// is the pywinauto test tests/test_ui_annotations.py::test_file_attachment_create
// (desk-confirmed post-fix); this source guard is the cheap, deterministic lock
// that fails if anyone reverts the captures to a raw pointer or drops a guard.

#include <gtest/gtest.h>

#include "ui/MainWindow.h"
#include "ui/ToolbarLayoutManager.h"

#include <QAction>
#include <QString>
#include <QStringList>
#include <QToolBar>
#include <QtGlobal>

#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>

using namespace pdfe::ui;

namespace {

// Resolve src/ui/MainWindow.cpp from this test's __FILE__ (tests/ -> ../src/..).
std::filesystem::path resolveMainWindowCpp() {
    std::filesystem::path here(__FILE__);
    // __FILE__ is .../tests/test_toolbar_registry_completeness.cpp
    std::filesystem::path root = here.parent_path().parent_path();
    return root / "src" / "ui" / "MainWindow.cpp";
}

std::string slurp(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

} // namespace

// The toolbars that are subject to the saved-layout apply/drop path. This is
// the EXACT set MainWindow::onCustomizeToolbars() captures and saves
// (MainWindow.cpp ~16091) and that loadSavedToolbarLayout() -> applyToolbarLayoutsList()
// then rebuilds from (saved layout ∩ registry). An action on one of THESE
// toolbars that is absent from the registry is dropped on any machine with a
// saved layout — the #381 defect. FormToolBar is deliberately NOT here: it is
// a contextual, non-customizable toolbar (created hidden, never captured or
// saved), so its form-field actions are correctly absent from the registry
// (which also drives the Customize dialog's "Available actions" offering —
// placing form-field creation on the general toolbars would be wrong). Keep
// this list in lockstep with onCustomizeToolbars(): if a toolbar is added
// there, add it here, and its actions must then be registered.
static const char* kCustomizableToolbars[] = {"mainToolBar", "annotationToolBar"};

// Part A — every id placed on a CUSTOMIZABLE toolbar must resolve in the
// registry, otherwise it is dropped whenever a saved layout is applied
// (GitHub #381, the stamp/attach regression lived on annotationToolBar).
TEST(ToolbarRegistryCompleteness, EveryOnToolbarActionIdResolvesInTheRegistry) {
    MainWindow w;

    std::set<QString> registered;
    for (QAction* a : w.toolbarActionRegistryForTesting()) {
        if (a && !a->objectName().isEmpty()) {
            registered.insert(a->objectName());
        }
    }
    ASSERT_FALSE(registered.empty())
        << "registry is empty — MainWindow construction did not build the "
           "toolbars, test setup is wrong";

    int checkedIds = 0;
    for (const char* name : kCustomizableToolbars) {
        QToolBar* tb = w.findChild<QToolBar*>(QString::fromUtf8(name));
        ASSERT_NE(tb, nullptr)
            << "customizable toolbar '" << name << "' not found on MainWindow — "
               "either it was renamed (update kCustomizableToolbars and "
               "onCustomizeToolbars together) or setup is wrong";
        const ToolbarLayout layout =
            ToolbarLayoutManager::captureFromToolbar(tb);
        for (const QString& id : layout.actionIds) {
            if (id.isEmpty()) continue;  // separator or empty-name widget action
            ++checkedIds;
            EXPECT_TRUE(registered.count(id) > 0)
                << "customizable toolbar '" << name
                << "' carries action id '" << id.toStdString()
                << "' which is NOT in buildToolbarActionRegistry(); it would be "
                   "DROPPED from the toolbar on any machine with a saved layout "
                   "(GitHub #381). Register it in "
                   "MainWindow::buildToolbarActionRegistry().";
        }
    }
    EXPECT_GT(checkedIds, 0)
        << "no non-separator toolbar action ids were captured — the toolbars "
           "appear empty, so the guard proved nothing";
}

// Part A, pinned — the two specific ids the sweep reported must be present,
// so this test documents the exact regression it locks.
TEST(ToolbarRegistryCompleteness, StampAndAttachAreRegistered) {
    MainWindow w;
    std::set<QString> registered;
    for (QAction* a : w.toolbarActionRegistryForTesting()) {
        if (a) registered.insert(a->objectName());
    }
    EXPECT_TRUE(registered.count("stampToolAction") > 0)
        << "stampToolAction missing from the toolbar action registry (#381)";
    EXPECT_TRUE(registered.count("fileAttachmentToolAction") > 0)
        << "fileAttachmentToolAction missing from the toolbar action registry "
           "(#381)";
}

// Part A, mechanism — the exact function that emits the startup warning,
// applyLayoutToToolbar(), must emit NO "unknown action id" line when a saved
// layout naming the live annotation tools (incl. stamp/attach, as this
// machine's real layout did) is applied against the real registry. This is
// criterion-1's "zero unknown-action-id launch-log lines" proven
// deterministically without a GUI launch (a GUI app routes qWarning to a log
// file whose location is environment-dependent; this captures the message at
// source). Fails on the pre-fix tree (registry missing the two ids), passes
// after.
namespace { QStringList g_capturedMessages; }
TEST(ToolbarRegistryCompleteness, SavedLayoutApplyEmitsNoUnknownActionIdWarning) {
    MainWindow w;
    QToolBar* annot = w.findChild<QToolBar*>(QStringLiteral("annotationToolBar"));
    ASSERT_NE(annot, nullptr);

    // A saved layout exactly as captureFromToolbar() would record the live bar
    // — this includes stampToolAction and fileAttachmentToolAction.
    const ToolbarLayout saved = ToolbarLayoutManager::captureFromToolbar(annot);
    ASSERT_TRUE(saved.actionIds.contains("stampToolAction"))
        << "fixture precondition: the live annotationToolBar must carry "
           "stampToolAction for this guard to be meaningful";
    ASSERT_TRUE(saved.actionIds.contains("fileAttachmentToolAction"))
        << "fixture precondition: the live annotationToolBar must carry "
           "fileAttachmentToolAction";

    g_capturedMessages.clear();
    QtMessageHandler prev = qInstallMessageHandler(
        [](QtMsgType, const QMessageLogContext&, const QString& msg) {
            g_capturedMessages.push_back(msg);
        });
    // The real function that emits "ToolbarLayoutManager: unknown action id ..."
    ToolbarLayoutManager::applyLayoutToToolbar(
        annot, saved, w.toolbarActionRegistryForTesting());
    qInstallMessageHandler(prev);

    for (const QString& m : g_capturedMessages) {
        EXPECT_FALSE(m.contains("unknown action id"))
            << "applyLayoutToToolbar emitted an unknown-action-id warning (the "
               "#381 startup symptom): " << m.toStdString();
    }
}

// Part B — source guard: the stamp/attach triggered-lambdas must capture a
// QPointer<QAction>, never a raw QAction* by value, and every setChecked on
// them must be null-guarded. Locks the attachment-create UAF fix (#381 Part B).
TEST(ToolbarRegistryCompleteness, StampAndAttachLambdasCaptureQPointerNotRawAction) {
    const std::filesystem::path cpp = resolveMainWindowCpp();
    ASSERT_TRUE(std::filesystem::exists(cpp))
        << "could not resolve MainWindow.cpp from __FILE__ (" << __FILE__
        << ") -> " << cpp.string();
    const std::string src = slurp(cpp);
    ASSERT_FALSE(src.empty()) << "MainWindow.cpp read empty";

    // The QPointer captures must be present.
    EXPECT_NE(src.find("stampAct = QPointer<QAction>(stampAct)"), std::string::npos)
        << "stamp triggered-lambda must capture QPointer<QAction>, not a raw "
           "QAction* (GitHub #381 Part B UAF).";
    EXPECT_NE(src.find("attachAct = QPointer<QAction>(attachAct)"), std::string::npos)
        << "fileAttachment triggered-lambda must capture QPointer<QAction>, not "
           "a raw QAction* (GitHub #381 Part B UAF — this was the faulting "
           "slot).";

    // The dangerous raw-pointer captures must be gone.
    EXPECT_EQ(src.find("[this, stampAct]"), std::string::npos)
        << "raw [this, stampAct] capture reintroduced — reopens the #381 UAF.";
    EXPECT_EQ(src.find("[this, attachAct]"), std::string::npos)
        << "raw [this, attachAct] capture reintroduced — reopens the #381 UAF.";

    // Every setChecked(false) on these actions must be null-guarded.
    EXPECT_EQ(src.find("{ stampAct->setChecked(false)"), std::string::npos)
        << "unguarded stampAct->setChecked found — guard with if (stampAct).";
    EXPECT_EQ(src.find("{ attachAct->setChecked(false)"), std::string::npos)
        << "unguarded attachAct->setChecked found — guard with if (attachAct).";
    // And the guarded form must actually be present (not merely the raw form absent).
    EXPECT_NE(src.find("if (attachAct) attachAct->setChecked(false)"),
              std::string::npos)
        << "guarded attachAct->setChecked(false) not found.";
    EXPECT_NE(src.find("if (stampAct) stampAct->setChecked(false)"),
              std::string::npos)
        << "guarded stampAct->setChecked(false) not found.";
}
