#include "CompareDialog.h"
#include "diff/DiffEngine.h"
#include "diff/DiffTypes.h"
#include "core/MuPdfContext.h"
#include "core/Document.h"
#include "core/Page.h"

#include <QPushButton>
#include <QLabel>
#include <QScrollArea>
#include <QSpinBox>
#include <QTextEdit>
#include <QSplitter>
#include <QComboBox>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QApplication>
#include <QScrollBar>
#include <QPainter>
#include "ui/TestDialogSeam.h"

namespace pdfe::ui {

CompareDialog::CompareDialog(
    std::shared_ptr<pdfe::core::MuPdfContext> context,
    QWidget* parent)
    : QDialog(parent)
    , context_(std::move(context))
{
    setupUi();

    // Register with the dialog-introspection seam so tests can observe
    // this dialog while its modal exec() loop runs (UIA cannot enumerate
    // it). MUST stay last: the seam snapshots the child inventory.
    pdfe::ui::testseam::registerDialogIntrospection(this);
}

CompareDialog::~CompareDialog() = default;

void CompareDialog::setupUi() {
    setWindowTitle(tr("Compare Documents"));
    setMinimumSize(900, 650);
    setObjectName("compareDialog");
    setAccessibleName("Compare Documents");

    auto* mainLayout = new QVBoxLayout(this);

    // File selection
    auto* fileGroup = new QGroupBox(tr("Documents"), this);
    fileGroup->setObjectName("compareFileGroup");
    auto* fileLayout = new QHBoxLayout(fileGroup);

    selectFileAButton_ = new QPushButton(tr("Select File A..."), fileGroup);
    selectFileAButton_->setObjectName("selectFileAButton");
    selectFileAButton_->setAccessibleName("Select First Document");
    fileALabel_ = new QLabel(tr("No file selected"), fileGroup);
    fileALabel_->setObjectName("fileALabel");

    selectFileBButton_ = new QPushButton(tr("Select File B..."), fileGroup);
    selectFileBButton_->setObjectName("selectFileBButton");
    selectFileBButton_->setAccessibleName("Select Second Document");
    fileBLabel_ = new QLabel(tr("No file selected"), fileGroup);
    fileBLabel_->setObjectName("fileBLabel");

    compareButton_ = new QPushButton(tr("Compare"), fileGroup);
    compareButton_->setObjectName("compareButton");
    compareButton_->setAccessibleName("Compare Documents");
    compareButton_->setEnabled(false);

    fileLayout->addWidget(selectFileAButton_);
    fileLayout->addWidget(fileALabel_, 1);
    fileLayout->addWidget(selectFileBButton_);
    fileLayout->addWidget(fileBLabel_, 1);
    fileLayout->addWidget(compareButton_);
    mainLayout->addWidget(fileGroup);

    // Navigation
    auto* navLayout = new QHBoxLayout();
    prevPageButton_ = new QPushButton(tr("<"), this);
    prevPageButton_->setObjectName("prevPageButton");
    prevPageButton_->setAccessibleName("Previous Page");
    prevPageButton_->setFixedWidth(30);
    pageSpinBox_ = new QSpinBox(this);
    pageSpinBox_->setObjectName("pageSpinBox");
    pageSpinBox_->setAccessibleName("Page Number");
    pageSpinBox_->setMinimum(1);
    pageSpinBox_->setMaximum(1);
    nextPageButton_ = new QPushButton(tr(">"), this);
    nextPageButton_->setObjectName("nextPageButton");
    nextPageButton_->setAccessibleName("Next Page");
    nextPageButton_->setFixedWidth(30);
    pageCountLabel_ = new QLabel(tr("/ 0"), this);
    pageCountLabel_->setObjectName("pageCountLabel");

    prevChangeButton_ = new QPushButton(tr("Prev Change"), this);
    prevChangeButton_->setObjectName("prevChangeButton");
    prevChangeButton_->setAccessibleName("Previous Change");
    nextChangeButton_ = new QPushButton(tr("Next Change"), this);
    nextChangeButton_->setObjectName("nextChangeButton");
    nextChangeButton_->setAccessibleName("Next Change");

    diffColorCombo_ = new QComboBox(this);
    diffColorCombo_->setObjectName("diffColorCombo");
    diffColorCombo_->setAccessibleName("Diff Highlight Color");
    diffColorCombo_->addItem(tr("Red"), QColor(255, 0, 0));
    diffColorCombo_->addItem(tr("Blue"), QColor(0, 0, 255));
    diffColorCombo_->addItem(tr("Green"), QColor(0, 200, 0));
    diffColorCombo_->addItem(tr("Orange"), QColor(255, 165, 0));

    similarityLabel_ = new QLabel(this);
    similarityLabel_->setObjectName("similarityLabel");
    similarityLabel_->setAccessibleName("Similarity Score");
    changeCountLabel_ = new QLabel(this);
    changeCountLabel_->setObjectName("changeCountLabel");
    changeCountLabel_->setAccessibleName("Change Count");

    navLayout->addWidget(prevPageButton_);
    navLayout->addWidget(pageSpinBox_);
    navLayout->addWidget(pageCountLabel_);
    navLayout->addWidget(nextPageButton_);
    navLayout->addSpacing(16);
    navLayout->addWidget(prevChangeButton_);
    navLayout->addWidget(nextChangeButton_);
    navLayout->addStretch();
    navLayout->addWidget(new QLabel(tr("Highlight:"), this));
    navLayout->addWidget(diffColorCombo_);
    navLayout->addStretch();
    navLayout->addWidget(similarityLabel_);
    navLayout->addWidget(changeCountLabel_);
    mainLayout->addLayout(navLayout);

    // Side-by-side viewer
    auto* splitter = new QSplitter(Qt::Horizontal, this);
    splitter->setObjectName("compareSplitter");

    scrollAreaA_ = new QScrollArea(splitter);
    scrollAreaA_->setObjectName("scrollAreaA");
    scrollAreaA_->setAccessibleName("Document A View");
    scrollAreaA_->setWidgetResizable(false);
    imageALabel_ = new QLabel();
    imageALabel_->setObjectName("imageALabel");
    scrollAreaA_->setWidget(imageALabel_);

    scrollAreaB_ = new QScrollArea(splitter);
    scrollAreaB_->setObjectName("scrollAreaB");
    scrollAreaB_->setAccessibleName("Document B View");
    scrollAreaB_->setWidgetResizable(false);
    imageBLabel_ = new QLabel();
    imageBLabel_->setObjectName("imageBLabel");
    scrollAreaB_->setWidget(imageBLabel_);

    splitter->addWidget(scrollAreaA_);
    splitter->addWidget(scrollAreaB_);
    mainLayout->addWidget(splitter, 1);

    // Synchronized scrolling
    connect(scrollAreaA_->verticalScrollBar(), &QScrollBar::valueChanged,
            scrollAreaB_->verticalScrollBar(), &QScrollBar::setValue);
    connect(scrollAreaB_->verticalScrollBar(), &QScrollBar::valueChanged,
            scrollAreaA_->verticalScrollBar(), &QScrollBar::setValue);
    connect(scrollAreaA_->horizontalScrollBar(), &QScrollBar::valueChanged,
            scrollAreaB_->horizontalScrollBar(), &QScrollBar::setValue);
    connect(scrollAreaB_->horizontalScrollBar(), &QScrollBar::valueChanged,
            scrollAreaA_->horizontalScrollBar(), &QScrollBar::setValue);

    // Text diff view
    textDiffView_ = new QTextEdit(this);
    textDiffView_->setObjectName("textDiffView");
    textDiffView_->setAccessibleName("Text Differences");
    textDiffView_->setReadOnly(true);
    textDiffView_->setMaximumHeight(150);
    mainLayout->addWidget(textDiffView_);

    // Connections
    connect(selectFileAButton_, &QPushButton::clicked, this, &CompareDialog::onSelectFileA);
    connect(selectFileBButton_, &QPushButton::clicked, this, &CompareDialog::onSelectFileB);
    connect(compareButton_, &QPushButton::clicked, this, &CompareDialog::onCompare);
    connect(pageSpinBox_, QOverload<int>::of(&QSpinBox::valueChanged), this, &CompareDialog::onPageChanged);
    connect(prevPageButton_, &QPushButton::clicked, this, [this]() {
        pageSpinBox_->setValue(pageSpinBox_->value() - 1);
    });
    connect(nextPageButton_, &QPushButton::clicked, this, [this]() {
        pageSpinBox_->setValue(pageSpinBox_->value() + 1);
    });
    connect(prevChangeButton_, &QPushButton::clicked,
            this, &CompareDialog::onPrevChangeButton);
    connect(nextChangeButton_, &QPushButton::clicked,
            this, &CompareDialog::onNextChangeButton);

    // GitHub #351: the combo was never connected, so picking a colour did
    // nothing — the diff was always drawn in the default red. Seed the
    // effective colour from the combo's current item and wire the signal so
    // a selection actually drives the highlight colour.
    diffColor_ = diffColorCombo_->currentData().value<QColor>();
    if (!diffColor_.isValid()) diffColor_ = QColor(255, 0, 0);
    connect(diffColorCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &CompareDialog::onDiffColorChanged);
}

void CompareDialog::onDiffColorChanged(int /*index*/) {
    // GitHub #351: update the effective colour from the selection and, if a
    // diff is already on screen, re-run it so the highlight recolours
    // immediately rather than remaining stuck on the previous colour.
    QColor picked = diffColorCombo_->currentData().value<QColor>();
    if (picked.isValid()) diffColor_ = picked;
    if (lastDocA_ && lastDocB_) {
        int cur = currentPageIndex();
        runDiff(lastDocA_, lastDocB_);
        if (diffResult_ && diffResult_->totalPages > 0) {
            displayPage(std::min(cur, diffResult_->totalPages - 1));
        }
    }
}

void CompareDialog::onNextChangeButton() {
    gotoNextChangedPage();
}

void CompareDialog::onPrevChangeButton() {
    gotoPrevChangedPage();
}

void CompareDialog::onSelectFileA() {
    QString path = QFileDialog::getOpenFileName(
        this, tr("Select First PDF"), QString(), tr("PDF Files (*.pdf)"));
    if (path.isEmpty()) return;

    docA_ = pdfe::core::Document::open(context_, path.toStdString());
    if (docA_) {
        fileALabel_->setText(QFileInfo(path).fileName());
        compareButton_->setEnabled(docB_ != nullptr);
    }
}

void CompareDialog::onSelectFileB() {
    QString path = QFileDialog::getOpenFileName(
        this, tr("Select Second PDF"), QString(), tr("PDF Files (*.pdf)"));
    if (path.isEmpty()) return;

    docB_ = pdfe::core::Document::open(context_, path.toStdString());
    if (docB_) {
        fileBLabel_->setText(QFileInfo(path).fileName());
        compareButton_->setEnabled(docA_ != nullptr);
    }
}

void CompareDialog::onCompare() {
    if (!docA_ || !docB_) return;

    QApplication::setOverrideCursor(Qt::BusyCursor);
    QApplication::processEvents();

    runDiff(docA_.get(), docB_.get());

    QApplication::restoreOverrideCursor();

    if (diffResult_ && diffResult_->totalPages > 0) {
        displayPage(0);
    }
}

// Shared diff driver. Applies the currently-selected highlight colour
// (GitHub #351), records the documents and engine render scale (GitHub #352)
// and refreshes the summary labels. Does NOT render a page — callers decide
// which page to display so the colour re-run can stay on the current page.
int CompareDialog::runDiff(pdfe::core::Document* docA,
                           pdfe::core::Document* docB) {
    if (!docA || !docB) {
        diffResult_.reset();
        lastDocA_ = nullptr;
        lastDocB_ = nullptr;
        return 0;
    }
    lastDocA_ = docA;
    lastDocB_ = docB;

    pdfe::diff::DiffEngine engine(context_);
    engine.setDiffColor(diffColor_.red(), diffColor_.green(), diffColor_.blue());
    lastEngineScale_ = engine.renderDpi() / 72.0f;

    diffResult_ = std::make_unique<pdfe::diff::DiffResult>(
        engine.compare(docA, docB));

    int totalPages = diffResult_->totalPages;
    if (pageSpinBox_) pageSpinBox_->setMaximum(std::max(1, totalPages));
    if (pageCountLabel_) pageCountLabel_->setText(tr("/ %1").arg(totalPages));
    if (similarityLabel_) {
        similarityLabel_->setText(
            tr("Overall: %1% similar").arg(
                static_cast<int>(diffResult_->overallSimilarity * 100)));
    }
    if (changeCountLabel_) {
        changeCountLabel_->setText(
            tr("%1 of %2 pages changed").arg(diffResult_->changedPages).arg(totalPages));
    }
    return diffResult_->changedPages;
}

void CompareDialog::onPageChanged(int page) {
    displayPage(page - 1);
}

int CompareDialog::compareLoadedDocuments(pdfe::core::Document* docA,
                                          pdfe::core::Document* docB) {
    int changed = runDiff(docA, docB);
    if (diffResult_ && diffResult_->totalPages > 0) {
        // Show the first page so navigation has a current page and the
        // overlay is composited (GitHub #352: display updates with content).
        displayPage(0);
    }
    return changed;
}

bool CompareDialog::selectDiffColorForTesting(const QColor& color) {
    if (!diffColorCombo_) return false;
    for (int i = 0; i < diffColorCombo_->count(); ++i) {
        if (diffColorCombo_->itemData(i).value<QColor>() == color) {
            // Drive the real combo: this fires currentIndexChanged, which is
            // the wiring GitHub #351 adds. If the index is already current
            // (no signal), update the colour and re-run explicitly.
            if (diffColorCombo_->currentIndex() == i) {
                onDiffColorChanged(i);
            } else {
                diffColorCombo_->setCurrentIndex(i);
            }
            return true;
        }
    }
    return false;
}

int CompareDialog::totalChangeRegions() const {
    if (!diffResult_) return 0;
    int total = 0;
    for (const auto& p : diffResult_->pages) {
        total += static_cast<int>(p.changeRegions.size());
    }
    return total;
}

int CompareDialog::changedPageCount() const {
    return diffResult_ ? diffResult_->changedPages : 0;
}

int CompareDialog::currentPageIndex() const {
    if (!pageSpinBox_) return 0;
    return std::max(0, pageSpinBox_->value() - 1);
}

namespace {
bool pageHasChange(const pdfe::diff::PageDiffResult& p) {
    return p.similarityScore < 1.0;
}
} // namespace

int CompareDialog::gotoNextChangedPage() {
    if (!diffResult_ || diffResult_->pages.empty()) return -1;
    int n = static_cast<int>(diffResult_->pages.size());
    int start = currentPageIndex();
    // GitHub #353: clamp at the end — search only forward, no modular wrap
    // back to the first change past the last one.
    for (int idx = start + 1; idx < n; ++idx) {
        if (pageHasChange(diffResult_->pages[idx])) {
            if (pageSpinBox_) pageSpinBox_->setValue(idx + 1);
            return idx;
        }
    }
    return -1;
}

int CompareDialog::gotoPrevChangedPage() {
    if (!diffResult_ || diffResult_->pages.empty()) return -1;
    int start = currentPageIndex();
    // GitHub #353: clamp at the start — search only backward, no wrap to the
    // last change before the first one.
    for (int idx = start - 1; idx >= 0; --idx) {
        if (pageHasChange(diffResult_->pages[idx])) {
            if (pageSpinBox_) pageSpinBox_->setValue(idx + 1);
            return idx;
        }
    }
    return -1;
}

QImage CompareDialog::overlayForDisplay(const QImage& overlay,
                                        const QSize& target) const {
    // GitHub #352: map the engine-DPI overlay onto a page rendered at the
    // display scale. Scaling to the page-image size registers the highlight
    // to the same page-space region as the diffed content.
    if (overlay.isNull()) return {};
    if (target.isEmpty() || overlay.size() == target) return overlay;
    return overlay.scaled(target, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
}

void CompareDialog::displayPage(int pageIndex) {
    if (!diffResult_ || pageIndex < 0 ||
        pageIndex >= static_cast<int>(diffResult_->pages.size())) return;

    const auto& pageResult = diffResult_->pages[pageIndex];
    const float scale = displayScale_;
    lastDisplayedPageIndex_ = pageIndex;
    lastOverlay_ = QImage();
    lastPageImageSize_ = QSize();

    // Render page A (prefer the docs used by the most recent diff so both the
    // file-picker flow and compareLoadedDocuments() render here).
    pdfe::core::Document* srcA = lastDocA_ ? lastDocA_ : docA_.get();
    pdfe::core::Document* srcB = lastDocB_ ? lastDocB_ : docB_.get();

    if (srcA && pageIndex < srcA->pageCount()) {
        auto pageA = srcA->page(pageIndex);
        if (pageA) {
            QImage imgA = pageA->render(pageA->boundingBox(), scale);
            // GitHub #352: the engine renders the overlay at its own (higher)
            // DPI; scale it to the display render size so the highlight
            // registers to the same page-space region as the content instead
            // of being blitted ~2x oversized at (0,0).
            QImage overlay = overlayForDisplay(pageResult.diffImage, imgA.size());
            if (!overlay.isNull()) {
                QPainter painter(&imgA);
                painter.drawImage(0, 0, overlay);
            }
            lastOverlay_ = overlay;
            lastPageImageSize_ = imgA.size();
            imageALabel_->setPixmap(QPixmap::fromImage(imgA));
            imageALabel_->resize(imgA.size());
        }
    }

    // Render page B
    if (srcB && pageIndex < srcB->pageCount()) {
        auto pageB = srcB->page(pageIndex);
        if (pageB) {
            QImage imgB = pageB->render(pageB->boundingBox(), scale);
            QImage overlay = overlayForDisplay(pageResult.diffImage, imgB.size());
            if (!overlay.isNull()) {
                QPainter painter(&imgB);
                painter.drawImage(0, 0, overlay);
            }
            imageBLabel_->setPixmap(QPixmap::fromImage(imgB));
            imageBLabel_->resize(imgB.size());
        }
    }

    // Update similarity for this page
    similarityLabel_->setText(
        tr("Page %1: %2% similar").arg(pageIndex + 1)
        .arg(static_cast<int>(pageResult.similarityScore * 100)));

    // Update text diff
    textDiffView_->clear();
    for (const auto& change : pageResult.textChanges) {
        switch (change.type) {
        case pdfe::diff::ChangeType::Deleted:
            textDiffView_->setTextColor(QColor(200, 0, 0));
            textDiffView_->append("- " + QString::fromStdString(change.text));
            break;
        case pdfe::diff::ChangeType::Inserted:
            textDiffView_->setTextColor(QColor(0, 150, 0));
            textDiffView_->append("+ " + QString::fromStdString(change.text));
            break;
        case pdfe::diff::ChangeType::Unchanged:
            break;
        default: break;
        }
    }
}

} // namespace pdfe::ui
