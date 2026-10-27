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
    QRectF boundingBox;          ///< Position on page in PDF coordinates
    float displayWidth = 0;      ///< Rendered width in points
    float displayHeight = 0;     ///< Rendered height in points
    int xDpi = 0, yDpi = 0;
    bool isJpeg = false;         ///< DCTDecode filter
    bool isJpx = false;          ///< JPXDecode (JPEG2000) filter — gated OFF by default (#50)
    bool isJbig2 = false;        ///< JBIG2Decode filter — gated OFF by default (SEC-56 / #56)
    bool isMask = false;
    std::array<float, 6> ctm{};  ///< Current transformation matrix [a,b,c,d,e,f]

    /// Check if this info represents a valid image.
    [[nodiscard]] bool isValid() const {
        return !xobjectName.empty() && pixelWidth > 0 && pixelHeight > 0;
    }

    /// Get pixel area.
    [[nodiscard]] int64_t pixelArea() const {
        return static_cast<int64_t>(pixelWidth) * pixelHeight;
    }

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

    /// Get a descriptive compression string.
    [[nodiscard]] std::string compressionString() const {
        return isJpeg ? "JPEG" : "Lossless";
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
    QRectF targetRect;           ///< Placement in PDF coordinates
    int jpegQuality = 85;        ///< JPEG compression quality (0-100)
    bool stripExif = true;       ///< Strip metadata on insert
    bool maintainAspectRatio = true;
};

/// Result of an image operation.
struct ImageOperationResult {
    bool success = false;
    std::string errorMessage;
    PageImageInfo imageInfo;     ///< Info about the affected image
};

} // namespace pdfe::image
