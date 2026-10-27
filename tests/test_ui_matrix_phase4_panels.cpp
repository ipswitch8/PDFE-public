// Pipeline p86-followups phase-4 — turn UI-matrix `planned` cells into tests
// that genuinely assert an outcome.
//
// Matrix rows covered by this file:
//   layer_panel_create          LayerPanelButtonFlowTest.CreateButtonAddsTreeItemNamedAsTyped
//   layer_panel_delete          LayerPanelButtonFlowTest.DeleteButtonRemovesTheSelectedLayerOnly
//   layer_panel_rename          LayerPanelButtonFlowTest.RenameButtonChangesSelectedItemTextToTypedName
//   defaults_log_multi          DefaultsLogPanelFlowTest.FiveLogsProduceFiveEntriesAllVisible
//   defaults_log_cycle          DefaultsLogPanelFlowTest.ClearThenLogLeavesOnlyTheNewEntry
//   defaults_log_widget_audit   DefaultsLogPanelFlowTest.AuditedWidgetsAreReachableAndLive
//
// Oracle discipline (tests/docs — "vacuous_oracle_three_shapes"):
//   * every assertion compares against an INPUT (the name typed into the
//     dialog, the string handed to logDefault) or an independently-known
//     fixture property — never against a second read of the same code path;
//   * every test first guards that the measurement HAPPENED. For the layer
//     panel that is `driver.driven() >= 1`: all three slots early-return when
//     `document_` is null, in which case a bare count assertion would be
//     comparing 0 to 0 and passing for the wrong reason.

#include <gtest/gtest.h>

#include "ui/LayerPanel.h"
#include "ui/DefaultsLogPanel.h"
#include "layers/LayerManager.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"

#include <QApplication>
#include <QInputDialog>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QStringList>
#include <QTextEdit>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItem>

#include <filesystem>
#include <memory>
#include <utility>

#ifndef TEST_FIXTURES_DIR
#define TEST_FIXTURES_DIR "."
#endif

using pdfe::core::Document;
using pdfe::core::MuPdfContext;
using pdfe::layers::LayerManager;
using pdfe::ui::DefaultsLogPanel;
using pdfe::ui::LayerPanel;

namespace {

// ---------------------------------------------------------------------------
// ModalDriver
// ---------------------------------------------------------------------------
//
// Drives whatever app-modal dialog appears while it is alive, and counts how
// many it drove.
//
// A REPEATING timer, not a single-shot: QInputDialog::getText() and
// QMessageBox::question() each spin their own nested event loop, so the driver
// has to keep firing *inside* that loop until the dialog is gone. (Same reason
// as the ModalDismisser in tests/test_ui_clipboard_dragdrop.cpp.)
//
// Unlike that dismisser this one supplies an ANSWER rather than just closing:
// LayerPanel::onCreateLayer/onRenameLayer need `ok == true` plus a name, and
// plain close() yields `ok == false`, which makes the slot return without
// doing anything — a silently vacuous test.
//
// The tick cap is load-bearing. pdfe_tests runs against the real `windows` QPA
// plugin (tests/test_main.cpp never sets QT_QPA_PLATFORM=offscreen), so a
// dialog that is never driven is a genuine hang rather than a no-op. After the
// cap the driver force-closes, so the test FAILS on its driven() guard instead
// of wedging the whole suite.
class ModalDriver {
public:
    explicit ModalDriver(QString replyText = {},
                         QMessageBox::StandardButton answer = QMessageBox::Yes)
        : replyText_(std::move(replyText)), answer_(answer)
    {
        timer_.setInterval(10);
        QObject::connect(&timer_, &QTimer::timeout, [this] { tick(); });
        timer_.start();
    }

    ~ModalDriver() { timer_.stop(); }

    ModalDriver(const ModalDriver&) = delete;
    ModalDriver& operator=(const ModalDriver&) = delete;

    /// Number of modal dialogs actually answered (not merely seen).
    [[nodiscard]] int driven() const noexcept { return driven_; }

private:
    void tick()
    {
        QWidget* modal = QApplication::activeModalWidget();
        if (!modal) {
            return;
        }
        if (modal == lastDriven_) {
            // Already answered this one; it is still unwinding its event loop.
            return;
        }
        if (++ticks_ > kMaxTicks) {
            modal->close();
            return;
        }

        if (auto* input = qobject_cast<QInputDialog*>(modal)) {
            input->setTextValue(replyText_);
            lastDriven_ = modal;
            ++driven_;
            input->accept();  // -> QInputDialog::getText() returns ok == true
            return;
        }

        if (auto* box = qobject_cast<QMessageBox*>(modal)) {
            if (auto* btn = box->button(answer_)) {
                lastDriven_ = modal;
                ++driven_;
                btn->click();  // records clickedButton() -> exec() returns `answer_`
                return;
            }
        }

        modal->close();
    }

    static constexpr int kMaxTicks = 400;  // ~4 s at 10 ms

    QTimer timer_;
    QString replyText_;
    QMessageBox::StandardButton answer_;
    QWidget* lastDriven_ = nullptr;
    int driven_ = 0;
    int ticks_ = 0;
};

// ---------------------------------------------------------------------------
// LayerPanel — button click -> tree mutation
// ---------------------------------------------------------------------------
//
// The manager-level behaviour (createLayer/renameLayer/deleteLayer) is already
// covered by LayerManagerTest in tests/test_layers.cpp, and the panel's static
// widget inventory by LayerPanelTest there and by
// LayerPanelInteractionTest in tests/test_panel_interaction_phase8.cpp.
// The gap those leave, and the one the matrix names, is the PANEL level:
// pressing the button must drive its dialog and move the tree.
class LayerPanelButtonFlowTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        const std::filesystem::path fixture =
            std::filesystem::path(TEST_FIXTURES_DIR) / "test_3page.pdf";
        if (!std::filesystem::exists(fixture)) {
            GTEST_SKIP() << "test_3page.pdf fixture unavailable";
        }

        context_ = std::make_shared<MuPdfContext>();
        document_ = Document::open(context_, fixture);
        ASSERT_NE(document_, nullptr) << "could not open test_3page.pdf";

        panel_ = std::make_unique<LayerPanel>();
        panel_->setContext(context_);
        panel_->setDocument(document_.get());

        tree_ = panel_->findChild<QTreeWidget*>("layerTreeWidget");
        createButton_ = panel_->findChild<QPushButton*>("createLayerButton");
        deleteButton_ = panel_->findChild<QPushButton*>("deleteLayerButton");
        renameButton_ = panel_->findChild<QPushButton*>("renameLayerButton");
        ASSERT_NE(tree_, nullptr);
        ASSERT_NE(createButton_, nullptr);
        ASSERT_NE(deleteButton_, nullptr);
        ASSERT_NE(renameButton_, nullptr);
    }

    void TearDown() override
    {
        // Never leave a modal behind for the next test in the binary.
        if (auto* modal = QApplication::activeModalWidget()) {
            modal->close();
            QApplication::processEvents();
        }
        panel_.reset();
        document_.reset();
        context_.reset();
    }

    /// Seed layers through the manager API (already independently covered) so
    /// a panel-level test can start from a known, NAMED population.
    void seedLayers(const QStringList& names)
    {
        LayerManager mgr(context_);
        for (const QString& n : names) {
            ASSERT_GE(mgr.createLayer(document_.get(), n.toStdString()), 0)
                << "seeding layer failed: " << n.toStdString();
        }
        panel_->refresh();
        ASSERT_EQ(tree_->topLevelItemCount(), static_cast<int>(names.size()))
            << "seeded population did not reach the tree";
    }

    std::shared_ptr<MuPdfContext> context_;
    std::unique_ptr<Document> document_;
    std::unique_ptr<LayerPanel> panel_;
    QTreeWidget* tree_ = nullptr;
    QPushButton* createButton_ = nullptr;
    QPushButton* deleteButton_ = nullptr;
    QPushButton* renameButton_ = nullptr;
};

// Matrix row: layer_panel_create
TEST_F(LayerPanelButtonFlowTest, CreateButtonAddsTreeItemNamedAsTyped)
{
    // Independently known: test_3page.pdf ships with no OCGs. Asserted rather
    // than assumed, so this test cannot silently become a 0-vs-0 comparison if
    // the fixture ever gains layers.
    const int before = tree_->topLevelItemCount();
    ASSERT_EQ(before, 0) << "fixture precondition: test_3page.pdf has no layers";

    const QString kTypedName = QStringLiteral("Phase4 Created Layer");
    ModalDriver driver(kTypedName);
    createButton_->click();

    // Guard that the measurement HAPPENED. onCreateLayer() early-returns when
    // document_/layerManager_ is null, and QInputDialog returning ok == false
    // is also a no-op; either way the count assertion below would pass for the
    // wrong reason.
    ASSERT_GE(driver.driven(), 1)
        << "createLayerButton never opened a dialog — the slot did not run";

    EXPECT_EQ(tree_->topLevelItemCount(), before + 1);

    ASSERT_GT(tree_->topLevelItemCount(), 0) << "nothing to inspect";
    // Compare against the INPUT: the exact string typed into the dialog.
    EXPECT_EQ(tree_->topLevelItem(0)->text(0), kTypedName);
}

// Matrix row: layer_panel_delete
TEST_F(LayerPanelButtonFlowTest, DeleteButtonRemovesTheSelectedLayerOnly)
{
    // Two DISTINCT, independently-known names, so the assertion can say which
    // layer survived rather than merely that the count fell.
    const QString kDoomed = QStringLiteral("Phase4 Doomed Layer");
    const QString kSurvivor = QStringLiteral("Phase4 Surviving Layer");
    // Extra parens: the brace-comma would otherwise split the macro arguments.
    ASSERT_NO_FATAL_FAILURE((seedLayers({kDoomed, kSurvivor})));

    const int before = tree_->topLevelItemCount();
    ASSERT_EQ(before, 2);
    ASSERT_EQ(tree_->topLevelItem(0)->text(0), kDoomed)
        << "seed ordering precondition";

    tree_->setCurrentItem(tree_->topLevelItem(0));
    ASSERT_NE(tree_->currentItem(), nullptr) << "selection precondition";

    ModalDriver driver({}, QMessageBox::Yes);
    deleteButton_->click();

    ASSERT_GE(driver.driven(), 1)
        << "deleteLayerButton never opened its confirmation box";

    EXPECT_EQ(tree_->topLevelItemCount(), before - 1);

    ASSERT_EQ(tree_->topLevelItemCount(), 1) << "nothing to inspect";
    // The RIGHT one went: a count-only oracle would pass even if the survivor
    // had been deleted instead.
    EXPECT_EQ(tree_->topLevelItem(0)->text(0), kSurvivor);
}

// Companion to layer_panel_delete: proves the confirmation gate is real, so
// the test above is measuring the Yes answer and not merely the button press.
TEST_F(LayerPanelButtonFlowTest, DeleteButtonHonoursNoAndKeepsTheLayer)
{
    const QString kKept = QStringLiteral("Phase4 Kept Layer");
    ASSERT_NO_FATAL_FAILURE((seedLayers({kKept})));

    const int before = tree_->topLevelItemCount();
    ASSERT_EQ(before, 1);
    tree_->setCurrentItem(tree_->topLevelItem(0));

    ModalDriver driver({}, QMessageBox::No);
    deleteButton_->click();

    ASSERT_GE(driver.driven(), 1)
        << "deleteLayerButton never opened its confirmation box";

    EXPECT_EQ(tree_->topLevelItemCount(), before);
    ASSERT_EQ(tree_->topLevelItemCount(), 1) << "nothing to inspect";
    EXPECT_EQ(tree_->topLevelItem(0)->text(0), kKept);
}

// Matrix row: layer_panel_rename
TEST_F(LayerPanelButtonFlowTest, RenameButtonChangesSelectedItemTextToTypedName)
{
    const QString kOriginal = QStringLiteral("Phase4 Original Name");
    ASSERT_NO_FATAL_FAILURE((seedLayers({kOriginal})));

    ASSERT_EQ(tree_->topLevelItemCount(), 1);
    const QString textBefore = tree_->topLevelItem(0)->text(0);
    ASSERT_EQ(textBefore, kOriginal) << "seed precondition";

    tree_->setCurrentItem(tree_->topLevelItem(0));
    ASSERT_NE(tree_->currentItem(), nullptr) << "selection precondition";

    const QString kTypedName = QStringLiteral("Phase4 Renamed Layer");
    ASSERT_NE(kTypedName, kOriginal) << "rename must actually change something";

    ModalDriver driver(kTypedName);
    renameButton_->click();

    ASSERT_GE(driver.driven(), 1)
        << "renameLayerButton never opened its dialog";

    // Rename must not add or drop rows.
    EXPECT_EQ(tree_->topLevelItemCount(), 1);
    ASSERT_EQ(tree_->topLevelItemCount(), 1) << "nothing to inspect";

    // Against the INPUT, and against the independently-known prior text.
    EXPECT_EQ(tree_->topLevelItem(0)->text(0), kTypedName);
    EXPECT_NE(tree_->topLevelItem(0)->text(0), textBefore);
}

// ---------------------------------------------------------------------------
// DefaultsLogPanel
// ---------------------------------------------------------------------------

class DefaultsLogPanelFlowTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        panel_ = std::make_unique<DefaultsLogPanel>();
        logView_ = panel_->findChild<QTextEdit*>("defaultsLogView");
        ASSERT_NE(logView_, nullptr);
        ASSERT_EQ(panel_->entryCount(), 0) << "fresh panel precondition";
    }

    void TearDown() override { panel_.reset(); }

    std::unique_ptr<DefaultsLogPanel> panel_;
    QTextEdit* logView_ = nullptr;
};

// Matrix row: defaults_log_multi
TEST_F(DefaultsLogPanelFlowTest, FiveLogsProduceFiveEntriesAllVisible)
{
    // Distinct, independently-known inputs — the oracle below compares the
    // rendered log against THESE strings, not against a second read of the
    // panel's own state.
    const QStringList contexts = {
        QStringLiteral("Phase4 Context Alpha"),
        QStringLiteral("Phase4 Context Bravo"),
        QStringLiteral("Phase4 Context Charlie"),
        QStringLiteral("Phase4 Context Delta"),
        QStringLiteral("Phase4 Context Echo"),
    };
    ASSERT_EQ(static_cast<int>(contexts.size()), 5)
        << "the matrix row specifies five entries";

    QStringList fallbacks;
    for (int i = 0; i < contexts.size(); ++i) {
        const QString fallback = QStringLiteral("Phase4Fallback%1").arg(i);
        fallbacks << fallback;
        panel_->logDefault(contexts.at(i),
                           QStringLiteral("Phase4Requested%1").arg(i),
                           fallback,
                           QStringLiteral("phase-4 matrix coverage"));
    }

    EXPECT_EQ(panel_->entryCount(), 5);

    const QString rendered = logView_->toPlainText();
    // Guard that the measurement happened: an empty view would make every
    // contains() check below trivially meaningful only in the negative.
    ASSERT_FALSE(rendered.isEmpty()) << "log view rendered nothing";

    for (const QString& context : contexts) {
        EXPECT_TRUE(rendered.contains(context))
            << "log view is missing context: " << context.toStdString();
    }
    for (const QString& fallback : fallbacks) {
        EXPECT_TRUE(rendered.contains(fallback))
            << "log view is missing fallback: " << fallback.toStdString();
    }
}

// Matrix row: defaults_log_cycle
TEST_F(DefaultsLogPanelFlowTest, ClearThenLogLeavesOnlyTheNewEntry)
{
    const QString kFirst = QStringLiteral("Phase4 Pre-Clear One");
    const QString kSecond = QStringLiteral("Phase4 Pre-Clear Two");
    const QString kThird = QStringLiteral("Phase4 Post-Clear Only");

    panel_->logDefault(kFirst, QStringLiteral("r1"), QStringLiteral("f1"),
                       QStringLiteral("phase-4"));
    panel_->logDefault(kSecond, QStringLiteral("r2"), QStringLiteral("f2"),
                       QStringLiteral("phase-4"));
    ASSERT_EQ(panel_->entryCount(), 2);
    // Prove the pre-clear entries were really there, otherwise their absence
    // afterwards proves nothing (a permanently-false guard).
    ASSERT_TRUE(logView_->toPlainText().contains(kFirst));
    ASSERT_TRUE(logView_->toPlainText().contains(kSecond));

    panel_->clearLog();
    ASSERT_EQ(panel_->entryCount(), 0) << "clearLog did not reset the counter";

    panel_->logDefault(kThird, QStringLiteral("r3"), QStringLiteral("f3"),
                       QStringLiteral("phase-4"));

    EXPECT_EQ(panel_->entryCount(), 1);

    const QString rendered = logView_->toPlainText();
    ASSERT_FALSE(rendered.isEmpty()) << "log view rendered nothing after relog";
    EXPECT_TRUE(rendered.contains(kThird));
    EXPECT_FALSE(rendered.contains(kFirst)) << "cleared entry came back";
    EXPECT_FALSE(rendered.contains(kSecond)) << "cleared entry came back";
}

// Matrix row: defaults_log_widget_audit
//
// The matrix names three widgets: logView, clearLogButton and statusLabel.
// The panel had no status label until this phase; one was added
// (`defaultsLogStatusLabel`, src/ui/DefaultsLogPanel.cpp) rather than quietly
// narrowing the row to the two widgets that already existed. It mirrors
// LayerPanel's `layerStatusLabel`.
TEST_F(DefaultsLogPanelFlowTest, AuditedWidgetsAreReachableAndLive)
{
    EXPECT_EQ(panel_->objectName(), QStringLiteral("defaultsLogPanel"));

    auto* view = panel_->findChild<QTextEdit*>("defaultsLogView");
    auto* clearButton = panel_->findChild<QPushButton*>("defaultsLogClearButton");
    auto* statusLabel = panel_->findChild<QLabel*>("defaultsLogStatusLabel");

    ASSERT_NE(view, nullptr) << "defaultsLogView not reachable by objectName";
    ASSERT_NE(clearButton, nullptr)
        << "defaultsLogClearButton not reachable by objectName";
    ASSERT_NE(statusLabel, nullptr)
        << "defaultsLogStatusLabel not reachable by objectName";

    // Reachable is not enough — prove each one is wired, not decoration.
    EXPECT_FALSE(statusLabel->text().isEmpty())
        << "status label is present but says nothing";
    const QString statusWhenEmpty = statusLabel->text();

    panel_->logDefault(QStringLiteral("Phase4 Audit"), QStringLiteral("req"),
                       QStringLiteral("fb"), QStringLiteral("phase-4"));
    ASSERT_EQ(panel_->entryCount(), 1);
    EXPECT_NE(statusLabel->text(), statusWhenEmpty)
        << "status label did not react to a logged default";
    EXPECT_TRUE(statusLabel->text().contains(QStringLiteral("1")))
        << "status label does not report the entry count, got: "
        << statusLabel->text().toStdString();
    EXPECT_FALSE(view->toPlainText().isEmpty());

    // The clear button must be connected, not merely present.
    clearButton->click();
    EXPECT_EQ(panel_->entryCount(), 0) << "clear button is not wired";
    EXPECT_TRUE(view->toPlainText().isEmpty());
    EXPECT_EQ(statusLabel->text(), statusWhenEmpty)
        << "status label did not return to its empty-state text";
}

} // namespace
