#pragma once

#include <QAccessible>
#include <QAccessibleWidget>
#include <QString>
#include <QStringList>
#include <QRect>
#include <QPoint>

#include <vector>

namespace pdfe::ui {

class PdfCanvas;
class PdfPageAccessible;

/**
 * @brief Provides accessibility support for the PdfCanvas widget.
 *
 * Implements QAccessibleWidget so screen readers and other assistive
 * technologies can observe PdfCanvas state and drive its actions.
 *
 * R2-18 (accessibility): additionally implements QAccessibleTextInterface,
 * exposing the loaded document's extracted text (all pages concatenated) so a
 * screen reader can read the page content, and exposes one child accessible
 * object per document page (PdfPageAccessible) — each of which is itself a
 * QAccessibleTextInterface carrying that page's extracted text. This makes the
 * page text reachable through the accessibility tree, which the prior
 * implementation (childCount() hard-returning 0, no text interface) did not.
 */
class PdfCanvasAccessible : public QAccessibleWidget,
                            public QAccessibleTextInterface {
public:
    explicit PdfCanvasAccessible(QWidget* widget);
    ~PdfCanvasAccessible() override;

    // QAccessibleInterface overrides
    QAccessible::Role role() const override;
    QString text(QAccessible::Text t) const override;
    int childCount() const override;
    QAccessibleInterface* child(int index) const override;
    QAccessibleInterface* childAt(int x, int y) const override;
    int indexOfChild(const QAccessibleInterface* child) const override;
    void* interface_cast(QAccessible::InterfaceType type) override;

    // QAccessibleActionInterface
    QStringList actionNames() const override;
    void doAction(const QString& actionName) override;
    QString localizedActionName(const QString& actionName) const override;

    // QAccessibleTextInterface — exposes the whole document's extracted text
    // (per-page text, joined). Selection is not supported (always empty).
    void selection(int selectionIndex, int* startOffset, int* endOffset) const override;
    int selectionCount() const override;
    void addSelection(int startOffset, int endOffset) override;
    void removeSelection(int selectionIndex) override;
    void setSelection(int selectionIndex, int startOffset, int endOffset) override;
    int cursorPosition() const override;
    void setCursorPosition(int position) override;
    QString text(int startOffset, int endOffset) const override;
    QString textBeforeOffset(int offset, QAccessible::TextBoundaryType boundaryType,
                             int* startOffset, int* endOffset) const override;
    QString textAfterOffset(int offset, QAccessible::TextBoundaryType boundaryType,
                            int* startOffset, int* endOffset) const override;
    QString textAtOffset(int offset, QAccessible::TextBoundaryType boundaryType,
                         int* startOffset, int* endOffset) const override;
    int characterCount() const override;
    QRect characterRect(int offset) const override;
    int offsetAtPoint(const QPoint& point) const override;
    void scrollToSubstring(int startIndex, int endIndex) override;
    QString attributes(int offset, int* startOffset, int* endOffset) const override;

    /// Full extracted text of the whole document — each page's plain text in
    /// page order, joined with a newline. Public so tests (and the per-page
    /// child interfaces) can reach the same text the a11y tree exposes.
    QString allPagesText() const;

    // Custom action names
    static const QString NextPageAction;
    static const QString PreviousPageAction;
    static const QString ZoomInAction;
    static const QString ZoomOutAction;

private:
    PdfCanvas* canvas() const;

    /// Rebuild the per-page child interface list when the loaded document or
    /// its page count changes. Children are owned by this object and deleted
    /// in the destructor (and on rebuild), mirroring how Qt's own
    /// QAccessibleTable owns its non-QObject cell interfaces.
    void rebuildChildrenIfNeeded() const;

    // Page child interfaces. Each PdfPageAccessible is OWNED BY Qt's
    // QAccessibleCache once registered (registerAccessibleInterface), NOT by this
    // object — Qt deletes them at cleanupAccessibleCache (QApplication teardown).
    // We therefore never raw-delete them (that left dangling cache entries that
    // Qt then dereferenced at shutdown → use-after-free). pageChildIds_ holds the
    // cache ids so a document change can remove stale children cache-safely via
    // QAccessible::deleteAccessibleInterface. Parallel to pageChildren_.
    mutable std::vector<PdfPageAccessible*> pageChildren_;
    mutable std::vector<QAccessible::Id> pageChildIds_;
    mutable const void* cachedDoc_ = nullptr;
    mutable int cachedPageCount_ = -1;
    int cursorPos_ = 0;
};

/**
 * @brief Factory function for QAccessible::installFactory().
 *
 * Creates PdfCanvasAccessible instances for PdfCanvas widgets. Register with
 * QAccessible::installFactory() during application initialization.
 */
QAccessibleInterface* pdfCanvasAccessibleFactory(const QString& classname, QObject* object);

} // namespace pdfe::ui
