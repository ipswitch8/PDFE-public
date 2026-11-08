#pragma once

#include <QDialog>
#include <QColor>
#include <QImage>
#include <QSize>
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

    // ---- Testing seams (headless gtest drives the real widgets) ----
    // GitHub #351: the highlight colour actually in effect (driven by the
    // combo), so a test can prove selecting a colour changed the stored one.
    QColor currentDiffColorForTesting() const { return diffColor_; }
    // Simulate the user picking a colour in the combo. Sets the combo's
    // current index to the item carrying this colour, exercising the real
    // currentIndexChanged wiring (not a back-door setter).
    bool selectDiffColorForTesting(const QColor& color);
    // GitHub #352: the overlay image AS COMPOSITED over page A at the last
    // displayPage() — i.e. after any scaling to the display render size —
    // plus the display-space page size and the page index it was built for.
    // A regression that blits the raw (engine-DPI) overlay 1:1 again will
    // make these two sizes disagree.
    QImage lastComposedOverlayForTesting() const { return lastOverlay_; }
    QSize lastDisplayedPageSizeForTesting() const { return lastPageImageSize_; }
    int lastDisplayedPageIndexForTesting() const { return lastDisplayedPageIndex_; }
    // The engine render scale used by the most recent diff (renderDpi/72).
    float lastEngineRenderScaleForTesting() const { return lastEngineScale_; }
    float displayScaleForTesting() const { return displayScale_; }
    const pdfe::diff::DiffResult* diffResultForTesting() const { return diffResult_.get(); }

private slots:
    void onSelectFileA();
    void onSelectFileB();
    void onCompare();
    void onPageChanged(int page);
    void onNextChangeButton();
    void onPrevChangeButton();
    // GitHub #351: react to a highlight-colour selection in the combo.
    void onDiffColorChanged(int index);

private:
    void setupUi();
    void displayPage(int pageIndex);
    // Shared diff driver used by onCompare(), compareLoadedDocuments() and
    // the colour re-run path, so the chosen colour and render scale are
    // applied consistently from one place.
    int runDiff(pdfe::core::Document* docA, pdfe::core::Document* docB);
    // GitHub #352: scale the engine-DPI overlay to the display render size.
    QImage overlayForDisplay(const QImage& overlay, const QSize& target) const;

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

    // GitHub #351: the highlight colour currently in effect. Kept in sync
    // with diffColorCombo_ and fed to the engine on every diff run.
    QColor diffColor_ = QColor(255, 0, 0);

    // Raw pointers to the documents used by the most recent diff. displayPage
    // and the colour re-run path render from these so both the file-picker
    // flow (docA_/docB_) and the compareLoadedDocuments() flow work.
    pdfe::core::Document* lastDocA_ = nullptr;
    pdfe::core::Document* lastDocB_ = nullptr;

    // GitHub #352: render scale the last diff ran at (renderDpi/72), and the
    // display render scale. The overlay is scaled by displayScale_/engineScale
    // so it registers to the same page-space region as the diffed content.
    float lastEngineScale_ = 150.0f / 72.0f;
    float displayScale_ = 1.0f;

    // Observability for tests: what displayPage last composited over page A.
    QImage lastOverlay_;
    QSize lastPageImageSize_;
    int lastDisplayedPageIndex_ = -1;
};

} // namespace pdfe::ui
