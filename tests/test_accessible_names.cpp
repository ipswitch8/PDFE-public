// Copyright (c) 2025 - PDFE Project
// Unit tests for accessible names in MainWindow widgets
// Ensures WCAG compliance and screen reader compatibility

#include <gtest/gtest.h>
#include "ui/MainWindow.h"
#include "tre/TreEditDialog.h"
#include "tre/RegionReconstructor.h"
#include <QDockWidget>
#include <QWidget>
#include <QDialog>
#include <QAbstractButton>
#include <QComboBox>
#include <QLineEdit>
#include <QAbstractSpinBox>
#include <QTextEdit>
#include <QPlainTextEdit>
#include <QAbstractItemView>
#include <QSlider>
#include <QList>
#include <QString>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace pdfe::ui;

// Test fixture for accessible names testing
class AccessibleNamesTest : public ::testing::Test {
protected:
    void SetUp() override {
        mainWindow = new MainWindow();
    }

    void TearDown() override {
        delete mainWindow;
        mainWindow = nullptr;
    }

    MainWindow* mainWindow = nullptr;
};

// Test 1: PDF Canvas should have an accessible name
TEST_F(AccessibleNamesTest, Canvas_HasAccessibleName) {
    auto* canvas = mainWindow->findChild<QWidget*>("pdfCanvas");
    ASSERT_NE(canvas, nullptr)
        << "PdfCanvas widget with objectName 'pdfCanvas' should exist";

    EXPECT_FALSE(canvas->accessibleName().isEmpty())
        << "PDF Canvas should have an accessible name for screen readers";
}

// Test 2: PDF Canvas should have an accessible description
TEST_F(AccessibleNamesTest, Canvas_HasAccessibleDescription) {
    auto* canvas = mainWindow->findChild<QWidget*>("pdfCanvas");
    ASSERT_NE(canvas, nullptr)
        << "PdfCanvas widget with objectName 'pdfCanvas' should exist";

    EXPECT_FALSE(canvas->accessibleDescription().isEmpty())
        << "PDF Canvas should have an accessible description for screen readers";
}

// Test 3: Page panel dock should have an accessible name
TEST_F(AccessibleNamesTest, PagePanelDock_HasAccessibleName) {
    auto* dock = mainWindow->findChild<QDockWidget*>("pagePanelDock");
    ASSERT_NE(dock, nullptr)
        << "Page panel dock widget should exist";

    EXPECT_FALSE(dock->accessibleName().isEmpty())
        << "Page panel dock should have an accessible name for screen readers";
}

// Test 4: Bookmark panel dock should have an accessible name
TEST_F(AccessibleNamesTest, BookmarkPanelDock_HasAccessibleName) {
    auto* dock = mainWindow->findChild<QDockWidget*>("bookmarkPanelDock");
    ASSERT_NE(dock, nullptr)
        << "Bookmark panel dock widget should exist";

    EXPECT_FALSE(dock->accessibleName().isEmpty())
        << "Bookmark panel dock should have an accessible name for screen readers";
}

// Test 5: Properties panel dock should have an accessible name
TEST_F(AccessibleNamesTest, PropertiesPanelDock_HasAccessibleName) {
    auto* dock = mainWindow->findChild<QDockWidget*>("propertiesPanelDock");
    ASSERT_NE(dock, nullptr)
        << "Properties panel dock widget should exist";

    EXPECT_FALSE(dock->accessibleName().isEmpty())
        << "Properties panel dock should have an accessible name for screen readers";
}

// Test 6: Search panel dock should have an accessible name
TEST_F(AccessibleNamesTest, SearchPanelDock_HasAccessibleName) {
    auto* dock = mainWindow->findChild<QDockWidget*>("searchPanelDock");
    ASSERT_NE(dock, nullptr)
        << "Search panel dock widget should exist";

    EXPECT_FALSE(dock->accessibleName().isEmpty())
        << "Search panel dock should have an accessible name for screen readers";
}

// Test 7: Comment panel dock should have an accessible name
TEST_F(AccessibleNamesTest, CommentPanelDock_HasAccessibleName) {
    auto* dock = mainWindow->findChild<QDockWidget*>("commentPanelDock");
    ASSERT_NE(dock, nullptr)
        << "Comment panel dock widget should exist";

    EXPECT_FALSE(dock->accessibleName().isEmpty())
        << "Comment panel dock should have an accessible name for screen readers";
}

// Test 8: Annotation properties dock should have an accessible name
TEST_F(AccessibleNamesTest, AnnotationPropertiesDock_HasAccessibleName) {
    auto* dock = mainWindow->findChild<QDockWidget*>("annotationPropertiesDock");
    ASSERT_NE(dock, nullptr)
        << "Annotation properties dock widget should exist";

    EXPECT_FALSE(dock->accessibleName().isEmpty())
        << "Annotation properties dock should have an accessible name for screen readers";
}

// Test 9: Redaction panel dock should have an accessible name
TEST_F(AccessibleNamesTest, RedactionPanelDock_HasAccessibleName) {
    auto* dock = mainWindow->findChild<QDockWidget*>("redactionPanelDock");
    ASSERT_NE(dock, nullptr)
        << "Redaction panel dock widget should exist";

    EXPECT_FALSE(dock->accessibleName().isEmpty())
        << "Redaction panel dock should have an accessible name for screen readers";
}

// Test 10: Signature panel dock should have an accessible name
TEST_F(AccessibleNamesTest, SignaturePanelDock_HasAccessibleName) {
    auto* dock = mainWindow->findChild<QDockWidget*>("signaturePanelDock");
    ASSERT_NE(dock, nullptr)
        << "Signature panel dock widget should exist";

    EXPECT_FALSE(dock->accessibleName().isEmpty())
        << "Signature panel dock should have an accessible name for screen readers";
}

// ---------------------------------------------------------------------------
// R2-19: OBJECTNAME_AUDIT scope extension to src/tre/, src/image/, src/layers/.
//
// src/tre/ contributes one interactive widget class, TreEditDialog. src/image/
// and src/layers/ contribute zero standalone interactive QWidgets (their tools
// are QObject canvas tools / backend classes; their user-facing panels live in
// src/ui/ and are already audited). The tests below give a live-widget
// CLAUDE.md-compliance check for the TreEditDialog surface plus a source-scan
// that guards all three scopes so any newly-added, un-named interactive widget
// re-reds the suite.
// ---------------------------------------------------------------------------

namespace {

// A widget is considered interactive if it accepts focus/click input.
bool isInteractiveWidget(QWidget* w) {
    return qobject_cast<QAbstractButton*>(w)
        || qobject_cast<QComboBox*>(w)
        || qobject_cast<QLineEdit*>(w)
        || qobject_cast<QAbstractSpinBox*>(w)
        || qobject_cast<QTextEdit*>(w)
        || qobject_cast<QPlainTextEdit*>(w)
        || qobject_cast<QAbstractItemView*>(w)
        || qobject_cast<QSlider*>(w);
}

// An interactive widget is acceptably identified for UIA/screen-readers if it
// carries an accessibleName, or (for a push/radio/check button) a visible text
// label that supplies its accessible identity (the audit "OK-NAME" status).
bool hasAccessibleIdentity(QWidget* w) {
    if (!w->accessibleName().isEmpty())
        return true;
    if (auto* b = qobject_cast<QAbstractButton*>(w))
        return !b->text().isEmpty();
    return false;
}

// Repo src/ directory, derived from the compile-time fixtures dir
// (<repo>/tests/fixtures -> <repo> -> <repo>/src).
std::filesystem::path srcDir() {
    return std::filesystem::path(TEST_FIXTURES_DIR)
        .parent_path()   // <repo>/tests
        .parent_path()   // <repo>
        / "src";
}

std::filesystem::path auditDocPath() {
    return std::filesystem::path(TEST_FIXTURES_DIR) / "ui_audit" / "OBJECTNAME_AUDIT.md";
}

std::string readFile(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Tokens that construct an interactive widget (construction happens in .cpp).
const std::vector<std::string>& interactiveConstructionTokens() {
    static const std::vector<std::string> tokens = {
        "new QPushButton", "new QToolButton", "new QRadioButton", "new QCheckBox",
        "new QComboBox", "new QFontComboBox", "new QLineEdit", "new QSpinBox",
        "new QDoubleSpinBox", "new QTextEdit", "new QPlainTextEdit", "new QSlider",
        "new QListWidget", "new QTreeWidget", "new QTableWidget", "new QListView",
        "new QTreeView", "new QTableView", "new QDialogButtonBox",
    };
    return tokens;
}

bool fileConstructsInteractiveWidget(const std::string& content) {
    for (const auto& tok : interactiveConstructionTokens()) {
        if (content.find(tok) != std::string::npos)
            return true;
    }
    return false;
}

} // namespace

// TreEditDialog is the sole interactive-widget class under src/tre/. Every one
// of its interactive children must expose an accessible identity. This is the
// RED->GREEN driver for the src/tre/ scope: before setAccessibleName was added
// the text editor and approach radios failed this walk.
TEST(TreAccessibleNamesTest, TreEditDialog_AllInteractiveWidgetsHaveAccessibleNames) {
    pdfe::tre::ReconstructionResult result;  // default-constructed is sufficient
    pdfe::tre::TreEditDialog dlg(result);

    // The dialog itself should be named for the UIA tree.
    EXPECT_FALSE(dlg.accessibleName().isEmpty())
        << "TreEditDialog should have an accessible name for screen readers";

    const QList<QWidget*> children = dlg.findChildren<QWidget*>();
    int interactiveCount = 0;
    for (QWidget* w : children) {
        if (!isInteractiveWidget(w))
            continue;
        ++interactiveCount;
        EXPECT_TRUE(hasAccessibleIdentity(w))
            << "Interactive widget in TreEditDialog lacks an accessible identity: objectName='"
            << w->objectName().toStdString() << "' class='"
            << w->metaObject()->className() << "'";
    }
    // Guard: the walk must actually have seen the dialog's interactive widgets,
    // otherwise a construction regression could make this pass vacuously.
    EXPECT_GE(interactiveCount, 3)
        << "Expected TreEditDialog to expose its text editor + approach radios";
}

// Named-widget spot checks (mirror the AccessibleNamesTest style) so a failure
// names the exact offending control.
TEST(TreAccessibleNamesTest, TreEditDialog_NamedControlsHaveAccessibleNames) {
    pdfe::tre::ReconstructionResult result;
    pdfe::tre::TreEditDialog dlg(result);

    for (const char* objName : {"treTextEdit", "approachBRadio", "approachCRadio", "buttonBox"}) {
        auto* w = dlg.findChild<QWidget*>(objName);
        ASSERT_NE(w, nullptr) << "Expected widget with objectName '" << objName << "'";
        EXPECT_FALSE(w->accessibleName().isEmpty())
            << "Widget '" << objName << "' should have an accessible name for screen readers";
    }
}

// Source-scan compliance check across the three newly-scoped subsystems.
// Any .cpp/.h file that constructs an interactive widget must also call
// setAccessibleName somewhere in the same translation unit. src/image/ and
// src/layers/ construct none today, so they pass trivially, but the coverage
// guards below ensure the scan really visited those directories.
TEST(ObjectNameAuditScopeTest, TreImageLayersInteractiveWidgetsAreNamed) {
    namespace fs = std::filesystem;

    struct ScopeStats { int filesScanned = 0; int filesWithWidgets = 0; };
    ScopeStats tre, image, layers;

    auto scanDir = [&](const fs::path& dir, ScopeStats& stats) {
        ASSERT_TRUE(fs::exists(dir)) << "Scope directory missing: " << dir.string();
        for (const auto& entry : fs::recursive_directory_iterator(dir)) {
            if (!entry.is_regular_file())
                continue;
            const auto ext = entry.path().extension().string();
            if (ext != ".cpp" && ext != ".h")
                continue;
            ++stats.filesScanned;
            const std::string content = readFile(entry.path());
            if (fileConstructsInteractiveWidget(content)) {
                ++stats.filesWithWidgets;
                EXPECT_NE(content.find("setAccessibleName"), std::string::npos)
                    << entry.path().filename().string()
                    << " constructs an interactive widget but never calls setAccessibleName "
                       "(R2-19: every interactive widget must be named)";
            }
        }
    };

    const fs::path src = srcDir();
    scanDir(src / "tre", tre);
    scanDir(src / "image", image);
    scanDir(src / "layers", layers);

    // Coverage guards: the scan must have actually visited each scope.
    EXPECT_GT(tre.filesScanned, 0)    << "src/tre/ was not scanned";
    EXPECT_GT(image.filesScanned, 0)  << "src/image/ was not scanned";
    EXPECT_GT(layers.filesScanned, 0) << "src/layers/ was not scanned";

    // src/tre/ must contain at least the TreEditDialog interactive surface, so
    // the setAccessibleName assertion above is not vacuous for this scope.
    EXPECT_GT(tre.filesWithWidgets, 0)
        << "Expected src/tre/ to contain an interactive-widget file (TreEditDialog)";
}

// The audit catalogue's scope must actually name the three extended subsystems,
// tying the "extend the OBJECTNAME_AUDIT scope" requirement to a machine check.
TEST(ObjectNameAuditScopeTest, AuditDocDeclaresExtendedScope) {
    const std::string doc = readFile(auditDocPath());
    ASSERT_FALSE(doc.empty()) << "OBJECTNAME_AUDIT.md could not be read";
    EXPECT_NE(doc.find("src/tre/"), std::string::npos)
        << "OBJECTNAME_AUDIT.md scope should include src/tre/";
    EXPECT_NE(doc.find("src/image/"), std::string::npos)
        << "OBJECTNAME_AUDIT.md scope should include src/image/";
    EXPECT_NE(doc.find("src/layers/"), std::string::npos)
        << "OBJECTNAME_AUDIT.md scope should include src/layers/";
}
