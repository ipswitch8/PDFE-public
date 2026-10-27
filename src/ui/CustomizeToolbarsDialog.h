#pragma once

#include "ToolbarLayoutManager.h"

#include <QDialog>

#include <vector>

class QListView;
class QStandardItemModel;
class QStandardItem;
class QComboBox;
class QLineEdit;
class QPushButton;
class QAction;

namespace pdfe::ui {

/// Pipeline E phase-10: Customize Toolbars dialog. The user picks a
/// toolbar from the dropdown, then uses Add / Remove / Move Up /
/// Move Down / Add Separator buttons to move actions between the
/// "Available" list (full registry) and the "Current" list (this
/// toolbar's items). "Reset to Default" restores factory layout.
/// On accept(), updatedLayouts() returns the new full set; the caller
/// is responsible for applying them to live QToolBars and persisting
/// via ToolbarLayoutManager.
class CustomizeToolbarsDialog : public QDialog {
    Q_OBJECT

public:
    /// @param actionRegistry  Every QAction the user can add to a
    ///                        toolbar. Each must have a stable
    ///                        objectName.
    /// @param initialLayouts  Current toolbar contents to seed the
    ///                        dialog from.
    CustomizeToolbarsDialog(std::vector<QAction*> actionRegistry,
                            std::vector<ToolbarLayout> initialLayouts,
                            QWidget* parent = nullptr);

    /// After accept(), returns the user-edited layouts. After reject(),
    /// returns the layouts unchanged.
    std::vector<ToolbarLayout> updatedLayouts() const;

    // ─── Pipeline b3f19d72 phase-1 test-automation hooks ────────────────
    //
    // These route through the same slots the UI buttons fire, so
    // production logic is exercised by tests without duplicating it.

    /// Add the action whose objectName matches ``objectName`` to the
    /// currently-selected toolbar (selects it in the Available list
    /// first, then runs the same code path as the Add button click).
    /// Returns true on success, false when no matching action is in
    /// the registry. Lets pywinauto / WinAppDriver scripts compose a
    /// toolbar without driving the two-list selection by clicks.
    Q_INVOKABLE bool addActionByObjectName(const QString& objectName);

    /// Remove the first action whose objectName matches ``objectName``
    /// from the currently-selected toolbar.  Routes through the same
    /// code path as the Remove button click.  Returns true on success,
    /// false when no matching action is in the current toolbar.
    Q_INVOKABLE bool removeActionByObjectName(const QString& objectName);

    /// Reset all toolbar layouts back to factory defaults. Routes
    /// through @ref onResetClicked so the registry rebuild, combo
    /// repopulate and list refresh use the same code path as the
    /// "Reset to Default" button.
    Q_INVOKABLE void resetToDefaults();

private slots:
    void onToolbarSelected(int index);
    void onAddClicked();
    void onRemoveClicked();
    void onMoveUpClicked();
    void onMoveDownClicked();
    void onAddSeparatorClicked();
    void onResetClicked();

private:
    void setupUi();
    void rebuildAvailableList();
    void rebuildCurrentList();
    void persistCurrentSelectionBack();
    void selectToolbar(int index);

    std::vector<QAction*> registry_;
    std::vector<ToolbarLayout> layouts_;
    int currentToolbarIndex_ = 0;

    QComboBox* toolbarCombo_ = nullptr;
    QListView* availableList_ = nullptr;
    QStandardItemModel* availableModel_ = nullptr;
    QListView* currentList_ = nullptr;
    QStandardItemModel* currentModel_ = nullptr;
    QPushButton* addButton_ = nullptr;
    QPushButton* removeButton_ = nullptr;
    QPushButton* moveUpButton_ = nullptr;
    QPushButton* moveDownButton_ = nullptr;
    QPushButton* addSeparatorButton_ = nullptr;
    QPushButton* resetButton_ = nullptr;
    QPushButton* okButton_ = nullptr;
    QPushButton* cancelButton_ = nullptr;

    // Pipeline b3f19d72 phase-14 / GAP-18: test-seam pipes (hidden 0x0 QLineEdits).
    // ALL pipes gated by PDFE_TEST_PIPES — never created in production.
    // Pywinauto finds them via UIA AutomationId matching objectName.
    QLineEdit* testAddActionPipe_ = nullptr;       // objectName = "testCustomizeAddActionPipe"
    QLineEdit* testRemoveActionPipe_ = nullptr;    // objectName = "testCustomizeRemoveActionPipe"
    QLineEdit* testResetToDefaultsPipe_ = nullptr; // objectName = "testCustomizeResetPipe"

    // Pipeline c91e7f3a phase-3: list-enumeration readback pipes.
    // These bridge the Qt6/Server-2019 UIA gap where QListView rows are
    // not exposed as ListItem descendants.  Python writes a query token
    // to the trigger pipe; the handler writes a newline-separated list
    // of item labels to the corresponding result pipe; Python reads via
    // UIA ValuePattern.
    //
    // testAvailableActionsQueryPipe_  (objectName = "testCustomizeAvailableActionsQueryPipe")
    //   Trigger: any non-empty string → result: newline-joined available action labels
    //
    // testAvailableActionsResultPipe_ (objectName = "testCustomizeAvailableActionsResultPipe")
    //   Read-only result sink.
    //
    // testToolbarContentsQueryPipe_   (objectName = "testCustomizeToolbarContentsQueryPipe")
    //   Trigger: toolbar display-name (or "current") → result: newline-joined current
    //   toolbar content labels for the queried toolbar
    //
    // testToolbarContentsResultPipe_  (objectName = "testCustomizeToolbarContentsResultPipe")
    //   Read-only result sink.
    //
    // testWidgetNamesQueryPipe_       (objectName = "testCustomizeWidgetNamesQueryPipe")
    //   Trigger: any non-empty string → result: newline-joined "auto_id:accessibleName"
    //   pairs for the dialog's interactive widgets (for accessibility verification)
    //
    // testWidgetNamesResultPipe_      (objectName = "testCustomizeWidgetNamesResultPipe")
    //   Read-only result sink.
    QLineEdit* testAvailableActionsQueryPipe_  = nullptr;
    QLineEdit* testAvailableActionsResultPipe_ = nullptr;
    QLineEdit* testToolbarContentsQueryPipe_   = nullptr;
    QLineEdit* testToolbarContentsResultPipe_  = nullptr;
    QLineEdit* testWidgetNamesQueryPipe_       = nullptr;
    QLineEdit* testWidgetNamesResultPipe_      = nullptr;
};

} // namespace pdfe::ui
