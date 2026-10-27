#include "PrintPreviewDialog.h"
#include "core/Document.h"
#include "core/PrintEngine.h"
#include "TestDialogSeam.h"

#include <QPrintPreviewWidget>
#include <QPrinter>
#include <QToolBar>
#include <QAction>
#include <QVBoxLayout>
#include <QEvent>
#include <QGraphicsView>

namespace pdfe::ui {

PrintPreviewDialog::PrintPreviewDialog(pdfe::core::Document* doc, const pdfe::core::PrintOptions& options, QWidget* parent)
    : QDialog(parent)
    , document_(doc)
    , options_(options)
{
    setWindowTitle("Print Preview");
    setObjectName("printPreviewDialog");
    setAccessibleName(tr("Print Preview Dialog"));

    auto* layout = new QVBoxLayout(this);

    // Create toolbar
    toolBar_ = new QToolBar(this);
    toolBar_->setObjectName("previewToolBar");
    toolBar_->setAccessibleName(tr("Print Preview Toolbar"));
    setupToolBar();
    layout->addWidget(toolBar_);

    // Create print preview widget
    previewWidget_ = new QPrintPreviewWidget(this);
    previewWidget_->setObjectName("printPreviewWidget");
    previewWidget_->setAccessibleName(tr("Print Preview"));
    layout->addWidget(previewWidget_);

    // Connect paint signal
    connect(previewWidget_, &QPrintPreviewWidget::paintRequested,
            this, &PrintPreviewDialog::onPaintRequested);

    applyThemeToPreviewBackdrop();

    resize(800, 600);

    // GAP-A14 keyboard-state seam: install the dialog-child keyboard
    // query/result pipes (gated by PDFE_TEST_PIPES inside the helper).
    pdfe::ui::testseam::registerDialogIntrospection(this);
}

PrintPreviewDialog::~PrintPreviewDialog() = default;

// WIN-39 (phase-8). QPrintPreviewWidget renders the pages onto a backdrop it
// paints itself — Qt's fixed mid-grey #a0a0a4 — through an internal
// QGraphicsView whose background brush is NOT taken from QPalette::Window. So
// under the dark theme this dialog showed a light-grey slab filling most of
// its area while every other surface went dark. The dark-pixel oracle measured
// a dominant colour 187 units from the dark base; it is the one dialog of
// twenty-two that failed, and it failed because the widget genuinely ignores
// the palette rather than because the test was wrong.
//
// The internal view is a private Qt subclass, but it is still a QGraphicsView
// CHILD, and setting its background brush is the supported way to reach it.
// Guarded rather than asserted: if a future Qt changes the internal structure,
// the preview should keep working and this test should go red — a hard failure
// here would break printing over a cosmetic defect.
void PrintPreviewDialog::applyThemeToPreviewBackdrop()
{
    if (!previewWidget_) return;
    const QColor window = palette().color(QPalette::Window);
    for (QGraphicsView* view : previewWidget_->findChildren<QGraphicsView*>()) {
        view->setBackgroundBrush(window);
    }
}

void PrintPreviewDialog::changeEvent(QEvent* event)
{
    QDialog::changeEvent(event);
    // The backdrop is pushed, not inherited, so it has to be pushed again on
    // every palette change — a theme switch while the dialog is open would
    // otherwise leave the old colour behind.
    if (event && (event->type() == QEvent::PaletteChange ||
                  event->type() == QEvent::ApplicationPaletteChange ||
                  event->type() == QEvent::ThemeChange)) {
        applyThemeToPreviewBackdrop();
    }
}

void PrintPreviewDialog::setupToolBar() {
    // Zoom In action
    auto* zoomInAction = new QAction("Zoom In", this);
    zoomInAction->setObjectName("zoomInAction");
    zoomInAction->setShortcut(QKeySequence::ZoomIn);
    connect(zoomInAction, &QAction::triggered, this, &PrintPreviewDialog::onZoomIn);
    toolBar_->addAction(zoomInAction);

    // Zoom Out action
    auto* zoomOutAction = new QAction("Zoom Out", this);
    zoomOutAction->setObjectName("zoomOutAction");
    zoomOutAction->setShortcut(QKeySequence::ZoomOut);
    connect(zoomOutAction, &QAction::triggered, this, &PrintPreviewDialog::onZoomOut);
    toolBar_->addAction(zoomOutAction);

    toolBar_->addSeparator();

    // Fit Width action
    auto* fitWidthAction = new QAction("Fit Width", this);
    fitWidthAction->setObjectName("fitWidthAction");
    connect(fitWidthAction, &QAction::triggered, this, &PrintPreviewDialog::onFitWidth);
    toolBar_->addAction(fitWidthAction);

    // Fit Page action
    auto* fitPageAction = new QAction("Fit Page", this);
    fitPageAction->setObjectName("fitPageAction");
    connect(fitPageAction, &QAction::triggered, this, &PrintPreviewDialog::onFitPage);
    toolBar_->addAction(fitPageAction);
}

void PrintPreviewDialog::onPaintRequested(QPrinter* printer) {
    if (!printer || !document_) {
        return;
    }

    // Use PrintEngine to render pages
    pdfe::core::PrintEngine engine(document_);
    engine.print(printer, options_);
}

void PrintPreviewDialog::onZoomIn() {
    if (previewWidget_) {
        previewWidget_->zoomIn();
    }
}

void PrintPreviewDialog::onZoomOut() {
    if (previewWidget_) {
        previewWidget_->zoomOut();
    }
}

void PrintPreviewDialog::onFitWidth() {
    if (previewWidget_) {
        previewWidget_->fitToWidth();
    }
}

void PrintPreviewDialog::onFitPage() {
    if (previewWidget_) {
        previewWidget_->fitInView();
    }
}

} // namespace pdfe::ui
