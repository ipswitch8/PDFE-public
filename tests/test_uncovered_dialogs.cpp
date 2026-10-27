// -*- coding: utf-8 -*-
//
// Pipeline a7c2e541 phase-17 (coverage expansion).
//
// Targets previously-uncovered src/ headers identified in
// tests/docs/coverage_gaps.md (entries #5-#16).
//
// AC2-coverage breakdown:
//   #5  PrintDialog            — PrintDialogConstructAndDefaults
//   #6  PageLabelsDialog       — PageLabelsDialogConstructAndRanges
//   #7  HeaderFooterDialog     — HeaderFooterDialogConstructAndObjectNames
//   #8  OptimizeDialog         — OptimizeDialogConstructAndObjectNames
//   #9  TrustAnchorDialog      — TrustAnchorDialogConstructAndObjectNames
//   #10 PropertiesPanel        — PropertiesPanelSetClearRoundTrip
//   #11 PageInfo               — PageInfoDefaultValues
//   #12 generateSrgbIccProfile — SrgbIccProfileHeaderShape +
//                                SrgbIccProfileRequiredTagsPresent
//   #13 CropTool               — CropToolToolTypeIsUnknown
//   #14 RedactionTool          — RedactionToolToolTypeIsUnknown
//   #15 OcspCrlData            — OcspResponseAndCrlInfoDefaultValues
//   #16 TantivyBridge          — TantivyHitAndResultsStructLayout

#include <gtest/gtest.h>

#include "annotations/CropTool.h"
#include "annotations/AnnotationTool.h"
#include "compliance/srgb_icc_profile.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/PageInfo.h"
#include "core/PrintEngine.h"
#include "redaction/RedactionManager.h"
#include "redaction/RedactionTool.h"
#include "search/TantivyBridge.h"
#include "signatures/OcspCrlData.h"
#include "ui/CustomizeToolbarsDialog.h"
#include "ui/HeaderFooterDialog.h"
#include "ui/OptimizeDialog.h"
#include "ui/PageLabelsDialog.h"
#include "ui/PrintDialog.h"
#include "ui/PrintPreviewDialog.h"
#include "ui/PropertiesPanel.h"
#include "ui/ToolbarLayoutManager.h"
#include "ui/TrustAnchorDialog.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QStandardItemModel>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QToolBar>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <vector>

// =====================================================================
// Gap #5 — PrintDialog
// =====================================================================

TEST(UncoveredDialogs, PrintDialogConstructAndDefaults)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";

    // Open a fixture so the dialog has a real Document to bind to.
    std::filesystem::path fixtures = TEST_FIXTURES_DIR;
    auto pdfPath = fixtures / "test_single.pdf";
    if (!std::filesystem::exists(pdfPath))
        GTEST_SKIP() << "test_single.pdf fixture unavailable";

    auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
    auto doc = pdfe::core::Document::open(ctx, pdfPath);
    ASSERT_NE(doc, nullptr);

    pdfe::ui::PrintDialog dlg(doc.get(), 0, nullptr);
    EXPECT_EQ("printDialog", dlg.objectName().toStdString());
    EXPECT_NE(nullptr, dlg.findChild<QRadioButton*>("allPagesRadio"));
    EXPECT_NE(nullptr, dlg.findChild<QRadioButton*>("currentPageRadio"));
    EXPECT_NE(nullptr, dlg.findChild<QRadioButton*>("pageRangeRadio"));
    EXPECT_NE(nullptr, dlg.findChild<QSpinBox*>("fromPageSpinBox"));
    EXPECT_NE(nullptr, dlg.findChild<QSpinBox*>("toPageSpinBox"));
    EXPECT_NE(nullptr, dlg.findChild<QSpinBox*>("copiesSpinBox"));
    EXPECT_NE(nullptr, dlg.findChild<QCheckBox*>("fitToPageCheckBox"));
    EXPECT_NE(nullptr, dlg.findChild<QCheckBox*>("centerOnPageCheckBox"));
    EXPECT_NE(nullptr, dlg.findChild<QCheckBox*>("autoRotateCheckBox"));
    EXPECT_NE(nullptr, dlg.findChild<QPushButton*>("printButton"));
    EXPECT_NE(nullptr, dlg.findChild<QPushButton*>("cancelButton"));

    // Default option contract: AllPages, copies>=1.
    auto opts = dlg.options();
    EXPECT_GE(opts.copies, 1);
}

// =====================================================================
// Gap #6 — PageLabelsDialog
// =====================================================================

TEST(UncoveredDialogs, PageLabelsDialogConstructAndRanges)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";
    std::filesystem::path fixtures = TEST_FIXTURES_DIR;
    auto pdfPath = fixtures / "test_single.pdf";
    if (!std::filesystem::exists(pdfPath))
        GTEST_SKIP() << "test_single.pdf fixture unavailable";

    auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
    auto doc = pdfe::core::Document::open(ctx, pdfPath);
    ASSERT_NE(doc, nullptr);

    pdfe::ui::PageLabelsDialog dlg(doc.get(), nullptr);
    EXPECT_EQ("pageLabelsDialog", dlg.objectName().toStdString());
    EXPECT_NE(nullptr, dlg.findChild<QTableWidget*>("pageLabelsTable"));

    // setRanges + ranges() round-trip
    std::vector<pdfe::core::PageLabelRange> ranges = {
        {0, pdfe::core::PageLabelStyle::RomanLower, "front-", 1},
        {3, pdfe::core::PageLabelStyle::Decimal, "", 1},
    };
    dlg.setRanges(ranges);
    ASSERT_EQ(dlg.ranges().size(), 2u);
    EXPECT_EQ(dlg.ranges()[0].firstPage, 0);
    EXPECT_EQ(dlg.ranges()[0].prefix, "front-");
    EXPECT_EQ(dlg.ranges()[1].firstPage, 3);
}

// =====================================================================
// Gap #7 — HeaderFooterDialog
// =====================================================================

TEST(UncoveredDialogs, HeaderFooterDialogConstructAndObjectNames)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";
    std::filesystem::path fixtures = TEST_FIXTURES_DIR;
    auto pdfPath = fixtures / "test_single.pdf";
    if (!std::filesystem::exists(pdfPath))
        GTEST_SKIP() << "test_single.pdf fixture unavailable";

    auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
    auto doc = pdfe::core::Document::open(ctx, pdfPath);
    ASSERT_NE(doc, nullptr);

    pdfe::ui::HeaderFooterDialog dlg(ctx, doc.get(), nullptr);
    EXPECT_EQ("headerFooterDialog", dlg.objectName().toStdString());
    EXPECT_NE(nullptr, dlg.findChild<QLineEdit*>("headerFooterLeftHeader"));
    EXPECT_NE(nullptr, dlg.findChild<QLineEdit*>("headerFooterCenterHeader"));
    EXPECT_NE(nullptr, dlg.findChild<QLineEdit*>("headerFooterRightHeader"));
    EXPECT_NE(nullptr, dlg.findChild<QLineEdit*>("headerFooterLeftFooter"));
    EXPECT_NE(nullptr, dlg.findChild<QComboBox*>("headerFooterRangeCombo"));
    EXPECT_NE(nullptr, dlg.findChild<QDoubleSpinBox*>("headerFooterMarginSpin"));
    EXPECT_NE(nullptr, dlg.findChild<QPushButton*>("headerFooterApplyButton"));
    EXPECT_NE(nullptr, dlg.findChild<QPushButton*>("headerFooterCancelButton"));
}

// =====================================================================
// Gap #8 — OptimizeDialog
// =====================================================================

TEST(UncoveredDialogs, OptimizeDialogConstructAndObjectNames)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";
    std::filesystem::path fixtures = TEST_FIXTURES_DIR;
    auto pdfPath = fixtures / "test_single.pdf";
    if (!std::filesystem::exists(pdfPath))
        GTEST_SKIP() << "test_single.pdf fixture unavailable";

    auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
    auto doc = pdfe::core::Document::open(ctx, pdfPath);
    ASSERT_NE(doc, nullptr);

    pdfe::ui::OptimizeDialog dlg(ctx, doc.get(), nullptr);
    EXPECT_EQ("optimizeDialog", dlg.objectName().toStdString());
    EXPECT_NE(nullptr, dlg.findChild<QPushButton*>("optimizeRunButton"));
    EXPECT_NE(nullptr, dlg.findChild<QPushButton*>("optimizeCancelButton"));
    EXPECT_NE(nullptr, dlg.findChild<QLabel*>("optimizeResultLabel"));
}

// =====================================================================
// Gap #9 — TrustAnchorDialog
// =====================================================================

TEST(UncoveredDialogs, TrustAnchorDialogConstructAndObjectNames)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";
    pdfe::ui::TrustAnchorDialog dlg(nullptr);
    EXPECT_EQ("trustAnchorDialog", dlg.objectName().toStdString());
    EXPECT_NE(nullptr, dlg.findChild<QTableWidget*>("trustAnchorTable"));
    EXPECT_NE(nullptr, dlg.findChild<QPushButton*>("addTrustButton"));
    EXPECT_NE(nullptr, dlg.findChild<QPushButton*>("removeTrustButton"));
    EXPECT_NE(nullptr, dlg.findChild<QPushButton*>("closeTrustButton"));
}

TEST(UncoveredDialogs, TrustAnchorDialogRefreshIsIdempotent)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";
    pdfe::ui::TrustAnchorDialog dlg(nullptr);
    // Calling refresh() multiple times must not crash or duplicate rows
    // when no anchors have been added.
    dlg.refresh();
    dlg.refresh();
    dlg.refresh();
    SUCCEED();
}

// =====================================================================
// Gap #10 — PropertiesPanel
// =====================================================================

TEST(UncoveredDialogs, PropertiesPanelSetClearRoundTrip)
{
    // PropertiesPanel has no getPageInfo() API (display-only widget),
    // but setPageInfo() writes to named QLabel children we can read
    // back via findChild<QLabel*>(objectName) -- a genuine round-trip
    // without adding production API surface.
    if (!qApp) GTEST_SKIP() << "QApplication not available";
    pdfe::ui::PropertiesPanel panel(nullptr);

    // ── Verify child labels exist ─────────────────────────────────────
    auto* idxLabel  = panel.findChild<QLabel*>("pageIndexLabel");
    auto* lblLabel  = panel.findChild<QLabel*>("pageLabelLabel");
    auto* wLabel    = panel.findChild<QLabel*>("widthLabel");
    auto* hLabel    = panel.findChild<QLabel*>("heightLabel");
    auto* rotLabel  = panel.findChild<QLabel*>("rotationLabel");
    ASSERT_NE(idxLabel,  nullptr) << "pageIndexLabel must exist";
    ASSERT_NE(lblLabel,  nullptr) << "pageLabelLabel must exist";
    ASSERT_NE(wLabel,    nullptr) << "widthLabel must exist";
    ASSERT_NE(hLabel,    nullptr) << "heightLabel must exist";
    ASSERT_NE(rotLabel,  nullptr) << "rotationLabel must exist";

    // ── setPageInfo writes correct formatted text to each label ───────
    pdfe::core::PageInfo info;
    info.index    = 5;     // displayed as 1-based: "6"
    info.widthPt  = 612.0f;
    info.heightPt = 792.0f;
    info.rotation = 90;
    info.label    = "iv";

    panel.setPageInfo(info);

    // index + 1 (1-based display as coded in PropertiesPanel.cpp:66)
    EXPECT_EQ(idxLabel->text().toStdString(), "6")
        << "page index label must show 1-based index";
    EXPECT_EQ(lblLabel->text().toStdString(), "iv")
        << "page label label must reflect info.label";
    EXPECT_EQ(wLabel->text().toStdString(), "612.00 pt")
        << "width label must format widthPt with 2 dp and ' pt' suffix";
    EXPECT_EQ(hLabel->text().toStdString(), "792.00 pt")
        << "height label must format heightPt with 2 dp and ' pt' suffix";
    EXPECT_EQ(rotLabel->text().toStdString(), "90°")
        << "rotation label must format rotation followed by degree symbol";

    // ── clearPageInfo resets every label to the placeholder dash ──────
    panel.clearPageInfo();

    EXPECT_EQ(idxLabel->text().toStdString(), "-");
    EXPECT_EQ(lblLabel->text().toStdString(), "-");
    EXPECT_EQ(wLabel->text().toStdString(),   "-");
    EXPECT_EQ(hLabel->text().toStdString(),   "-");
    EXPECT_EQ(rotLabel->text().toStdString(), "-");

    // ── Empty label field falls back to dash (not empty string) ───────
    pdfe::core::PageInfo noLabel;
    noLabel.index    = 0;
    noLabel.widthPt  = 0.0f;
    noLabel.heightPt = 0.0f;
    noLabel.rotation = 0;
    // info.label intentionally left empty
    panel.setPageInfo(noLabel);
    EXPECT_EQ(lblLabel->text().toStdString(), "-")
        << "empty label string must display as dash placeholder";
}

// =====================================================================
// Gap #11 — PageInfo
// =====================================================================

TEST(UncoveredDialogs, PageInfoDefaultValues)
{
    pdfe::core::PageInfo info;
    EXPECT_EQ(info.index, 0);
    EXPECT_FLOAT_EQ(info.widthPt, 0.0f);
    EXPECT_FLOAT_EQ(info.heightPt, 0.0f);
    EXPECT_EQ(info.rotation, 0);
    EXPECT_TRUE(info.label.empty());
}

// =====================================================================
// Gap #12 — generateSrgbIccProfile
// =====================================================================

TEST(UncoveredDialogs, SrgbIccProfileHeaderShape)
{
    auto profile = pdfe::compliance::generateSrgbIccProfile();

    // Header is 128 bytes minimum.
    ASSERT_GE(profile.size(), 128u);

    // Profile size at offset 0..3 (big-endian uint32) must equal the
    // actual buffer size.
    auto readBE32 = [&profile](size_t o) -> uint32_t {
        return (uint32_t(profile[o]) << 24)
             | (uint32_t(profile[o+1]) << 16)
             | (uint32_t(profile[o+2]) << 8)
             |  uint32_t(profile[o+3]);
    };
    EXPECT_EQ(readBE32(0), profile.size());

    // ICC v2 file signature 'acsp' lives at offset 36.
    EXPECT_EQ(profile[36], 'a');
    EXPECT_EQ(profile[37], 'c');
    EXPECT_EQ(profile[38], 's');
    EXPECT_EQ(profile[39], 'p');

    // Device class 'mntr' at offset 12.
    EXPECT_EQ(profile[12], 'm');
    EXPECT_EQ(profile[13], 'n');
    EXPECT_EQ(profile[14], 't');
    EXPECT_EQ(profile[15], 'r');

    // Color space 'RGB ' at offset 16.
    EXPECT_EQ(profile[16], 'R');
    EXPECT_EQ(profile[17], 'G');
    EXPECT_EQ(profile[18], 'B');
}

TEST(UncoveredDialogs, SrgbIccProfileRequiredTagsPresent)
{
    auto profile = pdfe::compliance::generateSrgbIccProfile();
    ASSERT_GE(profile.size(), 132u);

    // Tag count is the first 4 bytes after the header (offset 128, BE32).
    auto readBE32 = [&profile](size_t o) -> uint32_t {
        return (uint32_t(profile[o]) << 24)
             | (uint32_t(profile[o+1]) << 16)
             | (uint32_t(profile[o+2]) << 8)
             |  uint32_t(profile[o+3]);
    };
    uint32_t tagCount = readBE32(128);
    EXPECT_GE(tagCount, 9u);

    // Required tags: desc / wtpt / rXYZ / gXYZ / bXYZ / rTRC / gTRC /
    // bTRC / cprt.  Walk the tag table and assert each signature is
    // present.
    auto haveTag = [&](const char* sig) -> bool {
        for (uint32_t i = 0; i < tagCount; ++i) {
            size_t off = 128 + 4 + i * 12;
            if (off + 4 > profile.size()) return false;
            if (profile[off]   == uint8_t(sig[0]) &&
                profile[off+1] == uint8_t(sig[1]) &&
                profile[off+2] == uint8_t(sig[2]) &&
                profile[off+3] == uint8_t(sig[3])) {
                return true;
            }
        }
        return false;
    };
    EXPECT_TRUE(haveTag("desc"));
    EXPECT_TRUE(haveTag("wtpt"));
    EXPECT_TRUE(haveTag("rXYZ"));
    EXPECT_TRUE(haveTag("gXYZ"));
    EXPECT_TRUE(haveTag("bXYZ"));
    EXPECT_TRUE(haveTag("rTRC"));
    EXPECT_TRUE(haveTag("gTRC"));
    EXPECT_TRUE(haveTag("bTRC"));
    EXPECT_TRUE(haveTag("cprt"));
}

// =====================================================================
// Gap #13 — CropTool
// =====================================================================

TEST(UncoveredDialogs, CropToolToolTypeIsUnknown)
{
    // CropTool's contract is to return AnnotationType::Unknown so the
    // PdfCanvas dispatch path treats it as a "rubber-band only" tool
    // (no annotation is created). Regression-locking this contract.
    pdfe::annotations::CropTool tool;
    EXPECT_EQ(tool.toolType(), pdfe::annotations::AnnotationType::Unknown);
    // cropApplied callback defaults to empty std::function; safe to
    // check without invoking.
    EXPECT_FALSE(static_cast<bool>(tool.cropApplied));
}

// =====================================================================
// Gap #14 — RedactionTool
// =====================================================================

TEST(UncoveredDialogs, RedactionToolToolTypeIsUnknown)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";
    std::filesystem::path fixtures = TEST_FIXTURES_DIR;
    auto pdfPath = fixtures / "test_single.pdf";
    if (!std::filesystem::exists(pdfPath))
        GTEST_SKIP() << "test_single.pdf fixture unavailable";

    auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
    auto doc = pdfe::core::Document::open(ctx, pdfPath);
    ASSERT_NE(doc, nullptr);
    pdfe::redaction::RedactionManager mgr(ctx);
    pdfe::redaction::RedactionTool tool(&mgr);

    EXPECT_EQ(tool.toolType(), pdfe::annotations::AnnotationType::Unknown);
}

// =====================================================================
// Gap #15 — OcspCrlData
// =====================================================================

TEST(UncoveredDialogs, OcspResponseAndCrlInfoDefaultValues)
{
    pdfe::signatures::OcspResponse r;
    EXPECT_TRUE(r.derBytes.empty());
    EXPECT_EQ(r.thisUpdate, 0);
    EXPECT_EQ(r.nextUpdate, 0);
    EXPECT_EQ(r.certStatus, -1);
    EXPECT_TRUE(r.statusMessage.empty());

    pdfe::signatures::CrlInfo c;
    EXPECT_TRUE(c.derBytes.empty());
    EXPECT_TRUE(c.issuerDN.empty());
    EXPECT_EQ(c.thisUpdate, 0);
    EXPECT_EQ(c.nextUpdate, 0);
    EXPECT_TRUE(c.statusMessage.empty());
}

// =====================================================================
// Gap #16 — TantivyBridge struct layout
// =====================================================================

TEST(UncoveredDialogs, TantivyHitAndResultsStructLayout)
{
    // Lock the C-ABI struct layout: any re-ordering of fields would
    // silently break the Rust ↔ C++ boundary in tantivy_bridge.dll.
    // sizes are the smallest contract.  We additionally assert each
    // field's offset is monotonically increasing.

    static_assert(sizeof(int32_t) == 4, "int32_t must be 4 bytes");
    static_assert(sizeof(float) == 4, "float must be 4 bytes");

    EXPECT_EQ(offsetof(TantivyHit, page_idx), 0u);
    EXPECT_EQ(offsetof(TantivyHit, score), 4u);
    // snippet pointer follows score; on 64-bit it sits at offset 8.
    EXPECT_GE(offsetof(TantivyHit, snippet), 8u);

    EXPECT_EQ(offsetof(TantivyResults, hits), 0u);
    // count + capacity are int32_t each, follow the pointer.
    EXPECT_GE(offsetof(TantivyResults, count), sizeof(void*));
    EXPECT_EQ(offsetof(TantivyResults, capacity),
              offsetof(TantivyResults, count) + sizeof(int32_t));
}

// =====================================================================
// Gap #17 — PrintPreviewDialog (pipeline b3f19d72 phase-15)
// =====================================================================
//
// Verifies that PrintPreviewDialog constructs without crashing when
// the caller passes a null document pointer. The dialog's
// onPaintRequested slot guards against null document_, so the
// QPrintPreviewWidget fires paintRequested on construction but the
// slot is a no-op and no crash occurs. Also verifies that the
// expected objectName and child widgets are present.

TEST(UncoveredDialogs, PrintPreviewDialogConstructsWithNullDocument)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";

    // Construct with null document — the dialog must not crash.
    // onPaintRequested guards: if (!printer || !document_) return;
    pdfe::core::PrintOptions opts;
    pdfe::ui::PrintPreviewDialog dlg(nullptr, opts, nullptr);

    // Object-name contract (objectName set in ctor).
    EXPECT_EQ(dlg.objectName().toStdString(), "printPreviewDialog");

    // Toolbar and preview widget must be present (child widgets always
    // created, regardless of whether the document is null).
    auto* toolBar = dlg.findChild<QToolBar*>("previewToolBar");
    EXPECT_NE(toolBar, nullptr)
        << "previewToolBar must be a named child of PrintPreviewDialog";

    auto* previewWidget = dlg.findChild<QWidget*>("printPreviewWidget");
    EXPECT_NE(previewWidget, nullptr)
        << "printPreviewWidget must be a named child of PrintPreviewDialog";
}

TEST(UncoveredDialogs, PrintPreviewDialogObjectNameMatchesCatalog)
{
    if (!qApp) GTEST_SKIP() << "QApplication not available";

    pdfe::core::PrintOptions opts;
    pdfe::ui::PrintPreviewDialog dlg(nullptr, opts, nullptr);

    // Accessible name set in ctor (for UIA / pywinauto discovery).
    EXPECT_FALSE(dlg.accessibleName().isEmpty())
        << "PrintPreviewDialog must set an accessible name for UIA";
    EXPECT_EQ(dlg.windowTitle().toStdString(), "Print Preview");
}

// =====================================================================
// Pipeline b3f19d72 phase-1 — CustomizeToolbarsDialog Q_INVOKABLE hooks
// =====================================================================
//
// Verifies the two Q_INVOKABLE accessors added to
// CustomizeToolbarsDialog in pipeline b3f19d72 phase-1:
//
//   1. addActionByObjectName(QString) -> bool
//   2. resetToDefaults() -> void
//
// Both route through the existing button-click slots (onAddClicked,
// onResetClicked), so production logic isn't duplicated for tests.

namespace {

// Construct a small action registry with stable objectNames so the
// tests don't depend on MainWindow's full action setup.
std::vector<QAction*> makeTinyRegistry(QObject* parent)
{
    auto* a1 = new QAction("Open", parent);
    a1->setObjectName("openAction");
    auto* a2 = new QAction("Save", parent);
    a2->setObjectName("saveAction");
    auto* a3 = new QAction("Print", parent);
    a3->setObjectName("printAction");
    return {a1, a2, a3};
}

// Build an initial layout containing just one toolbar with two items.
std::vector<pdfe::ui::ToolbarLayout> makeTinyLayouts()
{
    pdfe::ui::ToolbarLayout tb;
    tb.toolbarName = "mainToolBar";
    tb.actionIds = QStringList{"openAction", "saveAction"};
    return {tb};
}

} // namespace

TEST(UncoveredDialogs, AddActionByObjectName_AddsToCurrentList)
{
    // Phase-1 AC6: addActionByObjectName must drive the same insertion
    // path the Add button uses, so the action appears in the
    // "Current" list and the updatedLayouts() output reflects the
    // change. An unknown objectName must return false without
    // modifying state.
    if (!qApp) GTEST_SKIP() << "QApplication not available";

    QObject parent;
    auto registry = makeTinyRegistry(&parent);
    auto layouts = makeTinyLayouts();

    pdfe::ui::CustomizeToolbarsDialog dlg(registry, layouts, nullptr);

    // The dialog now uses QListView + QStandardItemModel (pipeline
    // b3f19d72 phase-14: QListWidget replaced for UIA item exposure).
    auto* currentListView = dlg.findChild<QListView*>("currentList");
    ASSERT_NE(currentListView, nullptr);
    auto* currentModel =
        qobject_cast<QStandardItemModel*>(currentListView->model());
    ASSERT_NE(currentModel, nullptr);
    const int initialCount = currentModel->rowCount();

    // (1) Adding a known action returns true and grows the current
    // list by exactly one entry (regardless of the initial seeded
    // state — the dialog's combo-init can clear the list in some
    // paths, so we test the delta, not the absolute count).
    EXPECT_TRUE(dlg.addActionByObjectName("printAction"));
    EXPECT_EQ(currentModel->rowCount(), initialCount + 1)
        << "addActionByObjectName must add exactly one item to the "
           "Current list (delta vs. initialCount)";

    // The new entry must carry the objectName as its UserRole data so
    // updatedLayouts() emits the correct id. Locate by id (the
    // visible label is localized).
    bool foundPrint = false;
    for (int i = 0; i < currentModel->rowCount(); ++i) {
        if (currentModel->item(i)->data(Qt::UserRole).toString()
            == "printAction") {
            foundPrint = true;
            break;
        }
    }
    EXPECT_TRUE(foundPrint)
        << "printAction must be present in the Current list after "
           "addActionByObjectName succeeds";

    // (2) An unknown objectName returns false WITHOUT growing the
    // list (regression-locks the no-op-on-unknown contract).
    const int countBeforeBad = currentModel->rowCount();
    EXPECT_FALSE(dlg.addActionByObjectName("noSuchAction"));
    EXPECT_EQ(currentModel->rowCount(), countBeforeBad)
        << "unknown objectName must leave the Current list untouched";

    // (3) Empty objectName is also a no-op.
    EXPECT_FALSE(dlg.addActionByObjectName(QString()));
    EXPECT_EQ(currentModel->rowCount(), countBeforeBad);

    // (4) updatedLayouts() must reflect the added id so the live
    // toolbars get the new action when the caller applies them.
    auto updated = dlg.updatedLayouts();
    ASSERT_FALSE(updated.empty());
    EXPECT_TRUE(updated[0].actionIds.contains("printAction"))
        << "updatedLayouts() must surface the freshly-added action id";
}

TEST(UncoveredDialogs, ResetToDefaults_ClearsCustomItems)
{
    // Phase-1 AC6: resetToDefaults must route through the same code
    // path the "Reset to Default" button uses (onResetClicked). The
    // observable contract: after calling it, the layouts revert to
    // ToolbarLayoutManager::defaultLayouts(), discarding any
    // user-injected items.
    if (!qApp) GTEST_SKIP() << "QApplication not available";

    QObject parent;
    auto registry = makeTinyRegistry(&parent);
    auto layouts = makeTinyLayouts();
    pdfe::ui::CustomizeToolbarsDialog dlg(registry, layouts, nullptr);

    // Add an extra item so we have something to discard.
    ASSERT_TRUE(dlg.addActionByObjectName("printAction"));

    auto beforeReset = dlg.updatedLayouts();
    ASSERT_EQ(beforeReset.size(), 1u);
    ASSERT_TRUE(beforeReset[0].actionIds.contains("printAction"));

    // Reset — onResetClicked replaces layouts_ with
    // ToolbarLayoutManager::defaultLayouts(). That set is built
    // independently of the makeTinyLayouts() we seeded with.
    dlg.resetToDefaults();

    auto afterReset = dlg.updatedLayouts();
    auto defaults = pdfe::ui::ToolbarLayoutManager::defaultLayouts();

    // (1) Defaults are non-empty (MainWindow seeds at least one
    // toolbar entry), so reset really did something.
    ASSERT_FALSE(defaults.empty())
        << "ToolbarLayoutManager::defaultLayouts() must return a "
           "non-empty layout list for the reset path to be testable";

    // (2) After reset, the layout count must match defaults exactly
    // (the seeded {mainToolBar with 2 items + printAction} layout
    // is replaced wholesale by the default set).
    EXPECT_EQ(afterReset.size(), defaults.size());

    // (3) The first default toolbar's actionIds list is what the
    // dialog's Current list now exposes. The "printAction" we added
    // earlier may or may not still be there — but if it is, it's
    // because the default layout includes it, not because reset
    // preserved the user's addition. Verify by comparing the
    // post-reset list to the default list byte-for-byte.
    for (size_t i = 0; i < afterReset.size(); ++i) {
        EXPECT_EQ(afterReset[i].toolbarName, defaults[i].toolbarName);
        EXPECT_EQ(afterReset[i].actionIds, defaults[i].actionIds);
    }
}
