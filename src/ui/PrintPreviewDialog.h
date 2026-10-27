#pragma once

#include "core/PrintEngine.h"
#include <QDialog>
#include <memory>

class QPrintPreviewWidget;
class QToolBar;
class QPrinter;
class QEvent;

namespace pdfe::core { class Document; }

namespace pdfe::ui {

class PrintPreviewDialog : public QDialog {
    Q_OBJECT

public:
    PrintPreviewDialog(pdfe::core::Document* doc, const pdfe::core::PrintOptions& options, QWidget* parent = nullptr);
    ~PrintPreviewDialog() override;

protected:
    /// WIN-39: QPrintPreviewWidget paints its own backdrop and does not follow
    /// QPalette::Window, so the dialog needs to push the theme colour into it
    /// again whenever the palette changes.
    void changeEvent(QEvent* event) override;

private slots:
    void onPaintRequested(QPrinter* printer);
    void onZoomIn();
    void onZoomOut();
    void onFitWidth();
    void onFitPage();

private:
    void setupToolBar();
    void applyThemeToPreviewBackdrop();

    pdfe::core::Document* document_ = nullptr;
    pdfe::core::PrintOptions options_;
    QPrintPreviewWidget* previewWidget_ = nullptr;
    QToolBar* toolBar_ = nullptr;
};

} // namespace pdfe::ui
