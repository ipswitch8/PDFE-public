#pragma once

#include <QDialog>
#include <vector>

class QListWidget;
class QPushButton;

namespace pdfe::forms {
class FormManager;
struct FormFieldData;
}

namespace pdfe::core {
class Page;
}

namespace pdfe::ui {

class TabOrderDialog : public QDialog {
    Q_OBJECT

public:
    explicit TabOrderDialog(pdfe::forms::FormManager* formManager,
                             pdfe::core::Page* page,
                             QWidget* parent = nullptr);
    ~TabOrderDialog() override;

    std::vector<int> newTabOrder() const;

    /// Pipeline a7c4f891 phase-1 (A3): programmatic reorder seam for
    /// tests. Moves the item at `fromIndex` to `toIndex` in the tab-
    /// order list, mirroring the effect of multiple Move Up/Down
    /// button clicks. Bypasses the UIA button-click route while still
    /// exercising the underlying list-mutation path. Returns true if
    /// both indices are in range and the move was applied; false
    /// otherwise. Pattern mirrors CalculationOrderDialog::
    /// moveProgrammatic (added in pipeline 3e9b1d7c phase-3).
    Q_INVOKABLE bool moveProgrammatic(int fromIndex, int toIndex);

private slots:
    void onMoveUp();
    void onMoveDown();
    void onApply();

private:
    void populateList();

    pdfe::forms::FormManager* formManager_;
    pdfe::core::Page* page_;

    QListWidget* fieldList_ = nullptr;
    QPushButton* moveUpButton_ = nullptr;
    QPushButton* moveDownButton_ = nullptr;
    QPushButton* applyButton_ = nullptr;
    QPushButton* cancelButton_ = nullptr;

    std::vector<int> widgetIndices_;
};

} // namespace pdfe::ui
