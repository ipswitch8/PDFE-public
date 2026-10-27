#pragma once

#include <QDialog>
#include <vector>
#include <string>

class QListWidget;
class QPushButton;

namespace pdfe::core { class Document; }
namespace pdfe::forms { class FormManager; }

namespace pdfe::ui {

/// Editor for /AcroForm /CO — the form-wide calculation order.
/// Parallels TabOrderDialog but operates on field *names* at the
/// document level rather than widget *indices* at the page level.
/// Fields not currently in the calc order can be added via the "Add"
/// button; unchecked entries are omitted from the new /CO array.
class CalculationOrderDialog : public QDialog {
    Q_OBJECT
public:
    CalculationOrderDialog(pdfe::forms::FormManager* formManager,
                            pdfe::core::Document* doc,
                            QWidget* parent = nullptr);
    ~CalculationOrderDialog() override;

    /// Proposed calculation-order list (order of checked rows).
    std::vector<std::string> newCalculationOrder() const;

    /// Pipeline 3e9b1d7c phase-3 (C3 / P-GAP-95): programmatic
    /// reorder seam for tests. Moves the item at `fromIndex` to
    /// `toIndex` in the calculation-order list, mirroring the
    /// effect of multiple Move Up/Down button clicks. Bypasses
    /// the UIA button-click route while still exercising the
    /// underlying list-mutation path. Returns true if both indices
    /// are in range and the move was applied; false otherwise.
    Q_INVOKABLE bool moveProgrammatic(int fromIndex, int toIndex);

private slots:
    void onMoveUp();
    void onMoveDown();
    void onApply();

private:
    void populateList();

    pdfe::forms::FormManager* formManager_;
    pdfe::core::Document* doc_;

    QListWidget* fieldList_ = nullptr;
    QPushButton* moveUpButton_ = nullptr;
    QPushButton* moveDownButton_ = nullptr;
    QPushButton* applyButton_ = nullptr;
    QPushButton* cancelButton_ = nullptr;
};

} // namespace pdfe::ui
