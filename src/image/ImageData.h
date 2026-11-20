#pragma once

#include <QRectF>
#include <array>
#include <string>
#include <vector>

namespace pdfe::image {

/// Information about an image found on a PDF page.
struct PageImageInfo {
    std::string xobjectName;     ///< XObject key (e.g., "Im0")
    int pageIndex = -1;
    int imageIndex = -1;         ///< Index among images on this page
    int pixelWidth = 0;
    int pixelHeight = 0;
    int bitsPerComponent = 8;
    int components = 3;          ///< 1=gray, 3=RGB, 4=CMYK
    QRectF boundingBox;          ///< The VISIBLE region of the placement as an
                                 ///< un-rotated, axis-aligned rect in the image's own
                                 ///< upright frame, centred on the visible centre, in
                                 ///< FITZ DEVICE SPACE of the RENDERED page: Y-down,
                                 ///< origin at the rendered page's TOP-left, page
                                 ///< /Rotate and box origin applied (GitHub #373/#426).
                                 ///< This is the same space as mouse input
                                 ///< (screenToPdf), the ImageTool overlay painter and
                                 ///< the canvas pdf-coord test seams. For a cropped
                                 ///< placement it is the crop, not the whole image
                                 ///< (GitHub #420); its size is visibleWidth() x
                                 ///< visibleHeight(). When rotationRadians is non-zero
                                 ///< this rect is NOT the on-page extent; rotate it
                                 ///< about its centre by MINUS rotationRadians (math
                                 ///< convention in this Y-down frame) to get the
                                 ///< oriented region (GitHub #342/#376).
    QRectF placementBox;         ///< The FULL (uncropped) placement, same frame and
                                 ///< convention as boundingBox (size = displayWidth x
                                 ///< displayHeight, centred on the image's centre).
                                 ///< Equal to boundingBox when not cropped.
    float rotationRadians = 0.0f;///< Rotation of the placement, CCW-positive in the
                                 ///< Y-up RENDERED page frame (positive = visually
                                 ///< counter-clockwise; on an unrotated page this is
                                 ///< PDF user space). In the Y-down fitz frame
                                 ///< boundingBox lives in, the placement corresponds
                                 ///< to a math rotation of -rotationRadians (GitHub
                                 ///< #342/#373/#376). A flip is NOT a rotation: a
                                 ///< mirrored placement reports flippedX or flippedY
                                 ///< plus the rotation that remains (GitHub #424); see
                                 ///< placement::decompose in ImagePlacement.h.
    bool flippedX = false;       ///< Mirrored left-right in its own frame (det < 0).
    bool flippedY = false;       ///< Mirrored top-bottom in its own frame (det < 0).
                                 ///< At most one is set: the pair with the smaller
                                 ///< |rotationRadians| is reported.
    float skewRadians = 0.0f;    ///< Angle between the image's own axes minus 90
                                 ///< degrees; 0 for a rectangular placement. Every
                                 ///< edit keeps it (the editor works from the full
                                 ///< matrix); it is reported, never rebuilt from.
    int occurrence = 0;          ///< Placement identity, together with xobjectName:
                                 ///< the n-th `Do` of this name in the page content,
                                 ///< in content order (GitHub #421). It counts the
                                 ///< page's OWN Do's only; a placement inside a Form
                                 ///< XObject reports -1, so it can never alias a page
                                 ///< placement of the same name.
    bool inFormXObject = false;  ///< Drawn inside a Form XObject (GitHub #427):
                                 ///< enumerated and selectable, NOT editable (the
                                 ///< form may be shared with other pages).
    bool cropped = false;        ///< A PDFE crop is in force (see crop).
    std::array<float, 4> crop{0.0f, 0.0f, 1.0f, 1.0f};
                                 ///< Crop in the image's unit square [u0 v0 u1 v1]
                                 ///< (v = 0 is the image's bottom row as drawn); the
                                 ///< full square when not cropped.
    std::array<float, 6> pageCtm{};
                                 ///< The placement matrix: unit square -> RENDERED
                                 ///< fitz page frame (Y-down), row-vector [a b c d e f].
    float displayWidth = 0;      ///< FULL (uncropped) placement width in points: the
                                 ///< length of the image's own x axis.
    float displayHeight = 0;     ///< FULL (uncropped) placement height in points.
    int xDpi = 0, yDpi = 0;
    bool isJpeg = false;         ///< DCTDecode filter
    bool isJpx = false;          ///< JPXDecode (JPEG2000) filter — gated OFF by default (#50)
    bool isJbig2 = false;        ///< JBIG2Decode filter — gated OFF by default (SEC-56 / #56)
    bool isMask = false;
    std::array<float, 6> ctm{};  ///< Effective CTM at the Do in PDF USER space (Y-up;
                                 ///< an enclosing Form XObject's matrix composed in),
                                 ///< [a,b,c,d,e,f]

    /// Check if this info represents a valid image.
    [[nodiscard]] bool isValid() const {
        return !xobjectName.empty() && pixelWidth > 0 && pixelHeight > 0;
    }

    /// Get pixel area.
    [[nodiscard]] int64_t pixelArea() const {
        return static_cast<int64_t>(pixelWidth) * pixelHeight;
    }

    /// Visible (cropped) width / height in points: the size of boundingBox.
    [[nodiscard]] float visibleWidth() const { return static_cast<float>(boundingBox.width()); }
    [[nodiscard]] float visibleHeight() const { return static_cast<float>(boundingBox.height()); }

    /// Get display area in square points.
    [[nodiscard]] float displayArea() const {
        return displayWidth * displayHeight;
    }

    /// Get a descriptive color space string.
    [[nodiscard]] std::string colorSpaceString() const {
        switch (components) {
        case 1: return "Grayscale";
        case 3: return "RGB";
        case 4: return "CMYK";
        default: return "Unknown";
        }
    }

    /// Get a descriptive compression string: the image's filter, read from
    /// the stream. JPX and JBIG2 are named for what they are (JBIG2 may be
    /// lossy, JPX usually is); only an image with none of these lossy-capable
    /// filters is "Lossless" (GitHub #425).
    [[nodiscard]] std::string compressionString() const {
        if (isJpeg) return "JPEG";
        if (isJpx) return "JPEG 2000";
        if (isJbig2) return "JBIG2";
        return "Lossless";
    }
};

/// Transform to apply to an image.
struct ImageTransform {
    float scaleX = 1.0f, scaleY = 1.0f;
    float rotation = 0.0f;       ///< Degrees clockwise
    float translateX = 0.0f, translateY = 0.0f;
    QRectF cropRect;             ///< Empty = no crop; in PDF coordinates

    /// Check if this is an identity transform (no change).
    [[nodiscard]] bool isIdentity() const {
        return scaleX == 1.0f && scaleY == 1.0f
            && rotation == 0.0f
            && translateX == 0.0f && translateY == 0.0f
            && cropRect.isEmpty();
    }

    /// Check if this transform includes a crop.
    [[nodiscard]] bool hasCrop() const {
        return !cropRect.isEmpty();
    }
};

/// Options for inserting an image.
struct InsertImageOptions {
    std::string filePath;
    QRectF targetRect;           ///< Placement in fitz device space (Y-down,
                                 ///< origin at the page's TOP-left, points):
                                 ///< y is the distance of the image's visual
                                 ///< top edge from the page top (GitHub #373)
    int jpegQuality = 85;        ///< JPEG compression quality (0-100)
    bool stripExif = true;       ///< Kept for source compatibility; insertImage
                                 ///< ignores it. Insert always re-encodes from
                                 ///< fresh pixels, so no EXIF/APP1, comment or
                                 ///< /Metadata ever reaches the PDF, whatever
                                 ///< this says (docs/image-placement-model.md).
    bool maintainAspectRatio = true;
};

/// Result of an image operation.
struct ImageOperationResult {
    bool success = false;
    std::string errorMessage;
    PageImageInfo imageInfo;     ///< Info about the affected image
};

} // namespace pdfe::image
