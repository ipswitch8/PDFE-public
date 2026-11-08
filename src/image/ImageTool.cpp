#include "image/ImageTool.h"
#include "image/ImageManager.h"
#include "core/Page.h"
#include "core/Document.h"
#include <QMouseEvent>
#include <QKeyEvent>
#include <QPainter>
#include <QPen>
#include <QBrush>
#include <algorithm>
#include <cmath>

namespace pdfe::image {

namespace {

/// Convert handle size from pixels to PDF coordinates.
constexpr float handleSizePixels = 8.0f;
constexpr float handleTolerancePixels = 12.0f;

/// Minimum body-drag distance (PDF points) that counts as a move rather than a
/// click-to-select. Below this a body-drag release commits nothing (GitHub #340).
constexpr float kMinMoveDistance = 1.0f;

/// Check if a point is within tolerance of a rectangle corner/edge.
bool isNearPoint(QPointF p, QPointF target, float tolerance) {
    return (p - target).manhattanLength() < tolerance;
}

/// Get the center point of a rectangle.
QPointF centerOf(const QRectF& rect) {
    return QPointF(rect.center().x(), rect.center().y());
}

/// Rotate point @p p about @p center by @p radians (CCW-positive, PDF y-up
/// convention). Used to map a page-space mouse point into an image's un-rotated
/// local frame (pass -rotationRadians) for hit-testing (GitHub #342).
QPointF rotateAbout(QPointF p, QPointF center, float radians) {
    const float c = std::cos(radians);
    const float s = std::sin(radians);
    const QPointF d = p - center;
    return center + QPointF(d.x() * c - d.y() * s, d.x() * s + d.y() * c);
}

} // anonymous namespace

class ImageTool::Impl {
public:
    ImageManager* manager_ = nullptr;
    pdfe::core::Page* page_ = nullptr;
    pdfe::core::Document* doc_ = nullptr;

    std::vector<PageImageInfo> images_;
    int selectedIndex_ = -1;
    HandlePosition activeHandle_ = HandlePosition::None;
    bool dragging_ = false;
    QPointF dragStart_;
    QRectF originalRect_;
    float pendingRotation_ = 0.0f;  ///< Live rotation angle during drag (degrees)

    explicit Impl(ImageManager* manager)
        : manager_(manager)
    {}

    void refreshImages() {
        images_.clear();
        selectedIndex_ = -1;
        activeHandle_ = HandlePosition::None;
        dragging_ = false;

        if (!page_ || !doc_ || !manager_) {
            return;
        }

        images_ = manager_->enumerateImages(page_, doc_);
    }

    /// Get handle rectangle in PDF coordinates.
    QRectF getHandleRect(const QRectF& bbox, HandlePosition pos, float scale) const {
        const float handleSize = handleSizePixels / scale;
        const float half = handleSize / 2.0f;

        QPointF center;
        switch (pos) {
        case HandlePosition::TopLeft:
            center = bbox.topLeft();
            break;
        case HandlePosition::TopCenter:
            center = QPointF(bbox.center().x(), bbox.top());
            break;
        case HandlePosition::TopRight:
            center = bbox.topRight();
            break;
        case HandlePosition::MiddleLeft:
            center = QPointF(bbox.left(), bbox.center().y());
            break;
        case HandlePosition::MiddleRight:
            center = QPointF(bbox.right(), bbox.center().y());
            break;
        case HandlePosition::BottomLeft:
            center = bbox.bottomLeft();
            break;
        case HandlePosition::BottomCenter:
            center = QPointF(bbox.center().x(), bbox.bottom());
            break;
        case HandlePosition::BottomRight:
            center = bbox.bottomRight();
            break;
        case HandlePosition::Rotation:
            // Position above top center
            center = QPointF(bbox.center().x(), bbox.top() - 20.0f / scale);
            break;
        default:
            return QRectF();
        }

        return QRectF(center.x() - half, center.y() - half, handleSize, handleSize);
    }

    /// Get rotation handle circle center in PDF coordinates.
    QPointF getRotationHandleCenter(const QRectF& bbox, float scale) const {
        return QPointF(bbox.center().x(), bbox.top() - 20.0f / scale);
    }

    /// Hit-test for a handle at the given position.
    ///
    /// @p bbox is the UN-rotated, axis-aligned local rect; @p rotationRadians is
    /// the placement's rotation. The handle rects are defined in the un-rotated
    /// local frame, so the page-space @p pos is inverse-rotated by -rotationRadians
    /// about the rect centre before comparison (GitHub #342). With rotationRadians
    /// == 0 this is a no-op and behaviour matches the former axis-aligned test.
    HandlePosition hitTestHandle(const QRectF& bbox, QPointF pos, float scale,
                                 float rotationRadians) const {
        const float tolerance = handleTolerancePixels / scale;

        if (rotationRadians != 0.0f) {
            pos = rotateAbout(pos, bbox.center(), -rotationRadians);
        }

        // Test all handles
        for (auto handle : {
            HandlePosition::TopLeft, HandlePosition::TopCenter, HandlePosition::TopRight,
            HandlePosition::MiddleLeft, HandlePosition::MiddleRight,
            HandlePosition::BottomLeft, HandlePosition::BottomCenter, HandlePosition::BottomRight,
            HandlePosition::Rotation
        }) {
            QRectF handleRect = getHandleRect(bbox, handle, scale);
            if (handleRect.adjusted(-tolerance, -tolerance, tolerance, tolerance).contains(pos)) {
                return handle;
            }
        }

        return HandlePosition::None;
    }

    /// Hit-test images at the given position (reverse order for z-order).
    ///
    /// Each image's boundingBox is its un-rotated local rect; inverse-rotate the
    /// page-space @p pos by the image's own -rotationRadians about its centre
    /// before the contains() test so a rotated image is still hittable (GitHub
    /// #342). Unchanged for axis-aligned images (rotationRadians == 0).
    int hitTestImage(QPointF pos) const {
        for (int i = static_cast<int>(images_.size()) - 1; i >= 0; --i) {
            const auto& img = images_[i];
            QPointF local = pos;
            if (img.rotationRadians != 0.0f) {
                local = rotateAbout(pos, img.boundingBox.center(), -img.rotationRadians);
            }
            if (img.boundingBox.contains(local)) {
                return i;
            }
        }
        return -1;
    }

    /// Compute new bounding box based on handle drag.
    QRectF computeNewRect(HandlePosition handle, QPointF currentPos) const {
        QRectF newRect = originalRect_;
        const QPointF delta = currentPos - dragStart_;

        switch (handle) {
        case HandlePosition::TopLeft:
            newRect.setTopLeft(originalRect_.topLeft() + delta);
            break;
        case HandlePosition::TopCenter:
            newRect.setTop(originalRect_.top() + delta.y());
            break;
        case HandlePosition::TopRight:
            newRect.setTopRight(originalRect_.topRight() + delta);
            break;
        case HandlePosition::MiddleLeft:
            newRect.setLeft(originalRect_.left() + delta.x());
            break;
        case HandlePosition::MiddleRight:
            newRect.setRight(originalRect_.right() + delta.x());
            break;
        case HandlePosition::BottomLeft:
            newRect.setBottomLeft(originalRect_.bottomLeft() + delta);
            break;
        case HandlePosition::BottomCenter:
            newRect.setBottom(originalRect_.bottom() + delta.y());
            break;
        case HandlePosition::BottomRight:
            newRect.setBottomRight(originalRect_.bottomRight() + delta);
            break;
        default:
            break;
        }

        // Ensure minimum size
        if (newRect.width() < 10.0f) {
            if (handle == HandlePosition::TopLeft || handle == HandlePosition::MiddleLeft || handle == HandlePosition::BottomLeft) {
                newRect.setLeft(newRect.right() - 10.0f);
            } else {
                newRect.setRight(newRect.left() + 10.0f);
            }
        }
        if (newRect.height() < 10.0f) {
            if (handle == HandlePosition::TopLeft || handle == HandlePosition::TopCenter || handle == HandlePosition::TopRight) {
                newRect.setTop(newRect.bottom() - 10.0f);
            } else {
                newRect.setBottom(newRect.top() + 10.0f);
            }
        }

        return newRect;
    }

    /// Get cursor for a given handle position.
    Qt::CursorShape getCursorForHandle(HandlePosition handle) const {
        switch (handle) {
        case HandlePosition::TopLeft:
        case HandlePosition::BottomRight:
            return Qt::SizeFDiagCursor;
        case HandlePosition::TopRight:
        case HandlePosition::BottomLeft:
            return Qt::SizeBDiagCursor;
        case HandlePosition::TopCenter:
        case HandlePosition::BottomCenter:
            return Qt::SizeVerCursor;
        case HandlePosition::MiddleLeft:
        case HandlePosition::MiddleRight:
            return Qt::SizeHorCursor;
        case HandlePosition::Rotation:
            return Qt::CrossCursor;
        default:
            return Qt::ArrowCursor;
        }
    }
};

ImageTool::ImageTool(ImageManager* manager, QObject* parent)
    : QObject(parent)
    , impl_(std::make_unique<Impl>(manager))
{
    setObjectName("imageTool");
}

ImageTool::~ImageTool() = default;

void ImageTool::setPage(pdfe::core::Page* page, pdfe::core::Document* doc) {
    impl_->page_ = page;
    impl_->doc_ = doc;
    impl_->refreshImages();
}

void ImageTool::clearPage() {
    impl_->page_ = nullptr;
    impl_->doc_ = nullptr;
    impl_->images_.clear();
    impl_->selectedIndex_ = -1;
    impl_->activeHandle_ = HandlePosition::None;
    impl_->dragging_ = false;
}

void ImageTool::mousePressEvent(QMouseEvent* event, QPointF pdfPos, float scale) {
    if (!event || !impl_->page_ || !impl_->doc_) {
        return;
    }

    // Check if clicking on a handle of the selected image
    if (impl_->selectedIndex_ >= 0 && impl_->selectedIndex_ < static_cast<int>(impl_->images_.size())) {
        const auto& selectedImg = impl_->images_[impl_->selectedIndex_];
        HandlePosition handle = impl_->hitTestHandle(
            selectedImg.boundingBox, pdfPos, scale, selectedImg.rotationRadians);

        if (handle != HandlePosition::None) {
            // Start handle drag
            impl_->activeHandle_ = handle;
            impl_->dragging_ = true;
            impl_->dragStart_ = pdfPos;
            impl_->originalRect_ = selectedImg.boundingBox;
            return;
        }
    }

    // Hit-test images
    int hitIndex = impl_->hitTestImage(pdfPos);

    if (hitIndex >= 0) {
        // Select this image
        if (hitIndex != impl_->selectedIndex_) {
            impl_->selectedIndex_ = hitIndex;
            emit imageSelected(impl_->images_[hitIndex]);
            emit repaintRequested();
        }

        // Start drag on image body (for move operation)
        impl_->dragging_ = true;
        impl_->dragStart_ = pdfPos;
        impl_->originalRect_ = impl_->images_[hitIndex].boundingBox;
        impl_->activeHandle_ = HandlePosition::None;
    } else {
        // Clicked outside any image - deselect
        if (impl_->selectedIndex_ >= 0) {
            deselect();
        }
    }
}

void ImageTool::mouseMoveEvent(QMouseEvent* event, QPointF pdfPos, float scale) {
    if (!event || !impl_->page_ || !impl_->doc_) {
        return;
    }

    if (impl_->dragging_ && impl_->selectedIndex_ >= 0) {
        if (impl_->activeHandle_ != HandlePosition::None && impl_->activeHandle_ != HandlePosition::Rotation) {
            // Resizing via handle
            QRectF newRect = impl_->computeNewRect(impl_->activeHandle_, pdfPos);
            impl_->images_[impl_->selectedIndex_].boundingBox = newRect;
            impl_->images_[impl_->selectedIndex_].displayWidth = newRect.width();
            impl_->images_[impl_->selectedIndex_].displayHeight = newRect.height();
            emit repaintRequested();
        } else if (impl_->activeHandle_ == HandlePosition::Rotation) {
            // Compute live rotation angle from drag start to current position
            QPointF center = centerOf(impl_->originalRect_);
            QPointF start = impl_->dragStart_ - center;
            QPointF end = pdfPos - center;

            float startAngle = std::atan2(start.y(), start.x()) * 180.0f / 3.14159265f;
            float endAngle = std::atan2(end.y(), end.x()) * 180.0f / 3.14159265f;
            // GitHub #341: atan2 in PDF space is CCW-positive, but the rotate
            // command and the toolbar both treat POSITIVE as clockwise. Negate
            // the CCW delta so a visually-clockwise drag yields a positive angle.
            float deltaAngle = startAngle - endAngle;

            // Snap to 90-degree increments for preview
            impl_->pendingRotation_ = std::round(deltaAngle / 90.0f) * 90.0f;
            emit repaintRequested();
        } else {
            // Moving the image
            QPointF delta = pdfPos - impl_->dragStart_;
            QRectF newRect = impl_->originalRect_.translated(delta);
            impl_->images_[impl_->selectedIndex_].boundingBox = newRect;
            emit repaintRequested();
        }
    }
}

void ImageTool::mouseReleaseEvent(QMouseEvent* event, QPointF pdfPos, float scale) {
    if (!event || !impl_->page_ || !impl_->doc_) {
        return;
    }

    if (impl_->dragging_ && impl_->selectedIndex_ >= 0) {
        const auto& img = impl_->images_[impl_->selectedIndex_];

        if (impl_->activeHandle_ != HandlePosition::None && impl_->activeHandle_ != HandlePosition::Rotation) {
            // Emit resize request
            emit resizeRequested(
                img.xobjectName,
                img.boundingBox.width(),
                img.boundingBox.height(),
                img.boundingBox.x(),
                img.boundingBox.y()
            );
        } else if (impl_->activeHandle_ == HandlePosition::Rotation) {
            // Compute rotation angle from drag
            QPointF center = centerOf(impl_->originalRect_);
            QPointF start = impl_->dragStart_ - center;
            QPointF end = pdfPos - center;

            float startAngle = std::atan2(start.y(), start.x()) * 180.0f / 3.14159265f;
            float endAngle = std::atan2(end.y(), end.x()) * 180.0f / 3.14159265f;
            // GitHub #341: negate the CCW atan2 delta to the clockwise-positive
            // convention the command and toolbar use (matches mouseMoveEvent).
            float deltaAngle = startAngle - endAngle;

            // Snap to 90-degree increments
            deltaAngle = std::round(deltaAngle / 90.0f) * 90.0f;

            if (std::abs(deltaAngle) > 1.0f) {
                emit rotateRequested(img.xobjectName, deltaAngle);
            }
        } else {
            // GitHub #340: body drag (no handle). mouseMoveEvent has already
            // translated the in-memory boundingBox; commit the move by emitting a
            // position-carrying command. ResizeImageCommand re-places the image at
            // (x,y) with the given w,h, so emit the UNCHANGED size and the new
            // origin. Guard a ~0 delta so a plain click-to-select never commits a
            // no-op move (preserving the selection-only press).
            const QPointF delta = pdfPos - impl_->dragStart_;
            if (std::hypot(delta.x(), delta.y()) > kMinMoveDistance) {
                emit resizeRequested(
                    img.xobjectName,
                    img.boundingBox.width(),
                    img.boundingBox.height(),
                    img.boundingBox.x(),
                    img.boundingBox.y()
                );
            }
        }
    }

    impl_->dragging_ = false;
    impl_->activeHandle_ = HandlePosition::None;
    impl_->pendingRotation_ = 0.0f;
}

void ImageTool::keyPressEvent(QKeyEvent* event) {
    if (!event) {
        return;
    }

    if (event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) {
        if (impl_->selectedIndex_ >= 0) {
            emit removeRequested(impl_->selectedIndex_);
        }
    } else if (event->key() == Qt::Key_Escape) {
        deselect();
    }
}

void ImageTool::paint(QPainter& painter, float scale) {
    if (impl_->selectedIndex_ < 0 || impl_->selectedIndex_ >= static_cast<int>(impl_->images_.size())) {
        return;
    }

    const auto& img = impl_->images_[impl_->selectedIndex_];
    const QRectF& bbox = img.boundingBox;

    // GitHub #342: bbox is the UN-rotated local rect. Draw the selection frame
    // and handles in that local frame but under a painter rotation about the
    // image centre so they land on the visually-rotated corners — matching the
    // inverse-rotation the hit-test applies. baseDeg maps the CCW-positive PDF
    // rotation to this painter's degrees (same sign relationship the live-preview
    // outline below uses). No-op when rotationRadians == 0.
    const float baseDeg = -img.rotationRadians * 180.0f / 3.14159265f;
    const QPointF imgCenter = bbox.center();

    painter.save();
    if (img.rotationRadians != 0.0f) {
        painter.translate(imgCenter);
        painter.rotate(baseDeg);
        painter.translate(-imgCenter);
    }

    // Draw selection rectangle
    QPen selectionPen(Qt::blue, 1.0f / scale, Qt::DashLine);
    painter.setPen(selectionPen);
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(bbox);

    // Draw resize handles
    QPen handlePen(Qt::blue, 1.0f / scale);
    QBrush handleBrush(Qt::white);
    painter.setPen(handlePen);
    painter.setBrush(handleBrush);

    for (auto handle : {
        HandlePosition::TopLeft, HandlePosition::TopCenter, HandlePosition::TopRight,
        HandlePosition::MiddleLeft, HandlePosition::MiddleRight,
        HandlePosition::BottomLeft, HandlePosition::BottomCenter, HandlePosition::BottomRight
    }) {
        QRectF handleRect = impl_->getHandleRect(bbox, handle, scale);
        painter.drawRect(handleRect);
    }

    // Draw rotation handle (circle)
    QPointF rotationCenter = impl_->getRotationHandleCenter(bbox, scale);
    const float rotationRadius = handleSizePixels / (2.0f * scale);
    painter.drawEllipse(rotationCenter, rotationRadius, rotationRadius);
    painter.restore();

    // Draw rotation angle indicator during drag
    if (impl_->dragging_ && impl_->activeHandle_ == HandlePosition::Rotation
        && std::abs(impl_->pendingRotation_) > 0.5f) {
        painter.save();
        QPointF center = bbox.center();

        // Draw angle text near the rotation handle
        QFont font = painter.font();
        font.setPointSizeF(10.0 / scale);
        painter.setFont(font);
        painter.setPen(QPen(Qt::blue, 1.0f / scale));
        QString angleText = QString::fromLatin1("%1%2").arg(static_cast<int>(impl_->pendingRotation_)).arg(QChar(0x00B0));
        painter.drawText(rotationCenter + QPointF(rotationRadius * 2, 0), angleText);

        // Draw a rotated outline preview. GitHub #341: pendingRotation_ now carries
        // the clockwise-positive convention (same as the committed command), so the
        // painter rotates by +pendingRotation_ here (the former -pendingRotation_
        // negated the raw CCW delta). Composing with baseDeg previews base + pending
        // so the dotted outline matches what the commit will produce.
        QPen previewPen(Qt::blue, 1.0f / scale, Qt::DotLine);
        painter.setPen(previewPen);
        painter.setBrush(Qt::NoBrush);
        painter.translate(center);
        painter.rotate(baseDeg + impl_->pendingRotation_);
        painter.translate(-center);
        painter.drawRect(bbox);
        painter.restore();
    }
}

QCursor ImageTool::cursorAt(QPointF pdfPos, float scale) const {
    if (impl_->selectedIndex_ >= 0 && impl_->selectedIndex_ < static_cast<int>(impl_->images_.size())) {
        const auto& selectedImg = impl_->images_[impl_->selectedIndex_];
        HandlePosition handle = impl_->hitTestHandle(
            selectedImg.boundingBox, pdfPos, scale, selectedImg.rotationRadians);

        if (handle != HandlePosition::None) {
            return QCursor(impl_->getCursorForHandle(handle));
        }

        QPointF bodyPos = pdfPos;
        if (selectedImg.rotationRadians != 0.0f) {
            bodyPos = rotateAbout(pdfPos, selectedImg.boundingBox.center(),
                                  -selectedImg.rotationRadians);
        }
        if (selectedImg.boundingBox.contains(bodyPos)) {
            return QCursor(Qt::OpenHandCursor);
        }
    }

    // Check if hovering over any image
    int hitIndex = impl_->hitTestImage(pdfPos);
    if (hitIndex >= 0) {
        return QCursor(Qt::PointingHandCursor);
    }

    return QCursor(Qt::ArrowCursor);
}

bool ImageTool::hasSelection() const {
    return impl_->selectedIndex_ >= 0 && impl_->selectedIndex_ < static_cast<int>(impl_->images_.size());
}

const PageImageInfo& ImageTool::selectedImage() const {
    static const PageImageInfo emptyInfo;
    if (hasSelection()) {
        return impl_->images_[impl_->selectedIndex_];
    }
    return emptyInfo;
}

bool ImageTool::selectImage(const std::string& xobjectName) {
    if (xobjectName.empty()) {
        return false;
    }
    for (int i = 0; i < static_cast<int>(impl_->images_.size()); ++i) {
        if (impl_->images_[i].xobjectName == xobjectName) {
            impl_->selectedIndex_ = i;
            impl_->activeHandle_ = HandlePosition::None;
            impl_->dragging_ = false;
            // Same side effects a click-select produces: handles paint and the
            // image-toolbar actions enable off imageSelected (GitHub #329).
            emit imageSelected(impl_->images_[i]);
            emit repaintRequested();
            return true;
        }
    }
    return false;
}

bool ImageTool::selectLast() {
    if (impl_->images_.empty()) {
        return false;
    }
    const int last = static_cast<int>(impl_->images_.size()) - 1;
    impl_->selectedIndex_ = last;
    impl_->activeHandle_ = HandlePosition::None;
    impl_->dragging_ = false;
    emit imageSelected(impl_->images_[last]);
    emit repaintRequested();
    return true;
}

void ImageTool::deselect() {
    if (impl_->selectedIndex_ >= 0) {
        impl_->selectedIndex_ = -1;
        impl_->activeHandle_ = HandlePosition::None;
        impl_->dragging_ = false;
        emit imageDeselected();
        emit repaintRequested();
    }
}

HandlePosition ImageTool::hitTestHandleForTest(QPointF pdfPos, float scale) const {
    if (impl_->selectedIndex_ < 0
        || impl_->selectedIndex_ >= static_cast<int>(impl_->images_.size())) {
        return HandlePosition::None;
    }
    const auto& selectedImg = impl_->images_[impl_->selectedIndex_];
    return impl_->hitTestHandle(
        selectedImg.boundingBox, pdfPos, scale, selectedImg.rotationRadians);
}

void ImageTool::setImagesForTest(std::vector<PageImageInfo> images, int selectIndex) {
    impl_->images_ = std::move(images);
    impl_->activeHandle_ = HandlePosition::None;
    impl_->dragging_ = false;
    if (selectIndex >= 0 && selectIndex < static_cast<int>(impl_->images_.size())) {
        impl_->selectedIndex_ = selectIndex;
    } else {
        impl_->selectedIndex_ = -1;
    }
}

} // namespace pdfe::image
