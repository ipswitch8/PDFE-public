// -*- coding: utf-8 -*-
#include "CustomizeToolbarsDialog.h"

#include <QAction>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QPushButton>
#include <QStandardItemModel>
#include <QVBoxLayout>
#include "ui/TestDialogSeam.h"

namespace pdfe::ui {

namespace {

// Display string for an action: prefer cleaned text, fall back to objectName.
QString labelFor(const QAction* a) {
    if (!a) return QString();
    QString t = a->text();
    t.remove('&');
    if (!t.isEmpty()) return t;
    return a->objectName();
}

QStandardItem* makeActionItem(const QAction* a) {
    auto* it = new QStandardItem();
    it->setText(labelFor(a));
    it->setData(a->objectName(), Qt::UserRole);
    if (!a->icon().isNull()) it->setIcon(a->icon());
    it->setEditable(false);
    return it;
}

QStandardItem* makeSeparatorItem() {
    auto* it = new QStandardItem();
    it->setText(QStringLiteral("— Separator —"));
    it->setData(QString(), Qt::UserRole);
    it->setEditable(false);
    return it;
}

// Clone a QStandardItem (text + UserRole data + icon).
QStandardItem* cloneItem(const QStandardItem* src) {
    auto* it = new QStandardItem();
    it->setText(src->text());
    it->setData(src->data(Qt::UserRole), Qt::UserRole);
    it->setIcon(src->icon());
    it->setEditable(false);
    return it;
}

} // namespace

CustomizeToolbarsDialog::CustomizeToolbarsDialog(
    std::vector<QAction*> actionRegistry,
    std::vector<ToolbarLayout> initialLayouts,
    QWidget* parent)
    : QDialog(parent)
    , registry_(std::move(actionRegistry))
    , layouts_(std::move(initialLayouts))
{
    setupUi();
    if (!layouts_.empty()) {
        for (const auto& l : layouts_) {
            toolbarCombo_->addItem(l.toolbarName);
        }
        selectToolbar(0);
    }

    // Register with the dialog-introspection seam so tests can observe
    // this dialog while its modal exec() loop runs (UIA cannot enumerate
    // it). MUST stay last: the seam snapshots the child inventory.
    pdfe::ui::testseam::registerDialogIntrospection(this);
}

void CustomizeToolbarsDialog::setupUi() {
    setObjectName("customizeToolbarsDialog");
    setAccessibleName("Customize Toolbars");
    setWindowTitle(tr("Customize Toolbars"));
    resize(720, 460);

    auto* root = new QVBoxLayout(this);

    // Toolbar selector row
    auto* selectorRow = new QHBoxLayout();
    selectorRow->addWidget(new QLabel(tr("Toolbar:"), this));
    toolbarCombo_ = new QComboBox(this);
    toolbarCombo_->setObjectName("toolbarCombo");
    toolbarCombo_->setAccessibleName("Toolbar Selector");
    selectorRow->addWidget(toolbarCombo_, 1);
    root->addLayout(selectorRow);
    connect(toolbarCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &CustomizeToolbarsDialog::onToolbarSelected);

    // Two-list editor — QListView + QStandardItemModel for proper UIA
    // item exposure (Qt6's QListWidget does not propagate ListItem
    // descendants to the UIA tree; QListView with a model does).
    auto* listsRow = new QHBoxLayout();

    auto* availCol = new QVBoxLayout();
    availCol->addWidget(new QLabel(tr("Available actions"), this));
    availableModel_ = new QStandardItemModel(this);
    availableList_ = new QListView(this);
    availableList_->setObjectName("availableList");
    // GitHub #185: spoken text, matching the QLabel above it. objectName
    // remains the automation id. (The f3c91a2e AutomationId rationale was a
    // misdiagnosis — see the note in BatchDialog.cpp.)
    availableList_->setAccessibleName(tr("Available actions"));
    availableList_->setModel(availableModel_);
    availableList_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    availableList_->setSelectionMode(QAbstractItemView::SingleSelection);
    availCol->addWidget(availableList_);
    listsRow->addLayout(availCol);

    auto* btnCol = new QVBoxLayout();
    btnCol->addStretch();
    addButton_ = new QPushButton(tr("Add ▶"), this);
    addButton_->setObjectName("addActionButton");
    addButton_->setAccessibleName("Add Action To Toolbar");
    removeButton_ = new QPushButton(tr("◀ Remove"), this);
    removeButton_->setObjectName("removeActionButton");
    removeButton_->setAccessibleName("Remove Action From Toolbar");
    addSeparatorButton_ = new QPushButton(tr("Add Separator"), this);
    addSeparatorButton_->setObjectName("addSeparatorButton");
    addSeparatorButton_->setAccessibleName("Add Separator To Toolbar");
    moveUpButton_ = new QPushButton(tr("Move Up"), this);
    moveUpButton_->setObjectName("moveUpButton");
    moveUpButton_->setAccessibleName("Move Action Up");
    moveDownButton_ = new QPushButton(tr("Move Down"), this);
    moveDownButton_->setObjectName("moveDownButton");
    moveDownButton_->setAccessibleName("Move Action Down");
    btnCol->addWidget(addButton_);
    btnCol->addWidget(removeButton_);
    btnCol->addSpacing(8);
    btnCol->addWidget(addSeparatorButton_);
    btnCol->addSpacing(8);
    btnCol->addWidget(moveUpButton_);
    btnCol->addWidget(moveDownButton_);
    btnCol->addStretch();
    listsRow->addLayout(btnCol);

    auto* curCol = new QVBoxLayout();
    curCol->addWidget(new QLabel(tr("Current toolbar contents"), this));
    currentModel_ = new QStandardItemModel(this);
    currentList_ = new QListView(this);
    currentList_->setObjectName("currentList");
    // GitHub #185: spoken text, matching the QLabel above it.
    currentList_->setAccessibleName(tr("Current toolbar contents"));
    currentList_->setModel(currentModel_);
    currentList_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    currentList_->setSelectionMode(QAbstractItemView::SingleSelection);
    curCol->addWidget(currentList_);
    listsRow->addLayout(curCol);

    root->addLayout(listsRow, 1);

    // Bottom buttons
    auto* bottomRow = new QHBoxLayout();
    resetButton_ = new QPushButton(tr("Reset to Default"), this);
    resetButton_->setObjectName("resetToDefaultButton");
    resetButton_->setAccessibleName("Reset Toolbars To Default");
    bottomRow->addWidget(resetButton_);
    bottomRow->addStretch();

    auto* dlgButtons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    dlgButtons->setObjectName("customizeToolbarsButtonBox");
    okButton_ = dlgButtons->button(QDialogButtonBox::Ok);
    okButton_->setObjectName("okButton");
    okButton_->setAccessibleName("Apply Toolbar Changes");
    cancelButton_ = dlgButtons->button(QDialogButtonBox::Cancel);
    cancelButton_->setObjectName("cancelButton");
    cancelButton_->setAccessibleName("Cancel Toolbar Changes");
    bottomRow->addWidget(dlgButtons);
    root->addLayout(bottomRow);

    // QAbstractItemView uses Tab for internal item navigation
    // by default — disable so Tab advances to the next dialog widget.
    availableList_->setTabKeyNavigation(false);
    currentList_->setTabKeyNavigation(false);

    // Ensure all interactive widgets have tab-friendly focus policies.
    toolbarCombo_->setFocusPolicy(Qt::StrongFocus);
    availableList_->setFocusPolicy(Qt::StrongFocus);
    addButton_->setFocusPolicy(Qt::StrongFocus);
    removeButton_->setFocusPolicy(Qt::StrongFocus);
    addSeparatorButton_->setFocusPolicy(Qt::StrongFocus);
    moveUpButton_->setFocusPolicy(Qt::StrongFocus);
    moveDownButton_->setFocusPolicy(Qt::StrongFocus);
    currentList_->setFocusPolicy(Qt::StrongFocus);
    resetButton_->setFocusPolicy(Qt::StrongFocus);
    okButton_->setFocusPolicy(Qt::StrongFocus);
    cancelButton_->setFocusPolicy(Qt::StrongFocus);

    // Explicit tab order: combo → lists → action buttons → dialog buttons.
    setTabOrder(toolbarCombo_, availableList_);
    setTabOrder(availableList_, addButton_);
    setTabOrder(addButton_, removeButton_);
    setTabOrder(removeButton_, addSeparatorButton_);
    setTabOrder(addSeparatorButton_, moveUpButton_);
    setTabOrder(moveUpButton_, moveDownButton_);
    setTabOrder(moveDownButton_, currentList_);
    setTabOrder(currentList_, resetButton_);
    setTabOrder(resetButton_, okButton_);
    setTabOrder(okButton_, cancelButton_);
    setTabOrder(cancelButton_, toolbarCombo_);

    connect(addButton_, &QPushButton::clicked,
            this, &CustomizeToolbarsDialog::onAddClicked);
    connect(removeButton_, &QPushButton::clicked,
            this, &CustomizeToolbarsDialog::onRemoveClicked);
    connect(moveUpButton_, &QPushButton::clicked,
            this, &CustomizeToolbarsDialog::onMoveUpClicked);
    connect(moveDownButton_, &QPushButton::clicked,
            this, &CustomizeToolbarsDialog::onMoveDownClicked);
    connect(addSeparatorButton_, &QPushButton::clicked,
            this, &CustomizeToolbarsDialog::onAddSeparatorClicked);
    connect(resetButton_, &QPushButton::clicked,
            this, &CustomizeToolbarsDialog::onResetClicked);
    connect(dlgButtons, &QDialogButtonBox::accepted,
            this, &QDialog::accept);
    connect(dlgButtons, &QDialogButtonBox::rejected,
            this, &QDialog::reject);

    // ── Pipeline b3f19d72 phase-14 / GAP-18: test-seam pipes ──────────────
    // All hidden 0x0 QLineEdits are gated by PDFE_TEST_PIPES so they are
    // absent in production (env var not set → widgets never created →
    // nothing in the UIA tree).  Pywinauto discovers them via UIA
    // AutomationId matching objectName and writes to them via the Value
    // pattern; the textChanged handler dispatches the corresponding
    // Q_INVOKABLE.  launch.py::_qt_env_for_determinism() sets
    // PDFE_TEST_PIPES=1 for all harness-launched sessions.
    if (pdfe::ui::testseam::testPipesEnabled()) {
        testAddActionPipe_ = new QLineEdit(this);
        testAddActionPipe_->setObjectName("testCustomizeAddActionPipe");
        testAddActionPipe_->setAccessibleName("Test Customize Add Action Pipe");
        testAddActionPipe_->setFixedSize(0, 0);
        testAddActionPipe_->setFocusPolicy(Qt::NoFocus);
        connect(testAddActionPipe_, &QLineEdit::textChanged,
                this, [this](const QString& text) {
            if (text.isEmpty()) return;
            addActionByObjectName(text);
            testAddActionPipe_->blockSignals(true);
            testAddActionPipe_->clear();
            testAddActionPipe_->blockSignals(false);
        });

        testResetToDefaultsPipe_ = new QLineEdit(this);
        testResetToDefaultsPipe_->setObjectName("testCustomizeResetPipe");
        testResetToDefaultsPipe_->setAccessibleName("Test Customize Reset To Defaults Pipe");
        testResetToDefaultsPipe_->setFixedSize(0, 0);
        testResetToDefaultsPipe_->setFocusPolicy(Qt::NoFocus);
        connect(testResetToDefaultsPipe_, &QLineEdit::textChanged,
                this, [this](const QString& text) {
            if (text.isEmpty()) return;
            resetToDefaults();
            testResetToDefaultsPipe_->blockSignals(true);
            testResetToDefaultsPipe_->clear();
            testResetToDefaultsPipe_->blockSignals(false);
        });

        // ── Pipeline b3d7f942 phase-5 (GAP-13): Remove-action test pipe ───
        testRemoveActionPipe_ = new QLineEdit(this);
        testRemoveActionPipe_->setObjectName("testCustomizeRemoveActionPipe");
        testRemoveActionPipe_->setAccessibleName("Test Customize Remove Action Pipe");
        testRemoveActionPipe_->setFixedSize(0, 0);
        testRemoveActionPipe_->setFocusPolicy(Qt::NoFocus);
        connect(testRemoveActionPipe_, &QLineEdit::textChanged,
                this, [this](const QString& text) {
            if (text.isEmpty()) return;
            removeActionByObjectName(text);
            testRemoveActionPipe_->blockSignals(true);
            testRemoveActionPipe_->clear();
            testRemoveActionPipe_->blockSignals(false);
        });

        // ── Pipeline c91e7f3a phase-3: list-enumeration readback pipes ────
        // These bridge the Qt6/Server-2019 UIA gap where QListView rows
        // are NOT exposed as ListItem descendants.  Each pair follows the
        // trigger→result pattern: Python writes a query token to the
        // trigger pipe, the handler populates the result pipe, Python
        // reads via UIA ValuePattern.

        // Available-actions enumeration pipe
        testAvailableActionsResultPipe_ = new QLineEdit(this);
        testAvailableActionsResultPipe_->setObjectName(
            "testCustomizeAvailableActionsResultPipe");
        testAvailableActionsResultPipe_->setAccessibleName(
            "Test Customize Available Actions Result");
        testAvailableActionsResultPipe_->setFixedSize(0, 0);
        testAvailableActionsResultPipe_->setFocusPolicy(Qt::NoFocus);
        testAvailableActionsResultPipe_->setReadOnly(true);

        testAvailableActionsQueryPipe_ = new QLineEdit(this);
        testAvailableActionsQueryPipe_->setObjectName(
            "testCustomizeAvailableActionsQueryPipe");
        testAvailableActionsQueryPipe_->setAccessibleName(
            "Test Customize Available Actions Query");
        testAvailableActionsQueryPipe_->setFixedSize(0, 0);
        testAvailableActionsQueryPipe_->setFocusPolicy(Qt::NoFocus);
        connect(testAvailableActionsQueryPipe_, &QLineEdit::textChanged,
                this, [this](const QString& text) {
            if (text.isEmpty()) return;
            // Build pipe-separated list of all available-action labels.
            // Note: QLineEdit strips '\n' characters, so '|' is used as
            // the record separator.  Python splits on '|' to recover the
            // list.
            QStringList names;
            if (availableModel_) {
                for (int i = 0; i < availableModel_->rowCount(); ++i) {
                    QStandardItem* it = availableModel_->item(i);
                    if (it) names.append(it->text());
                }
            }
            testAvailableActionsResultPipe_->blockSignals(true);
            testAvailableActionsResultPipe_->setText(names.join('|'));
            testAvailableActionsResultPipe_->blockSignals(false);
            // Clear trigger so the pipe is ready for the next call.
            testAvailableActionsQueryPipe_->blockSignals(true);
            testAvailableActionsQueryPipe_->clear();
            testAvailableActionsQueryPipe_->blockSignals(false);
        });

        // Toolbar-contents enumeration pipe
        testToolbarContentsResultPipe_ = new QLineEdit(this);
        testToolbarContentsResultPipe_->setObjectName(
            "testCustomizeToolbarContentsResultPipe");
        testToolbarContentsResultPipe_->setAccessibleName(
            "Test Customize Toolbar Contents Result");
        testToolbarContentsResultPipe_->setFixedSize(0, 0);
        testToolbarContentsResultPipe_->setFocusPolicy(Qt::NoFocus);
        testToolbarContentsResultPipe_->setReadOnly(true);

        testToolbarContentsQueryPipe_ = new QLineEdit(this);
        testToolbarContentsQueryPipe_->setObjectName(
            "testCustomizeToolbarContentsQueryPipe");
        testToolbarContentsQueryPipe_->setAccessibleName(
            "Test Customize Toolbar Contents Query");
        testToolbarContentsQueryPipe_->setFixedSize(0, 0);
        testToolbarContentsQueryPipe_->setFocusPolicy(Qt::NoFocus);
        connect(testToolbarContentsQueryPipe_, &QLineEdit::textChanged,
                this, [this](const QString& text) {
            if (text.isEmpty()) return;
            // Determine which toolbar to enumerate.  The query token is
            // either "current" (meaning whichever toolbar is selected in
            // the combo) or the display-name of a specific toolbar.
            int targetIdx = currentToolbarIndex_;
            if (text.compare(QStringLiteral("current"), Qt::CaseInsensitive) != 0) {
                // Locate the toolbar by display-name.
                targetIdx = -1;
                for (int i = 0; i < static_cast<int>(layouts_.size()); ++i) {
                    if (layouts_[i].toolbarName == text) {
                        targetIdx = i;
                        break;
                    }
                }
            }
            QStringList names;
            if (targetIdx >= 0 && targetIdx < static_cast<int>(layouts_.size())) {
                // The currentList_ shows the selected toolbar's contents.
                // If this IS the current toolbar, enumerate currentModel_;
                // otherwise read directly from the layout (the model may
                // not have been populated yet for non-current toolbars).
                if (targetIdx == currentToolbarIndex_ && currentModel_) {
                    for (int i = 0; i < currentModel_->rowCount(); ++i) {
                        QStandardItem* it = currentModel_->item(i);
                        if (it) names.append(it->text());
                    }
                } else {
                    // Read action labels from the layout's actionIds via
                    // the registry.
                    for (const QString& id : layouts_[targetIdx].actionIds) {
                        if (id.isEmpty()) {
                            names.append(QStringLiteral("— Separator —"));
                            continue;
                        }
                        bool found = false;
                        for (QAction* a : registry_) {
                            if (a && a->objectName() == id) {
                                names.append(labelFor(a));
                                found = true;
                                break;
                            }
                        }
                        if (!found) names.append(id);
                    }
                }
            }
            // Use '|' as separator — QLineEdit strips '\n'.
            testToolbarContentsResultPipe_->blockSignals(true);
            testToolbarContentsResultPipe_->setText(names.join('|'));
            testToolbarContentsResultPipe_->blockSignals(false);
            // Clear trigger.
            testToolbarContentsQueryPipe_->blockSignals(true);
            testToolbarContentsQueryPipe_->clear();
            testToolbarContentsQueryPipe_->blockSignals(false);
        });

        // Widget-accessible-names enumeration pipe
        testWidgetNamesResultPipe_ = new QLineEdit(this);
        testWidgetNamesResultPipe_->setObjectName(
            "testCustomizeWidgetNamesResultPipe");
        testWidgetNamesResultPipe_->setAccessibleName(
            "Test Customize Widget Names Result");
        testWidgetNamesResultPipe_->setFixedSize(0, 0);
        testWidgetNamesResultPipe_->setFocusPolicy(Qt::NoFocus);
        testWidgetNamesResultPipe_->setReadOnly(true);

        testWidgetNamesQueryPipe_ = new QLineEdit(this);
        testWidgetNamesQueryPipe_->setObjectName(
            "testCustomizeWidgetNamesQueryPipe");
        testWidgetNamesQueryPipe_->setAccessibleName(
            "Test Customize Widget Names Query");
        testWidgetNamesQueryPipe_->setFixedSize(0, 0);
        testWidgetNamesQueryPipe_->setFocusPolicy(Qt::NoFocus);
        connect(testWidgetNamesQueryPipe_, &QLineEdit::textChanged,
                this, [this](const QString& text) {
            if (text.isEmpty()) return;
            // Return "objectName:accessibleName" lines for every
            // interactive widget.
            struct WidgetInfo { QString id; QString name; };
            const QList<WidgetInfo> widgets = {
                { toolbarCombo_->objectName(),      toolbarCombo_->accessibleName() },
                { availableList_->objectName(),     availableList_->accessibleName() },
                { currentList_->objectName(),       currentList_->accessibleName() },
                { addButton_->objectName(),         addButton_->accessibleName() },
                { removeButton_->objectName(),      removeButton_->accessibleName() },
                { addSeparatorButton_->objectName(),addSeparatorButton_->accessibleName() },
                { moveUpButton_->objectName(),      moveUpButton_->accessibleName() },
                { moveDownButton_->objectName(),    moveDownButton_->accessibleName() },
                { resetButton_->objectName(),       resetButton_->accessibleName() },
                { okButton_->objectName(),          okButton_->accessibleName() },
                { cancelButton_->objectName(),      cancelButton_->accessibleName() },
            };
            QStringList lines;
            for (const auto& wi : widgets) {
                lines.append(wi.id + QStringLiteral(":") + wi.name);
            }
            // Use '|' as separator — QLineEdit strips '\n'.
            testWidgetNamesResultPipe_->blockSignals(true);
            testWidgetNamesResultPipe_->setText(lines.join('|'));
            testWidgetNamesResultPipe_->blockSignals(false);
            // Clear trigger.
            testWidgetNamesQueryPipe_->blockSignals(true);
            testWidgetNamesQueryPipe_->clear();
            testWidgetNamesQueryPipe_->blockSignals(false);
        });
    } // end if (pdfe::ui::testseam::testPipesEnabled())
}

void CustomizeToolbarsDialog::selectToolbar(int index) {
    if (index < 0 || index >= static_cast<int>(layouts_.size())) return;
    currentToolbarIndex_ = index;
    if (toolbarCombo_->currentIndex() != index) {
        toolbarCombo_->setCurrentIndex(index);
    }
    rebuildAvailableList();
    rebuildCurrentList();
}

void CustomizeToolbarsDialog::onToolbarSelected(int index) {
    persistCurrentSelectionBack();
    selectToolbar(index);
}

void CustomizeToolbarsDialog::rebuildAvailableList() {
    availableModel_->clear();
    for (QAction* a : registry_) {
        if (!a) continue;
        availableModel_->appendRow(makeActionItem(a));
    }
    availableModel_->sort(0);
}

void CustomizeToolbarsDialog::rebuildCurrentList() {
    currentModel_->clear();
    if (currentToolbarIndex_ < 0
        || currentToolbarIndex_ >= static_cast<int>(layouts_.size())) return;
    const auto& layout = layouts_[currentToolbarIndex_];
    for (const QString& id : layout.actionIds) {
        if (id.isEmpty()) {
            currentModel_->appendRow(makeSeparatorItem());
            continue;
        }
        QAction* match = nullptr;
        for (QAction* a : registry_) {
            if (a && a->objectName() == id) { match = a; break; }
        }
        if (match) {
            currentModel_->appendRow(makeActionItem(match));
        } else {
            // Unknown id — preserve in list as a placeholder so it
            // round-trips on cancel.
            auto* it = new QStandardItem(QString("? %1").arg(id));
            it->setData(id, Qt::UserRole);
            it->setEditable(false);
            currentModel_->appendRow(it);
        }
    }
}

void CustomizeToolbarsDialog::persistCurrentSelectionBack() {
    if (currentToolbarIndex_ < 0
        || currentToolbarIndex_ >= static_cast<int>(layouts_.size())) return;
    QStringList ids;
    for (int i = 0; i < currentModel_->rowCount(); ++i) {
        ids.push_back(currentModel_->item(i)->data(Qt::UserRole).toString());
    }
    layouts_[currentToolbarIndex_].actionIds = ids;
}

void CustomizeToolbarsDialog::onAddClicked() {
    QModelIndex selIdx = availableList_->currentIndex();
    if (!selIdx.isValid()) return;
    QStandardItem* sel = availableModel_->itemFromIndex(selIdx);
    if (!sel) return;
    auto* cloned = cloneItem(sel);
    int insertAt = currentList_->currentIndex().row();
    if (insertAt < 0) insertAt = currentModel_->rowCount();
    else insertAt += 1;
    currentModel_->insertRow(insertAt, cloned);
    currentList_->setCurrentIndex(currentModel_->index(insertAt, 0));
    persistCurrentSelectionBack();
}

void CustomizeToolbarsDialog::onRemoveClicked() {
    int row = currentList_->currentIndex().row();
    if (row < 0) return;
    currentModel_->removeRow(row);
    persistCurrentSelectionBack();
}

void CustomizeToolbarsDialog::onMoveUpClicked() {
    int row = currentList_->currentIndex().row();
    if (row <= 0) return;
    QList<QStandardItem*> taken = currentModel_->takeRow(row);
    currentModel_->insertRow(row - 1, taken);
    currentList_->setCurrentIndex(currentModel_->index(row - 1, 0));
    persistCurrentSelectionBack();
}

void CustomizeToolbarsDialog::onMoveDownClicked() {
    int row = currentList_->currentIndex().row();
    if (row < 0 || row >= currentModel_->rowCount() - 1) return;
    QList<QStandardItem*> taken = currentModel_->takeRow(row);
    currentModel_->insertRow(row + 1, taken);
    currentList_->setCurrentIndex(currentModel_->index(row + 1, 0));
    persistCurrentSelectionBack();
}

void CustomizeToolbarsDialog::onAddSeparatorClicked() {
    int insertAt = currentList_->currentIndex().row();
    if (insertAt < 0) insertAt = currentModel_->rowCount();
    else insertAt += 1;
    currentModel_->insertRow(insertAt, makeSeparatorItem());
    currentList_->setCurrentIndex(currentModel_->index(insertAt, 0));
    persistCurrentSelectionBack();
}

void CustomizeToolbarsDialog::onResetClicked() {
    layouts_ = ToolbarLayoutManager::defaultLayouts();
    toolbarCombo_->blockSignals(true);
    toolbarCombo_->clear();
    for (const auto& l : layouts_) toolbarCombo_->addItem(l.toolbarName);
    toolbarCombo_->blockSignals(false);
    selectToolbar(0);
}

std::vector<ToolbarLayout> CustomizeToolbarsDialog::updatedLayouts() const {
    return layouts_;
}

// ─── Pipeline b3f19d72 phase-1 test-automation hooks ─────────────────────

bool CustomizeToolbarsDialog::removeActionByObjectName(const QString& objectName) {
    if (objectName.isEmpty()) return false;
    if (!currentModel_) return false;

    // Locate the Current-list row whose UserRole data (objectName) matches.
    int matchRow = -1;
    for (int i = 0; i < currentModel_->rowCount(); ++i) {
        QStandardItem* it = currentModel_->item(i);
        if (it && it->data(Qt::UserRole).toString() == objectName) {
            matchRow = i;
            break;
        }
    }
    if (matchRow < 0) return false;

    currentList_->setCurrentIndex(currentModel_->index(matchRow, 0));
    // Route through the exact button-click slot so any future logic
    // changes stay in one place.
    onRemoveClicked();
    return true;
}

bool CustomizeToolbarsDialog::addActionByObjectName(const QString& objectName) {
    if (objectName.isEmpty()) return false;
    if (!availableModel_) return false;

    // Locate the Available-list row whose item data (objectName, set
    // by makeActionItem) matches. We rely on data lookup rather than
    // the visible label because labels are localized.
    int matchRow = -1;
    for (int i = 0; i < availableModel_->rowCount(); ++i) {
        QStandardItem* it = availableModel_->item(i);
        if (it && it->data(Qt::UserRole).toString() == objectName) {
            matchRow = i;
            break;
        }
    }
    if (matchRow < 0) return false;

    availableList_->setCurrentIndex(availableModel_->index(matchRow, 0));
    // Route through the exact button-click slot so any future logic
    // changes (insert position, signals, etc.) stay in one place.
    onAddClicked();
    return true;
}

void CustomizeToolbarsDialog::resetToDefaults() {
    onResetClicked();
}

} // namespace pdfe::ui
