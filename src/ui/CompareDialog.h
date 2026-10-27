#pragma once

#include <QDialog>
#include <memory>
#include <filesystem>

namespace pdfe::core {
class MuPdfContext;
class Document;
}

namespace pdfe::diff {
class DiffEngine;
struct DiffResult;
}

class QPushButton;
class QLabel;
class QScrollArea;
class QSpinBox;
class QTextEdit;
class QSplitter;
class QComboBox;

namespace pdfe::ui {

class CompareDialog : public QDialog {
    Q_OBJECT

public:
    explicit CompareDialog(
        std::shared_ptr<pdfe::core::MuPdfContext> context,
        QWidget* parent = nullptr
    );
    ~CompareDialog() override;

    // Pipeline E phase-8: programmatic API used by tests and by callers
    // that already have docs loaded. Signatures use only int + Document*
    // to keep the header ABI lean (avoid forcing transitive includes).
    int compareLoadedDocuments(pdfe::core::Document* docA,
                               pdfe::core::Document* docB);
    int totalChangeRegions() const;
    int changedPageCount() const;
    int currentPageIndex() const;
    int gotoNextChangedPage();
    int gotoPrevChangedPage();

private slots:
    void onSelectFileA();
    void onSelectFileB();
    void onCompare();
    void onPageChanged(int page);
    void onNextChangeButton();
    void onPrevChangeButton();

private:
    void setupUi();
    void displayPage(int pageIndex);

    std::shared_ptr<pdfe::core::MuPdfContext> context_;
    std::unique_ptr<pdfe::core::Document> docA_;
    std::unique_ptr<pdfe::core::Document> docB_;
    std::unique_ptr<pdfe::diff::DiffResult> diffResult_;

    QPushButton* selectFileAButton_;
    QPushButton* selectFileBButton_;
    QLabel* fileALabel_;
    QLabel* fileBLabel_;
    QPushButton* compareButton_;

    QLabel* imageALabel_;
    QLabel* imageBLabel_;
    QLabel* diffOverlayLabel_;
    QScrollArea* scrollAreaA_;
    QScrollArea* scrollAreaB_;

    QSpinBox* pageSpinBox_;
    QLabel* pageCountLabel_;
    QPushButton* prevPageButton_;
    QPushButton* nextPageButton_;
    QPushButton* prevChangeButton_ = nullptr;
    QPushButton* nextChangeButton_ = nullptr;

    QLabel* similarityLabel_;
    QLabel* changeCountLabel_;
    QTextEdit* textDiffView_;
    QComboBox* diffColorCombo_;
};

} // namespace pdfe::ui
