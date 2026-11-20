#include "image/ImageTool.h"
#include "image/ImageManager.h"
#include "image/ImagePlacement.h"
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

/// Rotate point @p p about @p center by @p radians — plain math-convention
/// rotation matrix. The tool operates in fitz Y-down page space (GitHub #373),
/// where a POSITIVE math angle is visually CLOCKWISE. A placement whose
/// enumerated rotationRadians is θ (CCW-positive in Y-up user space, i.e.
/// visually CCW) maps local→page in this frame as a rotation by -θ, so the
/// page→local inverse used for hit-testing is a rotation by +θ (GitHub
/// #342/#376 — the sign here flipped when the bboxes moved to Y-down).
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
    /// @p bbox is the UN-rotated, axis-aligned local rect (fitz Y-down);
    /// @p rotationRadians is the placement's rotation (CCW-positive in Y-up
    /// user space). The handle rects are defined in the un-rotated local
    /// frame; in this Y-down frame the placement is a math rotation by
    /// -rotationRadians, so the page-space @p pos is inverse-rotated by
    /// +rotationRadians about the rect centre before comparison (GitHub
    /// #342/#373/#376). With rotationRadians == 0 this is a no-op and
    /// behaviour matches the former axis-aligned test.
    HandlePosition hitTestHandle(const QRectF& bbox, QPointF pos, float scale,
                                 float rotationRadians) const {
        const float tolerance = handleTolerancePixels / scale;

        if (rotationRadians != 0.0f) {
            pos = rotateAbout(pos, bbox.center(), rotationRadians);
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
    /// Each image's boundingBox is its un-rotated local rect (fitz Y-down);
    /// inverse-rotate the page-space @p pos by the image's own
    /// +rotationRadians about its centre (the Y-down inverse of the placement
    /// rotation — see rotateAbout) before the contains() test so a rotated
    /// image is still hittable (GitHub #342/#376). Unchanged for axis-aligned
    /// images (rotationRadians == 0).
    int hitTestImage(QPointF pos) const {
        for (int i = static_cast<int>(images_.size()) - 1; i >= 0; --i) {
            const auto& img = images_[i];
            QPointF local = pos;
            if (img.rotationRadians != 0.0f) {
                local = rotateAbout(pos, img.boundingBox.center(), img.rotationRadians);
            }
            if (img.boundingBox.contains(local)) {
                return i;
            }
        }
        return -1;
    }

    /// Compute new bounding box based on handle drag.
    ///
    /// GitHub #433: the handles belong to the image's OWN frame. originalRect_
    /// is the upright rect in that frame (centred on the visible centre), and
    /// the page shows it turned by -rotationRadians about its centre. The
    /// page-space drag is therefore taken into the image's frame (+rotation,
    /// the same inverse the hit-test uses), the edge is moved there, and the
    /// resulting centre shift is turned back onto the page, so the edge
    /// opposite the handle stays where it is on screen. With rotationRadians
    /// == 0 every step is the identity and the rect is what it always was.
    QRectF computeNewRect(HandlePosition handle, QPointF currentPos,
                          float rotationRadians) const {
        QRectF newRect = originalRect_;
        QPointF delta = currentPos - dragStart_;
        if (rotationRadians != 0.0f) {
            delta = rotateAbout(delta, QPointF(0.0, 0.0), rotationRadians);
        }

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

        if (rotationRadians != 0.0f) {
            // The centre moved in the image's frame; move it on the page.
            const QPointF localShift = newRect.center() - originalRect_.center();
            const QPointF pageShift = rotateAbout(localShift, QPointF(0.0, 0.0), -rotationRadians);
            newRect.moveCenter(originalRect_.center() + pageShift);
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
            QRectF newRect = impl_->computeNewRect(
                impl_->activeHandle_, pdfPos,
                impl_->images_[impl_->selectedIndex_].rotationRadians);
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
            // GitHub #376 (re-derives the #341 sign for genuine Y-down input):
            // pdfPos is fitz Y-down, where atan2 grows VISUALLY CLOCKWISE —
            // already the positive-is-clockwise convention the rotate command
            // and toolbar use. The raw delta (end - start) is therefore the
            // committed angle with NO negation; the old (start - end) negation
            // was derived from the Y-up seam coordinates the pre-#373 tests
            // fed in, and inverted the direction for real mouse input.
            float deltaAngle = endAngle - startAngle;

            // GitHub #391(a): carry the REAL (arbitrary) drag delta — no
            // 90-degree snap. paint() previews baseDeg + pendingRotation_, so
            // the dotted outline follows the true pointer angle.
            impl_->pendingRotation_ = deltaAngle;
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

    if (impl_->dragging_ && impl_->selectedIndex_ >= 0 &&
        impl_->images_[impl_->selectedIndex_].inFormXObject) {
        // docs/image-placement-model.md (GitHub #427): a placement inside a
        // Form XObject is selectable but not editable, so a drag commits
        // nothing; put the overlay back where the page has it.
        impl_->images_[impl_->selectedIndex_].boundingBox = impl_->originalRect_;
        impl_->dragging_ = false;
        impl_->activeHandle_ = HandlePosition::None;
        impl_->pendingRotation_ = 0.0f;
        emit repaintRequested();
        return;
    }

    if (impl_->dragging_ && impl_->selectedIndex_ >= 0) {
        const auto& img = impl_->images_[impl_->selectedIndex_];

        if (impl_->activeHandle_ != HandlePosition::None && impl_->activeHandle_ != HandlePosition::Rotation) {
            // Emit resize request. GitHub #416: carry the current orientation,
            // or the editor rebuilds an upright placement and a rotated image
            // snaps back to 0 while the overlay still shows it rotated. The name
            // is a COPY: handlers re-enumerate this tool, which frees `img`.
            const std::string name = img.xobjectName;
            // GitHub #420: the overlay is the VISIBLE region; the editor takes
            // the whole placement (identical for an uncropped image).
            const QRectF full = placementRectForVisibleRect(
                img, impl_->originalRect_, img.boundingBox);
            emit resizeRequested(
                name,
                full.width(),
                full.height(),
                full.x(),
                full.y(),
                visualClockwiseDegrees(img)
            );
        } else if (impl_->activeHandle_ == HandlePosition::Rotation) {
            // Compute rotation angle from drag
            QPointF center = centerOf(impl_->originalRect_);
            QPointF start = impl_->dragStart_ - center;
            QPointF end = pdfPos - center;

            float startAngle = std::atan2(start.y(), start.x()) * 180.0f / 3.14159265f;
            float endAngle = std::atan2(end.y(), end.x()) * 180.0f / 3.14159265f;
            // GitHub #376: Y-down atan2 is already clockwise-positive, so the
            // raw delta carries the command/toolbar convention directly
            // (matches mouseMoveEvent; see the derivation there).
            float deltaAngle = endAngle - startAngle;

            // GitHub #391(a): no 90-degree snap — the handle must emit the
            // real arbitrary drag angle.
            //
            // GitHub #391(b): ImageContentEditor::rotateImage treats its angle
            // argument as an ABSOLUTE orientation from upright (it removes the
            // old placement and rebuilds at exactly that angle; it never reads
            // the current rotation). The drag only produces a DELTA about the
            // handle, so emit the new absolute orientation = current + delta.
            // The current visual-clockwise angle is what paint() uses as its
            // base: baseDeg = -rotationRadians (deg). On the first drag from
            // upright rotationRadians == 0, so absolute == delta — preserving
            // the clockwise-positive convention #341/#376 established. The
            // guard is on the raw DELTA so a near-zero drag (a click-select on
            // the handle) still commits nothing.
            if (std::abs(deltaAngle) > 1.0f) {
                const float currentDeg = visualClockwiseDegrees(img);
                const float absoluteAngle = currentDeg + deltaAngle;
                // A copy, for the same reason as the resize emit above.
                const std::string name = img.xobjectName;
                emit rotateRequested(name, absoluteAngle);
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
                // GitHub #416: a move must keep the image's rotation. The name
                // is a copy: handlers re-enumerate this tool, which frees `img`.
                const std::string name = img.xobjectName;
                const QRectF full = placementRectForVisibleRect(
                    img, impl_->originalRect_, img.boundingBox);
                emit resizeRequested(
                    name,
                    full.width(),
                    full.height(),
                    full.x(),
                    full.y(),
                    visualClockwiseDegrees(img)
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

    // PAINTER CONTRACT (GitHub #373): the caller (PdfCanvas) hands this a
    // painter already transformed into fitz page space — translated to the
    // page's top-left and scaled by the zoom — so bbox/handle rects (fitz
    // Y-down points) draw directly, exactly like the annotation-tool overlays.
    // No scale(1,-1) flip is involved (it would mirror the angle text below).
    //
    // GitHub #342: bbox is the UN-rotated local rect. Draw the selection frame
    // and handles in that local frame but under a painter rotation about the
    // image centre so they land on the visually-rotated corners — matching the
    // inverse-rotation the hit-test applies. QPainter::rotate is visually
    // clockwise-positive in this Y-down frame, while rotationRadians is
    // visually CCW-positive (Y-up user convention), hence baseDeg negates it
    // (same sign relationship the live-preview outline below uses). No-op
    // when rotationRadians == 0.
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
        // GitHub #391: show the RESULTING absolute orientation (base + the live
        // unsnapped delta), the angle the commit will apply — not the bare
        // per-drag delta, and never a 90-only value.
        const int shownAngle = static_cast<int>(baseDeg + impl_->pendingRotation_);
        QString angleText = QString::fromLatin1("%1%2").arg(shownAngle).arg(QChar(0x00B0));
        painter.drawText(rotationCenter + QPointF(rotationRadius * 2, 0), angleText);

        // Draw a rotated outline preview. pendingRotation_ carries the
        // clockwise-positive convention (same as the committed command — #341,
        // re-derived for Y-down input in #376), and QPainter::rotate is also
        // clockwise-positive, so the painter rotates by +pendingRotation_.
        // Composing with baseDeg previews base + pending so the dotted outline
        // matches what the commit will produce.
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
            // Y-down inverse of the placement rotation (see rotateAbout).
            bodyPos = rotateAbout(pdfPos, selectedImg.boundingBox.center(),
                                  selectedImg.rotationRadians);
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

bool ImageTool::selectPlacement(const std::string& xobjectName, int occurrence) {
    if (xobjectName.empty()) {
        return false;
    }
    for (int i = 0; i < static_cast<int>(impl_->images_.size()); ++i) {
        const auto& img = impl_->images_[i];
        if (img.xobjectName == xobjectName && img.occurrence == occurrence) {
            impl_->selectedIndex_ = i;
            impl_->activeHandle_ = HandlePosition::None;
            impl_->dragging_ = false;
            emit imageSelected(impl_->images_[i]);
            emit repaintRequested();
            return true;
        }
    }
    return false;
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

float visualClockwiseDegrees(const PageImageInfo& info) {
    return -info.rotationRadians * 180.0f / 3.14159265f;
}

QRectF placementRectForVisibleRect(const PageImageInfo& info,
                                   const QRectF& originalVisible,
                                   const QRectF& newVisible) {
    if (!info.cropped || originalVisible.width() <= 0.0 || originalVisible.height() <= 0.0)
        return newVisible;
    const double sx = newVisible.width() / originalVisible.width();
    const double sy = newVisible.height() / originalVisible.height();
    // Offset from the visible centre to the image's centre, taken into the
    // upright frame (undo the on-page turn of -rotationRadians), scaled with
    // the visible rect, and turned back.
    const double t = -static_cast<double>(info.rotationRadians);
    const QPointF origin(0.0, 0.0);
    const QPointF offset = info.placementBox.center() - originalVisible.center();
    const QPointF upright = placement::rotateAbout(offset, origin, -t);
    const QPointF scaled = placement::rotateAbout(
        QPointF(upright.x() * sx, upright.y() * sy), origin, t);
    const QPointF centre = newVisible.center() + scaled;
    const double w = info.placementBox.width() * sx;
    const double h = info.placementBox.height() * sy;
    return QRectF(centre.x() - w / 2.0, centre.y() - h / 2.0, w, h);
}

bool resyncImageToolSelection(ImageTool& tool, pdfe::core::Page* page,
                              pdfe::core::Document* doc, const std::string& xobjectName) {
    // Copy BEFORE setPage. Callers are signal handlers whose xobjectName is a
    // reference straight into the tool's image list (the tool emits
    // img.xobjectName from mouseReleaseEvent), and setPage clears and refills
    // that list, so the reference dangles. selectImage then compared against
    // freed memory and the selection was silently lost after a drag
    // (GitHub #416).
    const std::string name = xobjectName;
    tool.setPage(page, doc);
    return tool.selectImage(name);
}

bool resyncImageToolPlacement(ImageTool& tool, pdfe::core::Page* page,
                              pdfe::core::Document* doc, const std::string& xobjectName,
                              int occurrence) {
    // A copy, for the reason given in resyncImageToolSelection.
    const std::string name = xobjectName;
    const bool hadSelection = tool.hasSelection();
    tool.setPage(page, doc);
    if (!name.empty() && tool.selectPlacement(name, occurrence)) {
        return true;
    }
    // setPage dropped the selection without a signal. Say so, so the
    // properties panel and the image actions do not keep showing a placement
    // that is no longer on the page.
    if (hadSelection) {
        emit tool.imageDeselected();
        emit tool.repaintRequested();
    }
    return false;
}

} // namespace pdfe::image
