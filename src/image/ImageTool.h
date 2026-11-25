#pragma once

#include "image/ImageData.h"
#include <QObject>
#include <QPointF>
#include <QRectF>
#include <QCursor>
#include <QPainterPath>
#include <QString>
#include <optional>
#include <vector>
#include <memory>

class QMouseEvent;
class QKeyEvent;
class QPainter;

namespace pdfe::core {
class Page;
class Document;
}

namespace pdfe::image {

class ImageManager;

/// Handle positions for image resize/rotate handles.
enum class HandlePosition {
    None,
    TopLeft, TopCenter, TopRight,
    MiddleLeft, MiddleRight,
    BottomLeft, BottomCenter, BottomRight,
    Rotation  // Handle above top center for rotation
};

/// Canvas tool for selecting and manipulating images on PDF pages.
class ImageTool : public QObject {
    Q_OBJECT
public:
    explicit ImageTool(ImageManager* manager, QObject* parent = nullptr);
    ~ImageTool() override;

    /// Set the page and document context.
    void setPage(pdfe::core::Page* page, pdfe::core::Document* doc);

    /// Clear the current page context.
    void clearPage();

    /// Index of the page the tool is bound to, or -1 when it has none.
    int pageIndex() const;

    /// Handle mouse events (called by PdfCanvas). @p pdfPos is in fitz device
    /// space — Y-down, origin at the page's TOP-left, points — the space
    /// CoordinateTransform::screenToPdf produces and the enumerated
    /// PageImageInfo::boundingBox lives in (GitHub #373). Drag deltas, bbox
    /// arithmetic and the committed resize/rotate signals all stay in this
    /// one space; the content-stream writers convert to Y-up user space.
    void mousePressEvent(QMouseEvent* event, QPointF pdfPos, float scale);
    void mouseMoveEvent(QMouseEvent* event, QPointF pdfPos, float scale);
    void mouseReleaseEvent(QMouseEvent* event, QPointF pdfPos, float scale);

    /// Double-click (called by PdfCanvas, same @p pdfPos space as the press).
    /// The DELIBERATE way to pick up a page-covering image (ui-tweaks-3
    /// phase-1): a single click on one deselects (see isPageBackgroundImage),
    /// a double-click on it selects it, with no drag started. Anywhere else a
    /// double-click acts as the press it replaced (QWidget's default turned it
    /// into a second mousePressEvent), so ordinary images, handles and the
    /// "click outside deselects" rule behave exactly as before.
    void mouseDoubleClickEvent(QMouseEvent* event, QPointF pdfPos, float scale);

    /// Handle key events.
    void keyPressEvent(QKeyEvent* event);

    /// Paint selection handles and overlays.
    ///
    /// CONTRACT (GitHub #373): @p painter must already be transformed into
    /// fitz page space — translate(pageTopLeft) then scale(zoom, zoom) — the
    /// way PdfCanvas sets it up before calling (mirroring what the annotation
    /// tools do internally). @p scale is that same zoom, used only to keep
    /// pixel-sized furniture (pen widths, handle sizes, text) zoom-invariant.
    void paint(QPainter& painter, float scale);

    /// Get cursor for current position.
    QCursor cursorAt(QPointF pdfPos, float scale) const;

    /// Check if an image is currently selected.
    bool hasSelection() const;

    /// Get the currently selected image info.
    const PageImageInfo& selectedImage() const;

    /// Programmatically select the enumerated image whose XObject key equals
    /// @p xobjectName, WITHOUT a mouse click (GitHub #329).
    ///
    /// Before this existed, `selectedIndex_` only became valid through the
    /// hit-test in mousePressEvent, so a freshly inserted image had no
    /// selection, drew no handles, and left the image-toolbar actions disabled
    /// until the user separately clicked it. This is the entry point
    /// MainWindow::onInsertImage() calls so the just-inserted image is live
    /// immediately. On a match it sets the selection, emits `imageSelected`
    /// (which paints the handles and enables the toolbar actions) and requests
    /// a repaint. Returns true if a matching image was found and selected;
    /// false (selection unchanged) if the name is empty or not present among
    /// the currently enumerated images.
    bool selectImage(const std::string& xobjectName);

    /// Select one placement by its identity, XObject name + occurrence
    /// (PageImageInfo::occurrence; docs/image-placement-model.md, GitHub
    /// #421). selectImage(name) picks the first placement of that name; this
    /// picks the n-th. Same side effects as selectImage. Returns false
    /// (selection unchanged) if no enumerated placement matches.
    bool selectPlacement(const std::string& xobjectName, int occurrence);

    /// Select the last enumerated image on the page, or false if the page has
    /// none. Enumeration order follows the page content stream, so the last
    /// entry is the most-recently drawn image — the one a fresh append places
    /// on top. Prefer selectImage(name) when the XObject identity is known;
    /// this is the fallback for callers that only know "the one just added".
    bool selectLast();

    /// Select the page-background placement of the tool's page: one for which
    /// isPageBackgroundImage() holds against the visible page box, the topmost
    /// (last drawn) if there are several. Same side effects as selectImage.
    /// Returns false, selection unchanged, when the page has none or the tool
    /// has no page. The Image menu's "Select Page Background Image" calls it.
    bool selectPageBackgroundImage();

    /// Deselect the current image.
    void deselect();

    /// TEST SEAM (GitHub #342). Run the REAL selected-image handle hit-test and
    /// return the handle hit at @p pdfPos (page-space), or HandlePosition::None.
    /// This is the exact routine mousePressEvent/cursorAt use (including the
    /// inverse-rotation about the image centre), exposed so a test can assert a
    /// handle is grabbable at a VISUALLY-ROTATED corner. Returns None when there
    /// is no selection.
    HandlePosition hitTestHandleForTest(QPointF pdfPos, float scale) const;

    /// TEST SEAM (GitHub #342). Replace the enumerated-image list with @p images
    /// and select index @p selectIndex (clamped; -1 clears selection), WITHOUT
    /// touching the page/document. Lets a test drive the tool against a known
    /// placement — e.g. a rotated PageImageInfo whose rotationRadians is set —
    /// so the rotated hit-test can be exercised deterministically without a full
    /// rotate/save/re-enumerate round trip. Does not emit signals.
    void setImagesForTest(std::vector<PageImageInfo> images, int selectIndex);

    /// TEST SEAM. True while a drag is live AND its preview pixels were
    /// captured, i.e. paint() is drawing the image at the in-progress geometry.
    bool hasLivePreviewForTest() const;

    /// The part of the selected image that lies outside its page, in fitz
    /// page space, at the live geometry (in-progress rect and, during a
    /// rotation drag, the pending angle). paint() shades exactly this region
    /// red. Empty when nothing is selected, the tool has no page, or the
    /// image is wholly on the page. See imageOverflowPath().
    QPainterPath overflowRegion() const;

    /// Tooltip for @p pdfPos (fitz page space): kImageOverflowCropTooltip when
    /// the point is inside overflowRegion(), else an empty string. PdfCanvas
    /// asks this from its QEvent::ToolTip handler.
    QString tooltipAt(QPointF pdfPos) const;

signals:
    /// Emitted when an image is selected.
    void imageSelected(const pdfe::image::PageImageInfo& info);

    /// Emitted when selection is cleared.
    void imageDeselected();

    /// Emitted when a move or resize is requested (body drag, resize handle,
    /// or the Resize action). (x, y) is the visual top-left and (newWidth,
    /// newHeight) the size of the image's UPRIGHT rect, in fitz Y-down points.
    /// @p angleDegrees is the image's current orientation, visually clockwise
    /// (the rotateRequested convention, = -rotationRadians in degrees); the
    /// committed placement keeps it. Without it a move rebuilt an upright
    /// placement and silently reset a rotated image to 0 (GitHub #416).
    void resizeRequested(const std::string& xobjectName, float newWidth, float newHeight,
                         float x, float y, float angleDegrees);

    /// Emitted when rotation is requested.
    void rotateRequested(const std::string& xobjectName, float angleDegrees);

    /// Emitted when removal is requested (Delete key or context menu).
    void removeRequested(int imageIndex);

    /// Emitted when crop is requested.
    void cropRequested(const std::string& xobjectName, QRectF cropRect);

    /// Emitted to request a repaint of the canvas.
    void repaintRequested();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

/// Tooltip shown while hovering the part of a selected image that lies outside
/// its page. The one copy of the text: tests compare against this constant.
inline constexpr char kImageOverflowCropTooltip[] =
    "This part of the image is outside the page and will be cropped when the "
    "document is saved.";

/// The rect a resize-handle drag produces, WITHOUT the page clamp: @p original
/// is the upright local rect (PageImageInfo::boundingBox), @p delta the
/// page-space pointer movement since the press, @p rotationRadians the
/// placement's rotation (PageImageInfo convention). The edge opposite the
/// handle stays put on screen; width and height never drop below 10 pt
/// (GitHub #433). ImageTool's resize path uses exactly this before clamping.
[[nodiscard]] QRectF resizeRectForHandle(HandlePosition handle, const QRectF& original,
                                         QPointF delta, float rotationRadians);

/// PAGE CLAMP RULE (phase-3 of ui-tweaks): an image's CENTRE must stay inside
/// its page rect (Page::mediaBox: the page's MediaBox, in the fitz space of
/// Page::boundingBox - so an image may sit past a CropBox that is smaller
/// than the MediaBox; ui-tweaks-2 phase-1). The centre always lies on
/// the image, so the image can never leave the page entirely, however it is
/// rotated, and rotating about the centre can never break the rule. Up to
/// half the image may hang off an edge; that excess is shown shaded red and
/// is cropped on save. If an image already had its centre off the page when
/// the drag started (content made elsewhere), the allowed region is the page
/// rect grown just enough to include that starting centre, so the drag can
/// bring it back but never take it further out.
///
/// Move: @p original translated by @p delta, with the translation clamped so
/// the centre obeys the rule. A delta that already obeys it is unchanged.
[[nodiscard]] QRectF clampMovedRectToPage(const QRectF& original, QPointF delta,
                                          const QRectF& pageRect);

/// Resize: resizeRectForHandle() with @p delta first limited so the resulting
/// centre obeys the same rule. A corner drag moves the centre by delta/2, an
/// edge drag by half the delta's component along the image's own axis; that
/// component (only) is clamped, so a rotated image's edge stops at the limit
/// while the rest of the drag still applies. Equal to resizeRectForHandle()
/// whenever that result already obeys the rule.
[[nodiscard]] QRectF clampResizedRectToPage(HandlePosition handle, const QRectF& original,
                                            QPointF delta, float rotationRadians,
                                            const QRectF& pageRect);

/// The image quad (@p bbox turned by @p clockwiseDegrees about its centre,
/// the QPainter::rotate convention in fitz Y-down space) minus @p pageRect.
/// Empty when the image is wholly on the page or @p pageRect is empty.
[[nodiscard]] QPainterPath imageOverflowPath(const QRectF& bbox, float clockwiseDegrees,
                                             const QRectF& pageRect);

/// SAVE-TIME CROP (phase-4 of ui-tweaks): the crop that makes good on
/// kImageOverflowCropTooltip. Returns the rect to pass to
/// ImageContentEditor::cropImage (or CropImageCommand) for placement @p info
/// so that nothing of it is drawn outside @p pageRect (Page::mediaBox, fitz
/// space - the same page rect the clamp and the overflow shading use), or std::nullopt
/// when the placement must be left exactly as it is.
///
/// The crop is the image's unit-square crop (docs/image-placement-model.md):
/// the visible quad (pageCtm applied to the current crop) is clipped to the
/// page, the clipped polygon is taken back into the unit square, and the new
/// crop is the smallest unit rect holding it. So every pixel that is on the
/// page now is still drawn after the crop, at the same place, and the
/// rotation, flips and skew are untouched (cropImage keeps the matrix).
///   * Upright, flipped, and 90/180/270-degree placements: the page edge runs
///     along the image's own axes, so the crop follows it exactly and the
///     visible quad ends up inside the page.
///   * Any other rotation: the page edge is diagonal in the image's axes and a
///     unit-square crop cannot follow it. Only what can be cut without losing
///     on-page pixels is cut (whole bands beyond an edge); the corner slivers
///     left outside the page are then clipped (pageClipNeeded /
///     ImageContentEditor::clipImageToPage).
///
/// std::nullopt when: the placement is inside a Form XObject (not editable);
/// it already lies within the page, or the crop would move no edge by more
/// than kOverflowCropTolerancePt (so an image at the page edge is left
/// byte-identical and a second save changes nothing); or no part of it is on
/// the page (cropping would leave nothing; removing content is not this
/// function's call).
[[nodiscard]] std::optional<QRectF> pageOverflowCropRect(const PageImageInfo& info,
                                                         const QRectF& pageRect);

/// Edges of a save-time crop that would move by less than this (points) are
/// treated as already on the page; see pageOverflowCropRect().
inline constexpr double kOverflowCropTolerancePt = 0.01;

/// SAVE-TIME CLIP (phase-4 of ui-tweaks): true when placement @p info, after
/// pageOverflowCropRect's crop, still draws outside @p pageRect (by more than
/// kOverflowCropTolerancePt) - a rotation that is not a multiple of 90
/// degrees - so the save must clip it to the page
/// (ImageContentEditor::clipImageToPage). False for a placement already inside
/// a clip (PageImageInfo::pageClip; clips never stack), one in a Form XObject,
/// or one with nothing on the page.
[[nodiscard]] bool pageClipNeeded(const PageImageInfo& info, const QRectF& pageRect);

/// PAGE BACKGROUND RULE (ui-tweaks-2 phase-2). A scanned page is one image
/// drawn over the whole page. To the user that image IS the page, so a click
/// on it must not pick it up: before this rule, on a scanned page every press
/// outside the selected image hit the scan, selected it and dragged, resized
/// or rotated the whole page (the "click away to deselect" branch could never
/// be reached). ImageTool's click hit-test and hover cursor skip a placement
/// for which isPageBackgroundImage() is true unless it is already the
/// selection; selectImage / selectPlacement still select it explicitly.
///
/// THRESHOLD 0.90, between the two neighbouring rules:
///   * Page::isImageDominated (OCR routing) uses > 0.5: "mostly picture". Far
///     too loose here; a half-page photo is an ordinary, editable image.
///   * PdfContentWriter's coversThePageUpright (GitHub #447) asks for every
///     edge within a point or 0.5%: "this image IS the page's pixels". Too
///     strict here; real scans are routinely a little smaller or offset (a
///     scanner's margin, Letter scanned onto A4: 595 x 792 of a 595 x 842 page
///     = 0.94), and those must still read as the page.
/// At 0.90 under a tenth of the page is left outside the image, so there is
/// no meaningful "outside" to click on: the user is clicking the page.
inline constexpr double kPageBackgroundCoverage = 0.90;

/// Fraction (0..1) of @p pageBox that placement @p info covers: its oriented
/// quad (boundingBox turned by visualClockwiseDegrees about its centre, the
/// frame the overlay draws) clamped to the page, over the page's area. 0 for
/// an empty page box or image. Pure: reads nothing but its arguments.
[[nodiscard]] double pageCoverageFraction(const PageImageInfo& info, const QRectF& pageBox);

/// True when @p info covers at least kPageBackgroundCoverage of @p pageBox,
/// the VISIBLE page (Page::boundingBox: what the user sees as the page, in
/// the same fitz frame as PageImageInfo::boundingBox). False for an empty
/// page box. Pure.
[[nodiscard]] bool isPageBackgroundImage(const PageImageInfo& info, const QRectF& pageBox);

/// The image's orientation, visually clockwise in degrees: the convention
/// rotateRequested / resizeRequested and ImageContentEditor use. Enumeration
/// stores rotationRadians CCW-positive in Y-up user space, hence the negation.
[[nodiscard]] float visualClockwiseDegrees(const PageImageInfo& info);

/// The FULL placement rect to pass to ImageContentEditor::resizeImage when
/// the VISIBLE region (boundingBox) of @p info is moved/resized from
/// @p originalVisible to @p newVisible (docs/image-placement-model.md: the
/// editor's resize takes the whole placement, the overlay shows the visible
/// crop, GitHub #420). The crop keeps its place inside the image and scales
/// with it. For an uncropped placement this returns @p newVisible unchanged.
[[nodiscard]] QRectF placementRectForVisibleRect(const PageImageInfo& info,
                                                 const QRectF& originalVisible,
                                                 const QRectF& newVisible);

/// After a committed placement edit, re-enumerate @p page into @p tool and
/// reselect the edited image by its XObject name, so the overlay paint() draws
/// is read back from the page instead of the tool's pre-edit cache
/// (GitHub #391(c) for rotate, GitHub #416 for move/resize). Returns false if
/// the image is no longer on the page.
bool resyncImageToolSelection(ImageTool& tool, pdfe::core::Page* page,
                              pdfe::core::Document* doc, const std::string& xobjectName);

/// As resyncImageToolSelection, but reselects ONE placement by its identity,
/// XObject name + occurrence (docs/image-placement-model.md, GitHub #421), so
/// editing the second copy of an image leaves the second copy selected. If
/// that placement is no longer on the page (it was removed, or an undo took
/// it away) the tool ends with no selection and, if it had one, emits
/// imageDeselected so the properties panel and the image actions follow.
bool resyncImageToolPlacement(ImageTool& tool, pdfe::core::Page* page,
                              pdfe::core::Document* doc, const std::string& xobjectName,
                              int occurrence);

} // namespace pdfe::image
