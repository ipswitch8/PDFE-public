#include "image/ImageTool.h"
#include "image/ImageManager.h"
#include "image/ImagePlacement.h"
#include "core/Page.h"
#include "core/Document.h"
#include "core/CoverageGeometry.h"
#include <QMouseEvent>
#include <QKeyEvent>
#include <QImage>
#include <QPainter>
#include <QPen>
#include <QBrush>
#include <QPolygonF>
#include <QTransform>
#include <algorithm>
#include <cmath>
#include <limits>

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

    /// Live drag preview: the selected image's VISIBLE pixels (crop applied,
    /// flips applied, upright), captured once when a move / resize / rotate
    /// drag starts and painted at the in-progress geometry by paint() until
    /// the drag ends. Null when no drag is live or the pixels could not be
    /// extracted (paint() then falls back to the outline-only overlay).
    QImage preview_;
    /// selectedIndex_ that preview_ was captured for (even if the capture
    /// failed), or -1. Lets paint() capture once for a selected image whose
    /// overflow it must draw, instead of re-extracting on every repaint.
    int previewFor_ = -1;

    /// The bound page's box (Page::mediaBox: the MediaBox in the fitz space
    /// of Page::boundingBox; ui-tweaks-2 phase-1), read once in setPage. Empty without a page; the clamp and the overflow are then off.
    QRectF pageRect_;

    /// The bound page as the user sees it (Page::boundingBox, the visible
    /// page), read once in setPage: the box isPageBackgroundImage measures
    /// against. Empty without a page; nothing is then a background.
    QRectF visiblePageRect_;

    void dropPreview() {
        preview_ = QImage();
        previewFor_ = -1;
    }

    /// buildPreview() unless preview_ already belongs to the selection.
    void ensurePreview() {
        if (previewFor_ != selectedIndex_) {
            buildPreview();
        }
    }

    /// The selected image's quad minus the page at the live geometry.
    QPainterPath liveOverflow() const {
        if (selectedIndex_ < 0 || selectedIndex_ >= static_cast<int>(images_.size())) {
            return QPainterPath();
        }
        const PageImageInfo& img = images_[selectedIndex_];
        const bool rotating = dragging_ && activeHandle_ == HandlePosition::Rotation;
        const float deg = visualClockwiseDegrees(img) + (rotating ? pendingRotation_ : 0.0f);
        QPainterPath over = imageOverflowPath(img.boundingBox, deg, pageRect_);
        // ui-tweaks phase-4: a placement inside a clip (the save's clip to
        // the page) draws nothing outside it, wherever it is dragged.
        if (!over.isEmpty() && !img.pageClip.isNull()) {
            QPainterPath clip;
            clip.addRect(img.pageClip);
            over = over.intersected(clip).simplified();
            const QRectF ob = over.boundingRect();
            if (over.isEmpty() || ob.width() < 1e-3 || ob.height() < 1e-3) return QPainterPath();
        }
        return over;
    }

    /// Capture preview_ for the selected image. Called on every drag start.
    void buildPreview() {
        preview_ = QImage();
        previewFor_ = selectedIndex_;
        if (!manager_ || !page_ || !doc_ || selectedIndex_ < 0 ||
            selectedIndex_ >= static_cast<int>(images_.size())) {
            return;
        }
        const PageImageInfo& img = images_[selectedIndex_];
        QImage src = manager_->extractImage(page_, doc_, img.imageIndex);
        if (src.isNull()) {
            return;
        }
        src = src.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        // crop is [u0 v0 u1 v1] in the image's unit square with v = 0 the
        // image's BOTTOM row as drawn; QImage row 0 is the top row.
        if (img.cropped) {
            const int w = src.width(), h = src.height();
            const int x0 = std::clamp(int(std::floor(img.crop[0] * w)), 0, w);
            const int x1 = std::clamp(int(std::ceil(img.crop[2] * w)), 0, w);
            const int y0 = std::clamp(int(std::floor((1.0f - img.crop[3]) * h)), 0, h);
            const int y1 = std::clamp(int(std::ceil((1.0f - img.crop[1]) * h)), 0, h);
            if (x1 > x0 && y1 > y0) {
                src = src.copy(x0, y0, x1 - x0, y1 - y0);
            }
        }
        Qt::Orientations flips;
        if (img.flippedX) flips |= Qt::Horizontal;
        if (img.flippedY) flips |= Qt::Vertical;
        if (flips) {
            src = src.flipped(flips);
        }
        preview_ = src;
    }

    explicit Impl(ImageManager* manager)
        : manager_(manager)
    {}

    void refreshImages() {
        images_.clear();
        selectedIndex_ = -1;
        activeHandle_ = HandlePosition::None;
        dragging_ = false;
        dropPreview();
        pageRect_ = page_ ? page_->mediaBox() : QRectF();
        visiblePageRect_ = page_ ? page_->boundingBox() : QRectF();

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
    ///
    /// The page's background (a scan; isPageBackgroundImage) is not a click
    /// target unless it is already the selection (ui-tweaks-2 phase-2): a
    /// press on it falls through to "outside any image", which deselects.
    int hitTestImage(QPointF pos) const {
        for (int i = static_cast<int>(images_.size()) - 1; i >= 0; --i) {
            const auto& img = images_[i];
            if (i != selectedIndex_ && isPageBackgroundImage(img, visiblePageRect_)) {
                continue;
            }
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

    /// The topmost page-background placement (isPageBackgroundImage) whose
    /// drawn rect contains @p pos, or -1. The double-click counterpart of
    /// hitTestImage, which skips exactly these.
    int hitTestBackgroundImage(QPointF pos) const {
        for (int i = static_cast<int>(images_.size()) - 1; i >= 0; --i) {
            const auto& img = images_[i];
            if (!isPageBackgroundImage(img, visiblePageRect_)) {
                continue;
            }
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

    /// Make @p i the selection with no gesture in progress.
    void selectIndex(int i) {
        selectedIndex_ = i;
        activeHandle_ = HandlePosition::None;
        dragging_ = false;
        dropPreview();
    }

    /// Compute new bounding box based on handle drag (unclamped; see
    /// resizeRectForHandle, which carries the GitHub #433 derivation).
    QRectF computeNewRect(HandlePosition handle, QPointF currentPos,
                          float rotationRadians) const {
        return resizeRectForHandle(handle, originalRect_, currentPos - dragStart_,
                                   rotationRadians);
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

int ImageTool::pageIndex() const {
    return impl_->page_ ? impl_->page_->index() : -1;
}

void ImageTool::clearPage() {
    impl_->page_ = nullptr;
    impl_->doc_ = nullptr;
    impl_->images_.clear();
    impl_->selectedIndex_ = -1;
    impl_->activeHandle_ = HandlePosition::None;
    impl_->dragging_ = false;
    impl_->dropPreview();
    impl_->pageRect_ = QRectF();
    impl_->visiblePageRect_ = QRectF();
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
            impl_->buildPreview();
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
        impl_->buildPreview();
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
            // Resizing via handle, held to the page clamp rule (see
            // clampResizedRectToPage in ImageTool.h).
            const float rot = impl_->images_[impl_->selectedIndex_].rotationRadians;
            QRectF newRect = impl_->pageRect_.isEmpty()
                ? impl_->computeNewRect(impl_->activeHandle_, pdfPos, rot)
                : clampResizedRectToPage(impl_->activeHandle_, impl_->originalRect_,
                                         pdfPos - impl_->dragStart_, rot,
                                         impl_->pageRect_);
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
            // Moving the image, held to the page clamp rule (the centre stays
            // on the page; see clampMovedRectToPage in ImageTool.h).
            QPointF delta = pdfPos - impl_->dragStart_;
            QRectF newRect = impl_->pageRect_.isEmpty()
                ? impl_->originalRect_.translated(delta)
                : clampMovedRectToPage(impl_->originalRect_, delta, impl_->pageRect_);
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
        impl_->dropPreview();
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
    impl_->dropPreview();
}

void ImageTool::mouseDoubleClickEvent(QMouseEvent* event, QPointF pdfPos, float scale) {
    if (!event || !impl_->page_ || !impl_->doc_) {
        return;
    }
    // Anything a press acts on keeps the press: a handle of the selection, or
    // an image the click hit-test picks (an ordinary one, or the background
    // when it is already selected). QWidget's default handler used to turn
    // every double-click into a second press, so this is unchanged.
    const bool onSelectionHandle =
        hasSelection() &&
        impl_->hitTestHandle(impl_->images_[impl_->selectedIndex_].boundingBox, pdfPos, scale,
                             impl_->images_[impl_->selectedIndex_].rotationRadians) !=
            HandlePosition::None;
    if (!onSelectionHandle && impl_->hitTestImage(pdfPos) < 0) {
        const int bg = impl_->hitTestBackgroundImage(pdfPos);
        if (bg >= 0) {
            // The deliberate pick-up of the page background: select, no drag.
            impl_->selectIndex(bg);
            emit imageSelected(impl_->images_[bg]);
            emit repaintRequested();
            return;
        }
    }
    mousePressEvent(event, pdfPos, scale);
}

void ImageTool::keyPressEvent(QKeyEvent* event) {
    if (!event) {
        return;
    }

    // ui-tweaks-2 phase-3: Qt delivers a KeyPress already ACCEPTED and
    // PdfCanvas::keyPressEvent takes its early-out on isAccepted(), so a key
    // this tool does not act on must be ignore()d or the canvas never sees it
    // (PageDown/PageUp/Home/End/arrows/zoom were swallowed while the image tool
    // was set). Same contract as SelectTool::onKeyPress (GitHub #186).
    if (event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) {
        if (impl_->selectedIndex_ >= 0) {
            emit removeRequested(impl_->selectedIndex_);
            event->accept();
        } else {
            event->ignore();
        }
    } else if (event->key() == Qt::Key_Escape) {
        deselect();
        event->accept();
    } else {
        event->ignore();
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

    // LIVE PREVIEW. The page tiles still show the image where it was until the
    // edit is committed on release, so during a drag the user saw only an
    // outline move. Wash out the old position, then draw the image's own
    // pixels at the in-progress geometry -- bbox for move/resize, plus the
    // pending angle for a rotation -- so the result is visible while dragging.
    if (impl_->dragging_ && !impl_->preview_.isNull()) {
        const bool rotating = impl_->activeHandle_ == HandlePosition::Rotation;
        const float liveDeg = baseDeg + (rotating ? impl_->pendingRotation_ : 0.0f);
        const bool moved = rotating ? std::abs(impl_->pendingRotation_) > 0.0f
                                    : bbox != impl_->originalRect_;
        if (moved) {
            const QPointF oc = impl_->originalRect_.center();
            painter.save();
            painter.translate(oc);
            painter.rotate(baseDeg);
            painter.translate(-oc);
            painter.fillRect(impl_->originalRect_, QColor(255, 255, 255, 190));
            painter.restore();
        }
        painter.save();
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.translate(imgCenter);
        painter.rotate(liveDeg);
        painter.translate(-imgCenter);
        painter.drawImage(bbox, impl_->preview_);
        painter.restore();
    }

    // OVERFLOW. The part of the image outside the page is cropped on save, so
    // show it, shaded red with a red border, and leave the in-page part
    // untouched. The page tiles end at the page edge, so outside a drag the
    // excess pixels are drawn here (clipped to the excess); during a drag the
    // live preview above has already drawn them. The border is the outline of
    // the excess, clipped to it, so not one red pixel lands on the page.
    const QPainterPath overflow = impl_->liveOverflow();
    if (!overflow.isEmpty()) {
        if (!impl_->dragging_) {
            impl_->ensurePreview();
            if (!impl_->preview_.isNull()) {
                painter.save();
                painter.setClipPath(overflow);
                painter.setRenderHint(QPainter::SmoothPixmapTransform);
                painter.translate(imgCenter);
                painter.rotate(baseDeg);
                painter.translate(-imgCenter);
                painter.drawImage(bbox, impl_->preview_);
                painter.restore();
            }
        }
        painter.save();
        painter.fillPath(overflow, QColor(255, 0, 0, 90));
        painter.setClipPath(overflow);
        painter.setPen(QPen(QColor(220, 0, 0), 3.0f / scale));
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(overflow);
        painter.restore();
    }

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

bool ImageTool::selectPageBackgroundImage() {
    if (!impl_->page_) {
        return false;
    }
    for (int i = static_cast<int>(impl_->images_.size()) - 1; i >= 0; --i) {
        if (isPageBackgroundImage(impl_->images_[i], impl_->visiblePageRect_)) {
            impl_->selectIndex(i);
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

bool ImageTool::hasLivePreviewForTest() const {
    return impl_->dragging_ && !impl_->preview_.isNull();
}

QPainterPath ImageTool::overflowRegion() const {
    return impl_->liveOverflow();
}

QString ImageTool::tooltipAt(QPointF pdfPos) const {
    const QPainterPath overflow = impl_->liveOverflow();
    if (!overflow.isEmpty() && overflow.contains(pdfPos)) {
        return QString::fromUtf8(kImageOverflowCropTooltip);
    }
    return QString();
}

void ImageTool::deselect() {
    if (impl_->selectedIndex_ >= 0) {
        impl_->selectedIndex_ = -1;
        impl_->activeHandle_ = HandlePosition::None;
        impl_->dragging_ = false;
        impl_->dropPreview();
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
    impl_->dropPreview();
    impl_->activeHandle_ = HandlePosition::None;
    impl_->dragging_ = false;
    if (selectIndex >= 0 && selectIndex < static_cast<int>(impl_->images_.size())) {
        impl_->selectedIndex_ = selectIndex;
    } else {
        impl_->selectedIndex_ = -1;
    }
}

QRectF resizeRectForHandle(HandlePosition handle, const QRectF& original,
                           QPointF delta, float rotationRadians) {
    // GitHub #433: the handles belong to the image's OWN frame. original is
    // the upright rect in that frame (centred on the visible centre), and the
    // page shows it turned by -rotationRadians about its centre. The
    // page-space drag is therefore taken into the image's frame (+rotation,
    // the same inverse the hit-test uses), the edge is moved there, and the
    // resulting centre shift is turned back onto the page, so the edge
    // opposite the handle stays where it is on screen. With rotationRadians
    // == 0 every step is the identity and the rect is what it always was.
    QRectF newRect = original;
    if (rotationRadians != 0.0f) {
        delta = rotateAbout(delta, QPointF(0.0, 0.0), rotationRadians);
    }

    switch (handle) {
    case HandlePosition::TopLeft:
        newRect.setTopLeft(original.topLeft() + delta);
        break;
    case HandlePosition::TopCenter:
        newRect.setTop(original.top() + delta.y());
        break;
    case HandlePosition::TopRight:
        newRect.setTopRight(original.topRight() + delta);
        break;
    case HandlePosition::MiddleLeft:
        newRect.setLeft(original.left() + delta.x());
        break;
    case HandlePosition::MiddleRight:
        newRect.setRight(original.right() + delta.x());
        break;
    case HandlePosition::BottomLeft:
        newRect.setBottomLeft(original.bottomLeft() + delta);
        break;
    case HandlePosition::BottomCenter:
        newRect.setBottom(original.bottom() + delta.y());
        break;
    case HandlePosition::BottomRight:
        newRect.setBottomRight(original.bottomRight() + delta);
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
        const QPointF localShift = newRect.center() - original.center();
        const QPointF pageShift = rotateAbout(localShift, QPointF(0.0, 0.0), -rotationRadians);
        newRect.moveCenter(original.center() + pageShift);
    }
    return newRect;
}

namespace {

/// Where the centre may go under the clamp rule (ImageTool.h): the page,
/// grown to include the centre the drag started from.
QRectF allowedCentreRegion(const QRectF& pageRect, QPointF startCentre) {
    const double l = std::min(pageRect.left(), startCentre.x());
    const double t = std::min(pageRect.top(), startCentre.y());
    const double r = std::max(pageRect.right(), startCentre.x());
    const double b = std::max(pageRect.bottom(), startCentre.y());
    return QRectF(QPointF(l, t), QPointF(r, b));
}

bool centreAllowed(QPointF c, const QRectF& region) {
    constexpr double eps = 1e-3;
    return c.x() >= region.left() - eps && c.x() <= region.right() + eps &&
           c.y() >= region.top() - eps && c.y() <= region.bottom() + eps;
}

} // anonymous namespace

QRectF clampMovedRectToPage(const QRectF& original, QPointF delta, const QRectF& pageRect) {
    if (pageRect.isEmpty()) {
        return original.translated(delta);
    }
    const QPointF oc = original.center();
    const QRectF region = allowedCentreRegion(pageRect, oc);
    const QPointF want = oc + delta;
    const QPointF c(std::clamp(want.x(), region.left(), region.right()),
                    std::clamp(want.y(), region.top(), region.bottom()));
    return original.translated(c - oc);
}

QRectF clampResizedRectToPage(HandlePosition handle, const QRectF& original,
                              QPointF delta, float rotationRadians,
                              const QRectF& pageRect) {
    if (pageRect.isEmpty()) {
        return resizeRectForHandle(handle, original, delta, rotationRadians);
    }
    const QPointF oc = original.center();
    const QRectF region = allowedCentreRegion(pageRect, oc);
    // A resize that already obeys the rule is left exactly as it is.
    const QRectF free = resizeRectForHandle(handle, original, delta, rotationRadians);
    if (centreAllowed(free.center(), region)) {
        return free;
    }

    switch (handle) {
    case HandlePosition::TopLeft:
    case HandlePosition::TopRight:
    case HandlePosition::BottomLeft:
    case HandlePosition::BottomRight:
        // A corner moves the centre by delta / 2 on the page, at any rotation
        // (the frame change in and out of the image's axes cancels).
        delta.setX(std::clamp(delta.x(), 2.0 * (region.left() - oc.x()),
                              2.0 * (region.right() - oc.x())));
        delta.setY(std::clamp(delta.y(), 2.0 * (region.top() - oc.y()),
                              2.0 * (region.bottom() - oc.y())));
        break;
    case HandlePosition::MiddleLeft:
    case HandlePosition::MiddleRight:
    case HandlePosition::TopCenter:
    case HandlePosition::BottomCenter: {
        // An edge moves the centre by s/2 along u, the image's own axis on the
        // page, where s = u . delta. Find the interval of s that keeps the
        // centre in the region and clamp s alone.
        const bool horizontal = handle == HandlePosition::MiddleLeft ||
                                handle == HandlePosition::MiddleRight;
        const QPointF u = rotateAbout(horizontal ? QPointF(1.0, 0.0) : QPointF(0.0, 1.0),
                                      QPointF(0.0, 0.0), -rotationRadians);
        double lo = -std::numeric_limits<double>::infinity();
        double hi = std::numeric_limits<double>::infinity();
        auto limit = [&](double uk, double ck, double minK, double maxK) {
            if (std::abs(uk) < 1e-9) {
                return;
            }
            double a = 2.0 * (minK - ck) / uk;
            double b = 2.0 * (maxK - ck) / uk;
            if (a > b) {
                std::swap(a, b);
            }
            lo = std::max(lo, a);
            hi = std::min(hi, b);
        };
        limit(u.x(), oc.x(), region.left(), region.right());
        limit(u.y(), oc.y(), region.top(), region.bottom());
        const double sv = u.x() * delta.x() + u.y() * delta.y();
        const double clamped = std::clamp(sv, std::min(lo, hi), std::max(lo, hi));
        delta += (clamped - sv) * u;
        break;
    }
    default:
        break;
    }

    QRectF r = resizeRectForHandle(handle, original, delta, rotationRadians);
    if (!centreAllowed(r.center(), region)) {
        // Only the 10 pt minimum-size floor can shift the centre past the
        // analytic limit above. Fall back to the largest fraction of the
        // drag that keeps the rule (fraction 0 is the start, which does).
        double good = 0.0, bad = 1.0;
        for (int i = 0; i < 40; ++i) {
            const double mid = 0.5 * (good + bad);
            if (centreAllowed(resizeRectForHandle(handle, original, delta * mid,
                                                  rotationRadians).center(), region)) {
                good = mid;
            } else {
                bad = mid;
            }
        }
        r = resizeRectForHandle(handle, original, delta * good, rotationRadians);
    }
    return r;
}

QPainterPath imageOverflowPath(const QRectF& bbox, float clockwiseDegrees,
                               const QRectF& pageRect) {
    if (pageRect.isEmpty() || bbox.isEmpty()) {
        return QPainterPath();
    }
    const QPointF c = bbox.center();
    QTransform t;
    t.translate(c.x(), c.y());
    t.rotate(clockwiseDegrees);
    t.translate(-c.x(), -c.y());
    const QPolygonF quad = t.map(QPolygonF(bbox));
    // Fast path: wholly on the page.
    if (pageRect.contains(quad.boundingRect())) {
        return QPainterPath();
    }
    QPainterPath image;
    image.addPolygon(quad);
    image.closeSubpath();
    QPainterPath page;
    page.addRect(pageRect);
    QPainterPath overflow = image.subtracted(page).simplified();
    const QRectF ob = overflow.boundingRect();
    if (overflow.isEmpty() || ob.width() < 1e-3 || ob.height() < 1e-3) {
        return QPainterPath();
    }
    return overflow;
}

std::optional<QRectF> pageOverflowCropRect(const PageImageInfo& info, const QRectF& pageRect) {
    namespace pl = placement;
    if (info.inFormXObject || pageRect.isEmpty()) return std::nullopt;
    const auto& m = info.pageCtm;
    const pl::Affine P{m[0], m[1], m[2], m[3], m[4], m[5]};
    pl::Affine Pinv;
    if (!pl::invert(P, Pinv)) return std::nullopt;
    const pl::UnitCrop C = pl::UnitCrop{info.crop[0], info.crop[1], info.crop[2], info.crop[3]}
                               .clamped();
    if (C.isEmpty()) return std::nullopt;

    // The visible quad on the page, clipped to the page rect (Sutherland-
    // Hodgman against the four edges; the page rect is axis-aligned in this
    // frame, the quad need not be).
    std::vector<QPointF> poly{P.apply(C.u0, C.v0), P.apply(C.u1, C.v0),
                              P.apply(C.u1, C.v1), P.apply(C.u0, C.v1)};
    auto clipEdge = [&poly](auto inside, auto cross) {
        std::vector<QPointF> out;
        for (std::size_t i = 0; i < poly.size(); ++i) {
            const QPointF a = poly[i];
            const QPointF b = poly[(i + 1) % poly.size()];
            const bool ina = inside(a), inb = inside(b);
            if (ina) out.push_back(a);
            if (ina != inb) out.push_back(cross(a, b));
        }
        poly = std::move(out);
    };
    auto atX = [](QPointF a, QPointF b, double x) {
        const double t = (x - a.x()) / (b.x() - a.x());
        return QPointF(x, a.y() + t * (b.y() - a.y()));
    };
    auto atY = [](QPointF a, QPointF b, double y) {
        const double t = (y - a.y()) / (b.y() - a.y());
        return QPointF(a.x() + t * (b.x() - a.x()), y);
    };
    const double L = pageRect.left(), R = pageRect.right();
    const double T = pageRect.top(), B = pageRect.bottom();
    clipEdge([&](QPointF p) { return p.x() >= L; }, [&](QPointF a, QPointF b) { return atX(a, b, L); });
    if (!poly.empty())
        clipEdge([&](QPointF p) { return p.x() <= R; }, [&](QPointF a, QPointF b) { return atX(a, b, R); });
    if (!poly.empty())
        clipEdge([&](QPointF p) { return p.y() >= T; }, [&](QPointF a, QPointF b) { return atY(a, b, T); });
    if (!poly.empty())
        clipEdge([&](QPointF p) { return p.y() <= B; }, [&](QPointF a, QPointF b) { return atY(a, b, B); });
    if (poly.size() < 3) return std::nullopt;  // nothing of it on the page

    // Back into the unit square: the smallest unit rect holding the on-page part.
    pl::UnitCrop K{1e300, 1e300, -1e300, -1e300};
    for (const QPointF& p : poly) {
        const QPointF u = Pinv.apply(p);
        K.u0 = std::min(K.u0, u.x()); K.u1 = std::max(K.u1, u.x());
        K.v0 = std::min(K.v0, u.y()); K.v1 = std::max(K.v1, u.y());
    }
    K = C.intersected(K).clamped();
    if (K.isEmpty()) return std::nullopt;

    // Leave the placement alone unless some edge really moves.
    const double lu = pl::axisLengthX(P), lv = pl::axisLengthY(P);
    const double tol = kOverflowCropTolerancePt;
    if ((K.u0 - C.u0) * lu < tol && (C.u1 - K.u1) * lu < tol &&
        (K.v0 - C.v0) * lv < tol && (C.v1 - K.v1) * lv < tol)
        return std::nullopt;

    // cropImage takes its rect in the placement's visible upright frame: a
    // point q of that frame is on the page at rotateAbout(q, centre,
    // -rotation), with the centre of the CURRENT crop. Turn K's on-page
    // corners the other way; cropImage then maps them straight back to K.
    const pl::Decomposition dec = pl::decompose(P);
    const QPointF centre = pl::visibleCentre(P, C);
    double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
    for (const QPointF u : {QPointF(K.u0, K.v0), QPointF(K.u1, K.v0),
                            QPointF(K.u1, K.v1), QPointF(K.u0, K.v1)}) {
        const QPointF q = pl::rotateAbout(P.apply(u), centre, dec.rotation);
        x0 = std::min(x0, q.x()); x1 = std::max(x1, q.x());
        y0 = std::min(y0, q.y()); y1 = std::max(y1, q.y());
    }
    return QRectF(QPointF(x0, y0), QPointF(x1, y1));
}

bool pageClipNeeded(const PageImageInfo& info, const QRectF& pageRect) {
    namespace pl = placement;
    if (info.inFormXObject || pageRect.isEmpty()) return false;
    const double tol = kOverflowCropTolerancePt;
    const QRectF page = pageRect.adjusted(-tol, -tol, tol, tol);
    // Already inside a clip: it is the save's own clip (enumeration reports
    // only PDFE's clip-to-page shape), and clips never stack.
    if (!info.pageClip.isNull()) return false;
    const auto& m = info.pageCtm;
    const pl::Affine P{m[0], m[1], m[2], m[3], m[4], m[5]};
    const QPointF q[4] = {P.apply(info.crop[0], info.crop[1]), P.apply(info.crop[2], info.crop[1]),
                          P.apply(info.crop[2], info.crop[3]), P.apply(info.crop[0], info.crop[3])};
    bool outside = false;
    for (const QPointF& p : q)
        if (!page.contains(p)) outside = true;
    if (!outside) return false;
    // Wholly off the page: nothing to keep (see pageOverflowCropRect).
    QPainterPath quad;
    quad.addPolygon(QPolygonF({q[0], q[1], q[2], q[3]}));
    quad.closeSubpath();
    QPainterPath pagePath;
    pagePath.addRect(pageRect);
    const QRectF on = quad.intersected(pagePath).boundingRect();
    return on.width() > tol && on.height() > tol;
}

double pageCoverageFraction(const PageImageInfo& info, const QRectF& pageBox) {
    const QRectF page = pageBox.normalized();
    const QRectF b = info.boundingBox.normalized();
    const double pageArea = page.width() * page.height();
    if (!(pageArea > 0.0) || b.isEmpty()) {
        return 0.0;
    }
    const float deg = visualClockwiseDegrees(info);
    double covered = 0.0;
    if (std::fmod(std::abs(static_cast<double>(deg)), 360.0) < 1e-6) {
        covered = pdfe::core::clampedCoverageArea(b.left(), b.top(), b.right(), b.bottom(),
                                                  page.left(), page.top(), page.right(),
                                                  page.bottom());
    } else {
        // The oriented quad, as the overlay draws it (imageOverflowPath),
        // clipped to the page; its area by the shoelace formula.
        const QPointF c = b.center();
        QTransform t;
        t.translate(c.x(), c.y());
        t.rotate(deg);
        t.translate(-c.x(), -c.y());
        const QPolygonF on = t.map(QPolygonF(b)).intersected(QPolygonF(page));
        double twice = 0.0;
        for (int i = 0, n = on.size(); i < n; ++i) {
            const QPointF& p = on[i];
            const QPointF& q = on[(i + 1) % n];
            twice += p.x() * q.y() - q.x() * p.y();
        }
        covered = std::abs(twice) / 2.0;
    }
    return std::clamp(covered / pageArea, 0.0, 1.0);
}

bool isPageBackgroundImage(const PageImageInfo& info, const QRectF& pageBox) {
    return pageCoverageFraction(info, pageBox) >= kPageBackgroundCoverage;
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
