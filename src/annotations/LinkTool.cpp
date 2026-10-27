#include "LinkTool.h"
#include "AnnotationManager.h"
#include "LinkAction.h"
#include "../core/Page.h"
#include "../core/CoordinateTransform.h"
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPen>
#include <QSpinBox>
#include <QStackedWidget>
#include <QWidget>
#include <algorithm>

namespace pdfe::annotations {

LinkTool::LinkTool(AnnotationManager* manager)
    : manager_(manager)
{
}

void LinkTool::onMousePress(QMouseEvent* event, int pageIndex, QPointF pdfPoint)
{
    if (event->button() != Qt::LeftButton) return;
    if (!page_) return;

    startPoint_ = pdfPoint;
    endPoint_ = pdfPoint;
    currentPageIndex_ = pageIndex;
    isDrawing_ = true;
}

void LinkTool::onMouseMove(QMouseEvent* event, int pageIndex, QPointF pdfPoint)
{
    (void)event;
    if (!isDrawing_ || pageIndex != currentPageIndex_) return;
    endPoint_ = pdfPoint;
}

void LinkTool::onMouseRelease(QMouseEvent* event, int pageIndex, QPointF pdfPoint)
{
    if (event->button() != Qt::LeftButton || !isDrawing_) return;
    if (!manager_ || !page_) {
        isDrawing_ = false;
        return;
    }

    endPoint_ = pdfPoint;
    isDrawing_ = false;

    qreal left = std::min(startPoint_.x(), endPoint_.x());
    qreal top = std::min(startPoint_.y(), endPoint_.y());
    qreal right = std::max(startPoint_.x(), endPoint_.x());
    qreal bottom = std::max(startPoint_.y(), endPoint_.y());
    QRectF rect(left, top, right - left, bottom - top);

    // Reject tiny rectangles (likely accidental click)
    if (rect.width() < 5.0 || rect.height() < 5.0) return;

    // Prompt for action type + details via a structured dialog.
    LinkAction action = promptForAction();
    if (!action.isValid()) return;

    AnnotationData data = manager_->createAnnotation(page_, AnnotationType::Link, rect);
    data.pageIndex = currentPageIndex_;

    manager_->setLinkAction(page_, data.annotIndex, action);

    // Mirror the URI (when applicable) into the annotation's /Contents so
    // older readers that look there still show something sensible. For
    // GoTo/Named we leave /Contents empty — /A is authoritative.
    if (action.type == LinkActionType::Uri) {
        manager_->setContents(page_, data.annotIndex, action.uri);
        data.contents = action.uri;
    }

    if (annotationCreated) {
        auto cb = annotationCreated;  // copy: the call may destroy this tool
        cb(currentPageIndex_, data);
    }
}

LinkAction LinkTool::promptForAction() const
{
    QDialog dlg;
    dlg.setObjectName("linkActionDialog");
    dlg.setWindowTitle(QObject::tr("Create Hyperlink"));

    auto* form = new QFormLayout(&dlg);

    auto* typeCombo = new QComboBox(&dlg);
    typeCombo->setObjectName("linkTypeCombo");
    typeCombo->setAccessibleName(QObject::tr("Link Action Type"));
    typeCombo->addItem(QObject::tr("URL / URI"),
                       static_cast<int>(LinkActionType::Uri));
    typeCombo->addItem(QObject::tr("Go to page"),
                       static_cast<int>(LinkActionType::GoTo));
    typeCombo->addItem(QObject::tr("Named action"),
                       static_cast<int>(LinkActionType::Named));
    typeCombo->addItem(QObject::tr("Launch external file..."),
                       static_cast<int>(LinkActionType::Launch));
    form->addRow(QObject::tr("Type:"), typeCombo);

    auto* stack = new QStackedWidget(&dlg);
    stack->setObjectName("linkDetailsStack");

    // --- URI page ---
    auto* uriPage = new QWidget(&dlg);
    auto* uriLayout = new QFormLayout(uriPage);
    auto* uriEdit = new QLineEdit(defaultUri_, uriPage);
    uriEdit->setObjectName("linkUriEdit");
    uriEdit->setAccessibleName(QObject::tr("Link URI"));
    uriEdit->setPlaceholderText(QObject::tr("https://example.com or mailto:..."));
    uriLayout->addRow(QObject::tr("URI:"), uriEdit);
    stack->addWidget(uriPage);

    // --- GoTo page ---
    auto* goToPage = new QWidget(&dlg);
    auto* goToLayout = new QFormLayout(goToPage);
    auto* pageSpin = new QSpinBox(goToPage);
    pageSpin->setObjectName("linkPageSpin");
    pageSpin->setAccessibleName(QObject::tr("Link Target Page"));
    pageSpin->setMinimum(1);
    pageSpin->setMaximum(100000);
    pageSpin->setValue(1);
    goToLayout->addRow(QObject::tr("Page number:"), pageSpin);
    auto* goToHint = new QLabel(
        QObject::tr("1-based page number within this document."),
        goToPage);
    goToHint->setWordWrap(true);
    goToLayout->addRow(goToHint);
    stack->addWidget(goToPage);

    // --- Named page ---
    auto* namedPage = new QWidget(&dlg);
    auto* namedLayout = new QFormLayout(namedPage);
    auto* namedCombo = new QComboBox(namedPage);
    namedCombo->setObjectName("linkNamedCombo");
    namedCombo->setAccessibleName(QObject::tr("Named Action"));
    const char* kNamedActions[] = {
        "NextPage", "PrevPage", "FirstPage", "LastPage",
        "GoBack", "GoForward", "Print", "SaveAs",
        "Find", "FullScreen", "ZoomIn", "ZoomOut",
    };
    for (const char* n : kNamedActions) namedCombo->addItem(QString::fromLatin1(n));
    namedLayout->addRow(QObject::tr("Action:"), namedCombo);
    stack->addWidget(namedPage);

    // --- Launch page ---
    auto* launchPage = new QWidget(&dlg);
    auto* launchLayout = new QFormLayout(launchPage);
    auto* launchEdit = new QLineEdit(launchPage);
    launchEdit->setObjectName("linkLaunchEdit");
    launchEdit->setAccessibleName(QObject::tr("Launch File Path"));
    launchEdit->setPlaceholderText(QObject::tr("attachment.docx or C:/..."));
    launchLayout->addRow(QObject::tr("File path:"), launchEdit);
    auto* launchWarn = new QLabel(
        QObject::tr(
            "Launch actions open external files through the OS shell "
            "when the link is followed. PDFE does not auto-execute "
            "these links; other readers (including Acrobat) will "
            "prompt the user for confirmation. Avoid pointing at "
            "executables or scripts."),
        launchPage);
    launchWarn->setWordWrap(true);
    launchLayout->addRow(launchWarn);
    stack->addWidget(launchPage);

    form->addRow(stack);

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    buttons->setObjectName("linkActionButtonBox");
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    form->addRow(buttons);

    QObject::connect(typeCombo,
        QOverload<int>::of(&QComboBox::currentIndexChanged),
        stack, &QStackedWidget::setCurrentIndex);
    stack->setCurrentIndex(0);

    if (dlg.exec() != QDialog::Accepted) return LinkAction{};

    LinkAction out;
    out.type = static_cast<LinkActionType>(typeCombo->currentData().toInt());
    switch (out.type) {
    case LinkActionType::Uri:
        out.uri = uriEdit->text().trimmed().toStdString();
        break;
    case LinkActionType::GoTo:
        out.pageIndex = pageSpin->value() - 1;  // spin is 1-based
        break;
    case LinkActionType::Named:
        out.namedAction = namedCombo->currentText().toStdString();
        break;
    case LinkActionType::Launch:
        out.launchPath = launchEdit->text().trimmed().toStdString();
        break;
    }
    return out;
}

void LinkTool::onPaintOverlay(QPainter& painter, int pageIndex,
                                float zoom, QPointF pageOrigin, float pageHeightPt)
{
    if (!isDrawing_ || pageIndex != currentPageIndex_) return;

    painter.save();

    // Dashed blue rectangle to indicate link area being drawn
    QPen pen(QColor(0, 0, 200));
    pen.setWidthF(1.5f);
    pen.setStyle(Qt::DashLine);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);

    QPointF screenStart = pdfe::core::CoordinateTransform::pdfToScreen(
        startPoint_, zoom, pageOrigin, pageHeightPt);
    QPointF screenEnd = pdfe::core::CoordinateTransform::pdfToScreen(
        endPoint_, zoom, pageOrigin, pageHeightPt);

    qreal left = std::min(screenStart.x(), screenEnd.x());
    qreal top = std::min(screenStart.y(), screenEnd.y());
    qreal right = std::max(screenStart.x(), screenEnd.x());
    qreal bottom = std::max(screenStart.y(), screenEnd.y());
    painter.drawRect(QRectF(QPointF(left, top), QPointF(right, bottom)));

    painter.restore();
}

} // namespace pdfe::annotations
