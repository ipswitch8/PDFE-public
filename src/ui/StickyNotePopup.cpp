#include "StickyNotePopup.h"
#include "UndoFallthroughFilter.h"   // GitHub #295

#include <QVBoxLayout>
#include <QTextEdit>
#include <QLabel>
#include <QEvent>
#include <QMouseEvent>
#include <QApplication>
#include <QScreen>

namespace pdfe::ui {

StickyNotePopup::StickyNotePopup(QWidget* parent)
    // Pipeline b3f19d72 phase-6 (P8-BUG-1): force the popup to be a
    // top-level window (Qt::Tool | Qt::FramelessWindowHint) so it is
    // not buried under PdfCanvas's QAccessible::Canvas role in the UIA
    // tree.  Keeping `parent` non-null preserves auto-destruction with
    // the owning window and keeps Qt::WA_DeleteOnClose semantics, but
    // the window flag promotes it to a top-level frameless tool window
    // visible to pywinauto + screen readers.
    : QFrame(parent, Qt::Tool | Qt::FramelessWindowHint)
{
    setObjectName("stickyNotePopup");
    setAccessibleName(tr("Sticky Note Popup"));
    setFrameStyle(QFrame::Box | QFrame::Raised);
    setLineWidth(1);
    setFixedSize(POPUP_WIDTH, POPUP_HEIGHT);
    hide();

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(2, 2, 2, 2);
    layout->setSpacing(0);

    // Title bar
    titleBar_ = new QLabel("Sticky Note", this);
    titleBar_->setObjectName("stickyNotePopupTitle");
    titleBar_->setAccessibleName(tr("Sticky Note Title"));
    // openForAnnotation() replaces this text with the annotation's /T author
    // string, which AnnotationManager reads straight off the annotation
    // dictionary and is therefore fully attacker-controlled. QLabel's default
    // Qt::AutoText would render a crafted author as HTML, and a QLabel in
    // rich-text mode resolves <img src=...> -- so double-clicking a sticky note
    // in a malicious PDF becomes a filesystem/network read chosen by the
    // document. Found by the phase-69 security-audit gate as a sibling of the
    // MetadataDialog fix.
    titleBar_->setTextFormat(Qt::PlainText);
    titleBar_->setFixedHeight(TITLE_BAR_HEIGHT);
    titleBar_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    titleBar_->setContentsMargins(4, 0, 4, 0);
    QFont titleFont = titleBar_->font();
    titleFont.setPointSize(8);
    titleFont.setBold(true);
    titleBar_->setFont(titleFont);
    layout->addWidget(titleBar_);

    // Text edit
    textEdit_ = new QTextEdit(this);
    textEdit_->setObjectName("stickyNotePopupEdit");
    textEdit_->setAccessibleName(tr("Sticky Note Text"));
    textEdit_->setFrameStyle(QFrame::NoFrame);
    textEdit_->setAcceptRichText(false);
    // GitHub #295, same property as annotContentsEdit rather than the same
    // symptom: this editor holds focus while a note is open, so an empty note
    // swallows Ctrl+Z here too. Fixed by property, not by the one path that was
    // measured red.
    textEdit_->installEventFilter(
        new pdfe::ui::UndoFallthroughFilter(textEdit_));
    layout->addWidget(textEdit_);

    // Connect text changes
    connect(textEdit_, &QTextEdit::textChanged, this, [this]() {
        if (!suppressSync_) {
            data_.contents = textEdit_->toPlainText().toStdString();
            emit contentsChanged(data_);
        }
    });

    // Install event filter on parent to detect outside clicks
    if (parent) {
        parent->installEventFilter(this);
    }
}

StickyNotePopup::~StickyNotePopup() = default;

void StickyNotePopup::openForAnnotation(const pdfe::annotations::AnnotationData& data,
                                         QPoint iconScreenBottomRight)
{
    data_ = data;
    applyColorTheme(data.color);

    // Set title based on annotation type
    QString author = QString::fromStdString(data.author);
    if (author.isEmpty()) {
        if (data.type == pdfe::annotations::AnnotationType::FreeText) {
            titleBar_->setText(tr("Text Box"));
        } else {
            titleBar_->setText(tr("Sticky Note"));
        }
    } else {
        titleBar_->setText(author);
    }

    // Set text content
    suppressSync_ = true;
    textEdit_->setPlainText(QString::fromStdString(data.contents));
    suppressSync_ = false;

    // Default to editable mode when opened via double-click
    setReadOnly(false);

    positionPopup(iconScreenBottomRight);
    show();
    raise();
    // Pipeline b3f19d72 phase-6 (P8-BUG-1): as a top-level Qt::Tool
    // window the popup does not automatically take the foreground
    // activation when shown, so keystrokes route to the main window
    // instead of the QTextEdit. activateWindow() + setFocus() ensures
    // typed text lands in the popup.
    activateWindow();
    textEdit_->setFocus(Qt::PopupFocusReason);
}

void StickyNotePopup::closePopup()
{
    if (!isVisible()) return;

    hide();
    emit popupClosed(data_.pageIndex, data_.annotIndex);
}

void StickyNotePopup::updatePosition(QPoint iconScreenBottomRight)
{
    if (isVisible()) {
        positionPopup(iconScreenBottomRight);
    }
}

QString StickyNotePopup::contents() const
{
    return textEdit_ ? textEdit_->toPlainText() : QString();
}

void StickyNotePopup::setContents(const QString& text)
{
    if (!textEdit_) return;

    suppressSync_ = true;
    textEdit_->setPlainText(text);
    suppressSync_ = false;
}

void StickyNotePopup::setReadOnly(bool readOnly)
{
    readOnly_ = readOnly;
    if (textEdit_) {
        textEdit_->setReadOnly(readOnly);
    }
    // Update visual style to indicate read-only state
    applyColorTheme(data_.color);
}

bool StickyNotePopup::eventFilter(QObject* obj, QEvent* event)
{
    if (isVisible() && event->type() == QEvent::MouseButtonPress) {
        auto* mouseEvent = static_cast<QMouseEvent*>(event);
        QPoint globalPos = mouseEvent->globalPosition().toPoint();
        QRect popupGlobalRect(mapToGlobal(QPoint(0, 0)), size());

        if (!popupGlobalRect.contains(globalPos)) {
            closePopup();
        }
    }

    return QFrame::eventFilter(obj, event);
}

void StickyNotePopup::applyColorTheme(QColor annotColor)
{
    // Use annotation color for title bar, lighter tint for body
    int r = annotColor.red(), g = annotColor.green(), b = annotColor.blue();

    // Title bar: full color
    titleBar_->setStyleSheet(
        QString("QLabel { background-color: rgb(%1, %2, %3); color: black; }")
            .arg(r).arg(g).arg(b));

    // Body: lighter tint (blend with white — more faded when read-only)
    int blendPct = readOnly_ ? 85 : 70;
    int lr = r + (255 - r) * blendPct / 100;
    int lg = g + (255 - g) * blendPct / 100;
    int lb = b + (255 - b) * blendPct / 100;

    textEdit_->setStyleSheet(
        QString("QTextEdit { background-color: rgb(%1, %2, %3); }")
            .arg(lr).arg(lg).arg(lb));

    setStyleSheet(
        QString("QFrame#stickyNotePopup { background-color: rgb(%1, %2, %3); }")
            .arg(lr).arg(lg).arg(lb));
}

void StickyNotePopup::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && event->position().y() < TITLE_BAR_HEIGHT) {
        dragging_ = true;
        dragOffset_ = event->globalPosition().toPoint() - frameGeometry().topLeft();
        event->accept();
        return;
    }
    QFrame::mousePressEvent(event);
}

void StickyNotePopup::mouseMoveEvent(QMouseEvent* event)
{
    if (dragging_) {
        // Top-level window: move() takes GLOBAL screen coordinates.
        // Clamp to the popup's current screen geometry so the drag
        // can't push the widget off-screen.
        QPoint newPos = event->globalPosition().toPoint() - dragOffset_;
        QRect screenGeom;
        if (QScreen* scr = screen()) {
            screenGeom = scr->availableGeometry();
        } else if (auto* primary = QGuiApplication::primaryScreen()) {
            screenGeom = primary->availableGeometry();
        }
        if (!screenGeom.isNull()) {
            newPos.setX(std::max(screenGeom.left(),
                                 std::min(newPos.x(), screenGeom.right() - width())));
            newPos.setY(std::max(screenGeom.top(),
                                 std::min(newPos.y(), screenGeom.bottom() - height())));
        }
        move(newPos);
        event->accept();
        return;
    }
    QFrame::mouseMoveEvent(event);
}

void StickyNotePopup::mouseReleaseEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && dragging_) {
        dragging_ = false;
        event->accept();
        return;
    }
    QFrame::mouseReleaseEvent(event);
}

void StickyNotePopup::positionPopup(QPoint anchor)
{
    // Pipeline b3f19d72 phase-6 (P8-BUG-1): anchor is in GLOBAL screen
    // coordinates and the popup is a top-level window, so move() takes
    // global coords directly.  Use the target screen's available
    // geometry for flip / clamp instead of the (no-longer-present)
    // parent rect.
    QRect screenGeom;
    if (QScreen* scr = QGuiApplication::screenAt(anchor)) {
        screenGeom = scr->availableGeometry();
    } else if (auto* primary = QGuiApplication::primaryScreen()) {
        screenGeom = primary->availableGeometry();
    }
    if (screenGeom.isNull()) {
        move(anchor);
        return;
    }

    int x = anchor.x();
    int y = anchor.y();

    // Flip horizontally if near right edge of screen.
    if (x + POPUP_WIDTH > screenGeom.right()) {
        x = anchor.x() - POPUP_WIDTH;
    }

    // Flip vertically if near bottom edge of screen.
    if (y + POPUP_HEIGHT > screenGeom.bottom()) {
        y = anchor.y() - POPUP_HEIGHT;
    }

    // Clamp to screen bounds.
    x = std::max(screenGeom.left(), std::min(x, screenGeom.right() - POPUP_WIDTH));
    y = std::max(screenGeom.top(), std::min(y, screenGeom.bottom() - POPUP_HEIGHT));

    move(x, y);
}

} // namespace pdfe::ui
