#pragma once

#include <QDialog>
#include <filesystem>
#include <memory>

namespace pdfe::core {
class MuPdfContext;
class Document;
}

class QCheckBox;
class QLabel;
class QPushButton;

namespace pdfe::ui {

/// Dialog for optimizing a PDF (stream compression, image re-compression,
/// font subsetting, unused-object removal, duplicate-object merging).
/// Shows before/after size + per-category savings.
class OptimizeDialog : public QDialog {
    Q_OBJECT

public:
    explicit OptimizeDialog(
        std::shared_ptr<pdfe::core::MuPdfContext> context,
        pdfe::core::Document* document,
        QWidget* parent = nullptr);
    ~OptimizeDialog() override;

signals:
    void optimized(const QString& outputPath);

private slots:
    void onRun();

private:
    void setupUi();

    std::shared_ptr<pdfe::core::MuPdfContext> context_;
    pdfe::core::Document* document_;

    QCheckBox* compressStreamsBox_  = nullptr;
    QCheckBox* recompressImagesBox_ = nullptr;
    QCheckBox* subsetFontsBox_      = nullptr;
    QCheckBox* removeUnusedBox_     = nullptr;
    QCheckBox* mergeDuplicatesBox_  = nullptr;
    QCheckBox* sanitizeBox_         = nullptr;
    QCheckBox* linearizeBox_        = nullptr;

    QPushButton* runButton_    = nullptr;
    QPushButton* cancelButton_ = nullptr;
    QLabel*      resultLabel_  = nullptr;
};

} // namespace pdfe::ui
