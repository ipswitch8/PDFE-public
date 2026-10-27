#include "SearchPanel.h"
#include "core/Document.h"
#include "core/SearchEngine.h"
#include "core/SearchResult.h"
#include "search/DocumentSearchIndex.h"
#include "search/TantivyIndex.h"
#include "app/AppSettings.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QCheckBox>
#include <QPushButton>
#include <QListWidget>
#include <QLabel>

namespace pdfe::ui {

SearchPanel::SearchPanel(QWidget* parent)
    : QWidget(parent)
{
    // Create the main layout
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(8, 8, 8, 8);
    mainLayout->setSpacing(8);

    // Search input row
    auto* searchLayout = new QHBoxLayout();
    searchLayout->setSpacing(4);

    searchEdit_ = new QLineEdit(this);
    searchEdit_->setObjectName("searchEdit");
    searchEdit_->setAccessibleName(tr("Search Text"));
    searchEdit_->setPlaceholderText("Search...");
    searchLayout->addWidget(searchEdit_);

    searchButton_ = new QPushButton("Search", this);
    searchButton_->setObjectName("searchButton");
    searchButton_->setAccessibleName(tr("Search"));
    searchLayout->addWidget(searchButton_);

    clearButton_ = new QPushButton("Clear", this);
    clearButton_->setObjectName("clearButton");
    clearButton_->setAccessibleName(tr("Clear Search"));
    searchLayout->addWidget(clearButton_);

    mainLayout->addLayout(searchLayout);

    // Options row
    auto* optionsLayout = new QHBoxLayout();
    optionsLayout->setSpacing(8);

    caseSensitiveCheck_ = new QCheckBox("Case sensitive", this);
    caseSensitiveCheck_->setObjectName("caseSensitiveCheck");
    caseSensitiveCheck_->setAccessibleName(tr("Case Sensitive Search"));
    optionsLayout->addWidget(caseSensitiveCheck_);

    wholeWordCheck_ = new QCheckBox("Whole word", this);
    wholeWordCheck_->setObjectName("wholeWordCheck");
    wholeWordCheck_->setAccessibleName(tr("Whole Word Search"));
    optionsLayout->addWidget(wholeWordCheck_);

    optionsLayout->addStretch();
    mainLayout->addLayout(optionsLayout);

    auto* replaceLayout = new QHBoxLayout();
    replaceLayout->setSpacing(4);

    auto* replaceLabel = new QLabel(tr("Replace:"), this);
    replaceLayout->addWidget(replaceLabel);

    replaceField_ = new QLineEdit(this);
    replaceField_->setObjectName("replaceField");
    replaceField_->setAccessibleName(tr("Replace with"));
    replaceField_->setPlaceholderText(tr("Replace with..."));
    replaceLayout->addWidget(replaceField_);

    mainLayout->addLayout(replaceLayout);

    auto* replaceButtonLayout = new QHBoxLayout();
    replaceButtonLayout->setSpacing(4);

    replaceButton_ = new QPushButton(tr("Replace"), this);
    replaceButton_->setObjectName("replaceButton");
    replaceButton_->setAccessibleName(tr("Replace"));
    replaceButton_->setEnabled(false);
    replaceButtonLayout->addWidget(replaceButton_);

    replaceAllButton_ = new QPushButton(tr("Replace All"), this);
    replaceAllButton_->setObjectName("replaceAllButton");
    replaceAllButton_->setAccessibleName(tr("Replace All"));
    replaceAllButton_->setEnabled(false);
    replaceButtonLayout->addWidget(replaceAllButton_);

    replaceButtonLayout->addStretch();
    mainLayout->addLayout(replaceButtonLayout);

    replaceStatusLabel_ = new QLabel(this);
    replaceStatusLabel_->setObjectName("replaceStatusLabel");
    replaceStatusLabel_->setAccessibleName(tr("Replace Status"));
    mainLayout->addWidget(replaceStatusLabel_);

    statusLabel_ = new QLabel(this);
    statusLabel_->setObjectName("statusLabel");
    statusLabel_->setAccessibleName(tr("Search Status"));
    statusLabel_->setText(tr("Enter search term and press Search"));
    mainLayout->addWidget(statusLabel_);

    resultsListWidget_ = new QListWidget(this);
    resultsListWidget_->setObjectName("resultsList");
    resultsListWidget_->setAccessibleName(tr("Search Results"));
    mainLayout->addWidget(resultsListWidget_);

    connect(searchEdit_, &QLineEdit::returnPressed, this, &SearchPanel::onSearchClicked);
    connect(searchButton_, &QPushButton::clicked, this, &SearchPanel::onSearchClicked);
    connect(clearButton_, &QPushButton::clicked, this, &SearchPanel::onClearClicked);
    connect(resultsListWidget_, &QListWidget::currentRowChanged, this, &SearchPanel::onResultItemClicked);
    connect(replaceButton_, &QPushButton::clicked, this, &SearchPanel::onReplaceClicked);
    connect(replaceAllButton_, &QPushButton::clicked, this, &SearchPanel::onReplaceAllClicked);

    // Pipeline b3f19d72 phase-3 (P7-BUG-3 / P7-BUG-4): Replace and
    // Replace All become clickable as soon as a document is loaded and
    // the find-text field is non-empty.  Previously these buttons stayed
    // disabled until onSearchClicked had populated a non-empty result
    // list, which forced users to click Search before they could click
    // Replace.  The downstream MainWindow handlers (onReplace /
    // onReplaceAll) already tolerate a null currentSearchResult by
    // falling back to a first-match / all-pages span scan.
    connect(searchEdit_, &QLineEdit::textChanged, this,
            [this](const QString& text) {
                bool canReplace = (document_ != nullptr) && !text.isEmpty();
                replaceButton_->setEnabled(canReplace);
                replaceAllButton_->setEnabled(canReplace);
            });

    searchButton_->setEnabled(false);
    clearButton_->setEnabled(false);
}

SearchPanel::~SearchPanel() = default;

void SearchPanel::setDocument(pdfe::core::Document* doc) {
    document_ = doc;
    searchButton_->setEnabled(doc != nullptr);

    // Drop any Tantivy index bound to the previous document — the new
    // one will be built lazily on first search.
    tantivyIndex_.reset();

    // Clear any existing results
    onClearClicked();

    if (doc) {
        statusLabel_->setText("Enter search term and press Search");
    } else {
        statusLabel_->setText("No document loaded");
    }
}

void SearchPanel::clearDocument() {
    setDocument(nullptr);
}

void SearchPanel::focusSearchBox() {
    searchEdit_->setFocus();
    searchEdit_->selectAll();
}

void SearchPanel::setReplaceStatus(const QString& text) {
    replaceStatusLabel_->setText(text);
}

void SearchPanel::selectResult(int index) {
    if (index >= 0 && index < resultsListWidget_->count()) {
        resultsListWidget_->setCurrentRow(index);
        onResultItemClicked(index);
    }
}

int SearchPanel::currentResultIndex() const {
    return resultsListWidget_->currentRow();
}

const pdfe::core::SearchResult* SearchPanel::currentSearchResult() const {
    return currentResult_.get();
}

void SearchPanel::onSearchClicked() {
    if (!document_) {
        statusLabel_->setText("No document loaded");
        return;
    }

    QString query = searchEdit_->text();
    if (query.isEmpty()) {
        statusLabel_->setText("Please enter a search term");
        return;
    }

    // Clear previous results
    resultsListWidget_->clear();
    currentResult_.reset();
    emit clearSearchHighlights();

    // Set up search flags
    pdfe::core::SearchFlags flags = pdfe::core::SearchFlags::None;
    if (caseSensitiveCheck_->isChecked()) {
        flags = flags | pdfe::core::SearchFlags::CaseSensitive;
    }
    if (wholeWordCheck_->isChecked()) {
        flags = flags | pdfe::core::SearchFlags::WholeWord;
    }

    // Perform search
    statusLabel_->setText("Searching...");
    searchButton_->setEnabled(false);

    // Phase 6: route through Tantivy when the feature flag is on. Tantivy
    // produces a ranked list of candidate pages; we then run MuPDF's
    // fz_search_page on only those pages to get the precise quad
    // rectangles the UI needs for highlighting.
    bool useTantivy = false;
    try {
        pdfe::app::AppSettings settings;
        useTantivy = settings.useTantivySearch();
    } catch (...) {
        useTantivy = true;  // default on
    }

    // PERMISSION GATE. The Tantivy backend writes every page's plain text into
    // an index under the system temp directory — a plaintext-searchable copy of
    // the document outside its encrypted container. A document that denies BOTH
    // extract-for-accessibility (ISO 32000-1 bit 10) and copy (bit 5) must not
    // be materialised that way. This uses the same predicate as the
    // accessibility gate in PdfCanvasAccessible.cpp so the product has one
    // extraction-permission rule.
    //
    // Search is NOT disabled: dropping useTantivy transparently routes this
    // search through pdfe::core::SearchEngine below, which is purely in-process
    // and writes nothing to disk. DocumentSearchIndex enforces the same refusal
    // itself (defence in depth) — this site keeps the UI honest about which
    // backend it reports and avoids constructing an index that would refuse.
    if (useTantivy) {
        try {
            if (!document_->extractForAccessibilityAllowed() &&
                !document_->copyAllowed()) {
                useTantivy = false;
            }
        } catch (...) {
            useTantivy = false;  // fail closed
        }
    }

    try {
        pdfe::core::SearchEngine engine(document_);
        pdfe::core::SearchResult result;

        if (useTantivy) {
            if (!tantivyIndex_) {
                tantivyIndex_ = std::make_unique<pdfe::search::DocumentSearchIndex>(document_);
            }
            auto tantivyHits = tantivyIndex_->searchPages(query.toStdString(), 500);
            if (tantivyHits.empty()) {
                result.query = query.toStdString();
                result.caseSensitive = hasFlag(flags, pdfe::core::SearchFlags::CaseSensitive);
            } else {
                std::vector<int> candidatePages;
                candidatePages.reserve(tantivyHits.size());
                for (const auto& h : tantivyHits) candidatePages.push_back(h.pageIndex);
                result = engine.searchSpecificPages(
                    query.toStdString(), candidatePages, flags);
            }
        } else {
            result = engine.search(query.toStdString(), flags);
        }

        // Store result
        currentResult_ = std::make_unique<pdfe::core::SearchResult>(std::move(result));

        // Update UI
        updateResultsList(*currentResult_);

        // Update status
        int matchCount = currentResult_->totalMatches();
        const char* backend = useTantivy ? "tantivy" : "mupdf";
        if (matchCount == 0) {
            statusLabel_->setText(QString("No matches found (%1)").arg(backend));
        } else if (matchCount == 1) {
            statusLabel_->setText(QString("1 match found (%1)").arg(backend));
        } else {
            statusLabel_->setText(QString("%1 matches found (%2)")
                .arg(matchCount).arg(backend));
        }

        clearButton_->setEnabled(true);
        bool hasMatches = (matchCount > 0);
        replaceButton_->setEnabled(hasMatches);
        replaceAllButton_->setEnabled(hasMatches);
        replaceStatusLabel_->clear();
    } catch (const std::exception& e) {
        statusLabel_->setText(QString("Search error: %1").arg(e.what()));
    }

    searchButton_->setEnabled(true);
}

void SearchPanel::onReplaceClicked() {
    QString find = searchEdit_->text();
    QString replace = replaceField_->text();
    if (find.isEmpty()) return;
    emit replaceRequested(find, replace);
}

void SearchPanel::onReplaceAllClicked() {
    QString find = searchEdit_->text();
    QString replace = replaceField_->text();
    if (find.isEmpty()) return;
    emit replaceAllRequested(find, replace);
}

int SearchPanel::performReplace(const QString& find,
                                 const QString& replace,
                                 bool replaceAll)
{
    // Pipeline a7c4f891 phase-2 (D2 / P-GAP-79): emit the same
    // signal that the user-driven button click would emit. Populates
    // the searchEdit_ + replaceField_ fields so the panel's visible
    // state matches what was acted on (helpful for test diagnostics
    // and for any subsequent button-click re-runs).
    //
    // Returns 0 if find is empty (no signal emitted, no-op).
    // Returns 1 if the replace signal was emitted. See the header
    // for the asynchronous vs synchronous count contract.
    if (find.isEmpty()) return 0;
    if (searchEdit_) searchEdit_->setText(find);
    if (replaceField_) replaceField_->setText(replace);
    if (replaceAll) {
        emit replaceAllRequested(find, replace);
    } else {
        emit replaceRequested(find, replace);
    }
    return 1;
}

void SearchPanel::onClearClicked() {
    searchEdit_->clear();
    replaceField_->clear();
    resultsListWidget_->clear();
    currentResult_.reset();
    clearButton_->setEnabled(false);
    replaceButton_->setEnabled(false);
    replaceAllButton_->setEnabled(false);
    replaceStatusLabel_->clear();
    statusLabel_->setText(tr("Enter search term and press Search"));
    emit clearSearchHighlights();
}

void SearchPanel::onResultItemClicked(int row) {
    if (!currentResult_ || row < 0 || row >= currentResult_->totalMatches()) {
        return;
    }

    const pdfe::core::SearchHit& hit = currentResult_->hits[row];

    // Convert std::vector<QRectF> to QVector<QRectF> for signal
    QVector<QRectF> quads;
    quads.reserve(hit.quads.size());
    for (const auto& quad : hit.quads) {
        quads.push_back(quad);
    }

    emit searchResultSelected(hit.pageIndex, quads);
}

void SearchPanel::updateResultsList(const pdfe::core::SearchResult& result) {
    resultsListWidget_->clear();

    for (const auto& hit : result.hits) {
        // Format: "Page X: ...context..."
        QString itemText = QString("Page %1: %2")
            .arg(hit.pageIndex + 1)  // Display as 1-based
            .arg(QString::fromStdString(hit.context));

        resultsListWidget_->addItem(itemText);
    }
}

} // namespace pdfe::ui
