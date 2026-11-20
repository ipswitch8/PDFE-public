// -*- coding: utf-8 -*-
#pragma once

#include <QRectF>
#include <memory>
#include <string>

namespace pdfe::core {
class MuPdfContext;
class Document;
class Page;
}

namespace pdfe::image {

class ImageManager;

/// Identity of one image placement: the XObject name plus the occurrence
/// index of its `Do` among the page's Do's of that name, in content order
/// (docs/image-placement-model.md, GitHub #421). PageImageInfo carries both
/// (xobjectName, occurrence).
struct PlacementId {
    std::string xobjectName;
    int occurrence = 0;
};

/// Edits ONE image placement in place (docs/image-placement-model.md).
///
/// Each edit rewrites the bytes of the targeted `Do` occurrence only. Every
/// other operator of the page content - other placements and copies, q/Q, gs,
/// BDC/EMC, clips, the drawing order - is left byte-for-byte as it was. The
/// placement is written as PDFE's wrapper
///     q <F> cm [u0 v0 du dv re W n] /Name Do Q
/// with the crop clip in the image's unit square, inside the cm (see
/// ImagePlacementScanner.h). The editor works from the placement's FULL
/// matrix as read from the page, so rotation, flips, skew and crop that the
/// edit does not name are kept (GitHub #416/#417/#419/#424).
///
/// COORDINATE SPACE (GitHub #373/#426): every geometry parameter below - x/y,
/// centerX/centerY, cropRect - is in FITZ DEVICE SPACE of the RENDERED page:
/// Y-down, origin at the rendered page's TOP-left, page /Rotate and box
/// origin applied. This is the space mouse input
/// (CoordinateTransform::screenToPdf), the enumerated PageImageInfo
/// boundingBox, and the canvas pdf-coord test seams all share. The writer
/// converts ONCE, through the inverse page transform, to the PDF user space
/// the `cm` operator requires (flip-exactly-once at the content-stream
/// boundary).
///
/// The name-only overloads edit occurrence 0 (the first placement of that
/// name). Placements inside a Form XObject are not editable (return false).
///
/// NOTE: Requires friend access to Page::fzPage() and Document::fzDocument().
class ImageContentEditor {
public:
    explicit ImageContentEditor(std::shared_ptr<pdfe::core::MuPdfContext> context);
    ~ImageContentEditor();

    // Non-copyable
    ImageContentEditor(const ImageContentEditor&) = delete;
    ImageContentEditor& operator=(const ImageContentEditor&) = delete;

    /// Move / resize a placement.
    /// @param newWidth, newHeight FULL (uncropped) size in points: the new
    ///        lengths of the image's own x and y axes. The image is scaled in
    ///        its own axes, so flips and skew are kept.
    /// @param x, y Top-left of the FULL placement's UPRIGHT rect (fitz
    ///        Y-down): the placement's centre lands at
    ///        (x + newWidth/2, y + newHeight/2).
    /// @param angleDegrees Orientation, visually clockwise (the rotateImage
    ///        convention). Absolute: pass visualClockwiseDegrees of the
    ///        enumerated placement to keep it (GitHub #416).
    /// The crop and every enclosing context are kept.
    /// @return true on success, false on error
    bool resizeImage(pdfe::core::Page* page, pdfe::core::Document* doc,
                     const std::string& xobjectName,
                     float newWidth, float newHeight,
                     float x, float y,
                     float angleDegrees = 0.0f);
    bool resizeImage(pdfe::core::Page* page, pdfe::core::Document* doc,
                     const PlacementId& placement,
                     float newWidth, float newHeight,
                     float x, float y,
                     float angleDegrees = 0.0f);

    /// Set the orientation of a placement.
    /// @param angleDegrees Absolute orientation, VISUALLY clockwise positive
    ///        (defined on-screen, not in the input frame; GitHub #376). It is
    ///        the rotation part of the decomposition that
    ///        PageImageInfo::rotationRadians reports, so flips and skew are
    ///        kept.
    /// @param centerX, centerY Where the VISIBLE centre (centre of the crop,
    ///        or of the image when uncropped) lands (fitz Y-down, points).
    /// @param displayWidth, displayHeight FULL (uncropped) size in points.
    /// The crop and every enclosing context are kept.
    /// @return true on success, false on error
    bool rotateImage(pdfe::core::Page* page, pdfe::core::Document* doc,
                     const std::string& xobjectName,
                     float angleDegrees,
                     float centerX, float centerY,
                     float displayWidth, float displayHeight);
    bool rotateImage(pdfe::core::Page* page, pdfe::core::Document* doc,
                     const PlacementId& placement,
                     float angleDegrees,
                     float centerX, float centerY,
                     float displayWidth, float displayHeight);

    /// Crop a placement.
    /// @param cropRect The region to keep, in the placement's CURRENT VISIBLE
    ///        UPRIGHT frame: the frame of the enumerated boundingBox (fitz
    ///        Y-down, un-rotated about the visible centre). The new crop is
    ///        the INTERSECTION of the current visible region and this rect, so
    ///        crops compose and a crop never brings back a cropped-away part.
    ///        It is stored in the image's unit square; the placement matrix
    ///        (rotation, flips, skew) is unchanged (GitHub #417).
    /// @param displayWidth, displayHeight, x, y Ignored; kept for source
    ///        compatibility. The placement is read from the page.
    /// @return true on success; false on error or when the intersection is
    ///         empty (the page is then unchanged).
    bool cropImage(pdfe::core::Page* page, pdfe::core::Document* doc,
                   const std::string& xobjectName,
                   QRectF cropRect,
                   float displayWidth, float displayHeight,
                   float x, float y);
    bool cropImage(pdfe::core::Page* page, pdfe::core::Document* doc,
                   const PlacementId& placement,
                   QRectF cropRect,
                   float displayWidth = 0.0f, float displayHeight = 0.0f,
                   float x = 0.0f, float y = 0.0f);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pdfe::image
