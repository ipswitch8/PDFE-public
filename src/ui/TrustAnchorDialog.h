// -*- coding: utf-8 -*-
// Pipeline F phase-10: UI for managing the persistent trust anchor list.
#pragma once

#include <QDialog>

class QPushButton;
class QTableWidget;

namespace pdfe::ui {

/**
 * @brief Modal dialog listing the user's trusted root certs.
 *
 * Bound to TrustAnchorManager singleton. Add opens a file picker
 * (PEM/DER); Remove drops the currently selected row.
 *
 * Object names exposed for UIA / pywinauto:
 *   - dialog            : "trustAnchorDialog"
 *   - table             : "trustAnchorTable"
 *   - Add button        : "addTrustButton"
 *   - Remove button     : "removeTrustButton"
 *   - Close button      : "closeTrustButton"
 */
class TrustAnchorDialog : public QDialog {
    Q_OBJECT
public:
    explicit TrustAnchorDialog(QWidget* parent = nullptr);
    ~TrustAnchorDialog() override;

    /// Re-read TrustAnchorManager and repopulate the table. Public so
    /// tests can poke the dialog after manipulating the singleton.
    void refresh();

    /// Pipeline f3c91a2e phase-7 (GAP-A14): the constructor records a
    /// pipe-delimited inventory of the dialog's real child widgets +
    /// row count here when PDFE_TEST_PIPES is set, so a pywinauto test
    /// can assert the dialog opened correctly via MainWindow's result
    /// pipe — even though UIA cannot enumerate the modal dialog on
    /// Windows Server 2019. Format:
    ///   "trustAnchorDialog|table=1|add=1|remove=1|close=1|rows=<n>".
    /// Empty until the dialog has been constructed at least once.
    static QString lastTestIntrospection();

private slots:
    void onAddClicked();
    void onRemoveClicked();
    void onSelectionChanged();

private:
    QTableWidget* table_ = nullptr;
    QPushButton* addButton_ = nullptr;
    QPushButton* removeButton_ = nullptr;
    QPushButton* closeButton_ = nullptr;
};

} // namespace pdfe::ui
