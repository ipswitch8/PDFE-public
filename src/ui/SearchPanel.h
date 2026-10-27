#pragma once

#include <QWidget>
#include <memory>
#include <vector>
#include <QRectF>

class QLineEdit;
class QCheckBox;
class QPushButton;
class QListWidget;
class QLabel;
class QVBoxLayout;

namespace pdfe::core {
class Document;
struct SearchResult;
struct SearchHit;
}

namespace pdfe::search {
class DocumentSearchIndex;
}

namespace pdfe::ui {

/// Panel for searching text within a PDF document.
///
/// Provides a search input, options (case-sensitive, whole word), results list,
/// and emits signals when results are selected for highlighting in the viewer.
class SearchPanel : public QWidget {
    Q_OBJECT

public:
    explicit SearchPanel(QWidget* parent = nullptr);
    ~SearchPanel() override;

    /// Set the document to search.
    void setDocument(pdfe::core::Document* doc);

    /// Clear the current document.
    void clearDocument();

    void focusSearchBox();
    void setReplaceStatus(const QString& text);
    void selectResult(int index);
    int currentResultIndex() const;
    const pdfe::core::SearchResult* currentSearchResult() const;

    /// Pipeline a7c4f891 phase-2 (D2 / P-GAP-79): programmatic
    /// search+replace seam for tests.
    ///
    /// Return-value contract: 0 if @p find is empty (no signal
    /// emitted, no-op); 1 if the replace signal was emitted
    /// successfully. The return value is NOT the count of actual
    /// replacements made — the signal/slot architecture emits one
    /// signal which propagates asynchronously through the document
    /// edit pipeline. For the actual per-instance count, callers
    /// must poll @ref replaceStatusLabel_ which is updated to
    /// "N replaced" by the downstream handler after the edit
    /// pipeline settles. This deliberate divergence from a
    /// synchronous count preserves the architecture's non-blocking
    /// edit model (karen+validator phase-2 review requirement).
    Q_INVOKABLE int performReplace(const QString& find,
                                    const QString& replace,
                                    bool replaceAll);

signals:
    void searchResultSelected(int pageIndex, QVector<QRectF> quads);
    void clearSearchHighlights();
    void replaceRequested(const QString& find, const QString& replace);
    void replaceAllRequested(const QString& find, const QString& replace);

private slots:
    void onSearchClicked();
    void onClearClicked();
    void onResultItemClicked(int row);
    void onReplaceClicked();
    void onReplaceAllClicked();

private:
    void updateResultsList(const pdfe::core::SearchResult& result);

    pdfe::core::Document* document_ = nullptr;
    std::unique_ptr<pdfe::core::SearchResult> currentResult_;
    std::unique_ptr<pdfe::search::DocumentSearchIndex> tantivyIndex_;

    QLineEdit* searchEdit_ = nullptr;
    QCheckBox* caseSensitiveCheck_ = nullptr;
    QCheckBox* wholeWordCheck_ = nullptr;
    QPushButton* searchButton_ = nullptr;
    QPushButton* clearButton_ = nullptr;
    QListWidget* resultsListWidget_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QLineEdit* replaceField_ = nullptr;
    QPushButton* replaceButton_ = nullptr;
    QPushButton* replaceAllButton_ = nullptr;
    QLabel* replaceStatusLabel_ = nullptr;
};

} // namespace pdfe::ui
