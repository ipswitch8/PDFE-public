#pragma once

#include <QDialog>
#include <memory>
#include <string>

namespace pdfe::core {
class Document;
class Page;
}
namespace pdfe::annotations {
class AnnotationManager;
}

class QLineEdit;
class QTextEdit;
class QPushButton;
class QLabel;

namespace pdfe::ui {

/// Editor for the PDF accessibility fields /Alt (short alt text) and
/// /ActualText (replacement text) on a single annotation.
///
/// Opened from the annotation context menu (typically for /Figure
/// shapes), or from a future menu action on a tagged structure element.
/// Changes persist through save/reload because AnnotationManager writes
/// them directly into the annotation's pdf_obj. Phase-8c-core already
/// shipped the data-layer set/get; this dialog is the UI surface.
class AltTextDialog : public QDialog {
    Q_OBJECT

public:
    AltTextDialog(pdfe::annotations::AnnotationManager* mgr,
                  pdfe::core::Page* page,
                  int annotIndex,
                  QWidget* parent = nullptr);
    ~AltTextDialog() override;

signals:
    /// Fired after the user clicks Save and the write succeeds. Allows
    /// the main window to refresh panels that display the annotation.
    void saved(int annotIndex);

private slots:
    void onSave();

private:
    void setupUi();
    void loadFromAnnotation();

    pdfe::annotations::AnnotationManager* mgr_ = nullptr;
    pdfe::core::Page* page_ = nullptr;
    int annotIndex_ = -1;

    QLineEdit* altEdit_         = nullptr;
    QTextEdit* actualEdit_      = nullptr;
    QPushButton* saveButton_    = nullptr;
    QPushButton* cancelButton_  = nullptr;
    QLabel*      statusLabel_   = nullptr;
};

} // namespace pdfe::ui
