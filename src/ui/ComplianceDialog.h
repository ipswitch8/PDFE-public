#pragma once

#include <QDialog>
#include <memory>

namespace pdfe::core {
class MuPdfContext;
class Document;
}

namespace pdfe::compliance {
enum class PdfALevel;
struct ComplianceResult;
struct StructureNode;
}

class QComboBox;
class QPushButton;
class QTableWidget;
class QTreeWidget;
class QTreeWidgetItem;
class QLabel;
class QProgressBar;

namespace pdfe::ui {

class ComplianceDialog : public QDialog {
    Q_OBJECT

public:
    explicit ComplianceDialog(
        std::shared_ptr<pdfe::core::MuPdfContext> context,
        pdfe::core::Document* document,
        QWidget* parent = nullptr
    );
    ~ComplianceDialog() override;

signals:
    void conversionCompleted(const QString& outputPath);

private slots:
    void onAnalyze();
    void onValidate();
    void onConvert();
    void onPreviewTags();

private:
    void setupUi();
    void populateIssueTable(const pdfe::compliance::ComplianceResult& result);
    void populateTagTree(const pdfe::compliance::StructureNode& root);
    void addTreeNode(QTreeWidgetItem* parent, const pdfe::compliance::StructureNode& node);
    void setConvertingState(bool converting);
    pdfe::compliance::PdfALevel selectedLevel() const;

    std::shared_ptr<pdfe::core::MuPdfContext> context_;
    pdfe::core::Document* document_;

    QComboBox* levelCombo_;
    QPushButton* analyzeButton_;
    QPushButton* validateButton_;
    QPushButton* convertButton_;
    QPushButton* previewTagsButton_;
    QTableWidget* issueTable_;
    QTreeWidget* tagStructureTree_;
    QProgressBar* progressBar_;
    QLabel* statusLabel_;
};

} // namespace pdfe::ui
