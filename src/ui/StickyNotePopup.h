#pragma once

#include <QFrame>
#include "../annotations/AnnotationData.h"

class QTextEdit;
class QLabel;

namespace pdfe::ui {

/// Inline popup widget for editing sticky note contents on the canvas.
///
/// Pipeline b3f19d72 phase-6 (P8-BUG-1): the popup is constructed as a
/// top-level frameless tool window (Qt::Tool | Qt::FramelessWindowHint)
/// rather than a child widget of PdfCanvas. PdfCanvasAccessible reports
/// childCount()=0 to UIA, so any QWidget parented under PdfCanvas is
/// invisible to UIA traversal. Making the popup top-level exposes it as
/// a discoverable child window of the application — pywinauto + screen
/// readers can locate ``stickyNotePopupEdit`` reliably.  Anchor positions
/// passed to openForAnnotation / updatePosition are now in GLOBAL screen
/// coordinates (the caller converts from canvas-local via mapToGlobal).
class StickyNotePopup : public QFrame {
    Q_OBJECT

public:
    explicit StickyNotePopup(QWidget* parent = nullptr);
    ~StickyNotePopup() override;

    /// Open the popup for a specific annotation.
    /// @param iconScreenBottomRight  GLOBAL screen coordinates of the
    ///        annotation icon's bottom-right corner.
    void openForAnnotation(const pdfe::annotations::AnnotationData& data,
                           QPoint iconScreenBottomRight);

    /// Close the popup, committing any pending changes.
    void closePopup();

    /// Update the popup's screen position (call on zoom/scroll changes).
    /// @param iconScreenBottomRight  GLOBAL screen coordinates.
    void updatePosition(QPoint iconScreenBottomRight);

    /// Get the current contents text.
    QString contents() const;

    /// Set the contents text (for external sync from dock panel).
    void setContents(const QString& text);

    /// Return the annotation data this popup is editing.
    const pdfe::annotations::AnnotationData& annotationData() const { return data_; }

    /// Whether the popup is currently showing.
    bool isPopupVisible() const { return isVisible(); }

    /// Set read-only mode (single-click preview vs double-click edit).
    void setReadOnly(bool readOnly);
    bool isReadOnly() const { return readOnly_; }

signals:
    /// Emitted when the user edits text in the popup.
    void contentsChanged(pdfe::annotations::AnnotationData data);

    /// Emitted when the popup is closed.
    void popupClosed(int pageIndex, int annotIndex);

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    void applyColorTheme(QColor annotColor);
    void positionPopup(QPoint anchor);

    pdfe::annotations::AnnotationData data_;
    QTextEdit* textEdit_ = nullptr;
    QLabel* titleBar_ = nullptr;
    bool suppressSync_ = false;
    bool readOnly_ = false;
    bool dragging_ = false;
    QPoint dragOffset_;

    static constexpr int POPUP_WIDTH = 200;
    static constexpr int POPUP_HEIGHT = 150;
    static constexpr int TITLE_BAR_HEIGHT = 20;
};

} // namespace pdfe::ui
