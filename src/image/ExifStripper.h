#pragma once

#include <QImage>
#include <QString>
#include <memory>
#include <string>

namespace pdfe::core {
class MuPdfContext;
class Document;
class Page;
}

namespace pdfe::image {

/// Strips EXIF and other metadata from images embedded in a PDF.
///
/// Removes metadata where a PDF image keeps it: the /Metadata (XMP) and other
/// informational entries of the image dictionary, and the EXIF/XMP/IPTC/
/// comment segments and any trailer after EOI of JPEG data (the JFIF, Adobe
/// and ICC segments a decoder reads are kept, cut to their standard size), or
/// every JPEG 2000 box a decoder does not need, cut out losslessly. The image
/// dictionary otherwise stays as it was, so soft masks, stencil masks,
/// /Decode and the colour space (CMYK included) are kept (GitHub #452, #453).
/// An image with nothing to strip is not touched. JPEG/JP2 data that does not
/// parse cleanly is decoded and stored as fresh Flate pixels (up to 100
/// megapixels); if it cannot be decoded it is reported as not stripped.
///
/// Visited: the images of the page's resources and of the Form XObjects in
/// them (recursively), their soft masks, masks and /Alternates. The page's
/// /Thumb is removed. NOT visited: inline images (counted in
/// StripReport::inlineNotProcessed when JPEG), images in patterns, Type 3
/// glyphs and annotation appearances. See ExifStripper.cpp.
class ExifStripper {
public:
    /// What happened to the images of a page or document.
    struct StripReport {
        int stripped = 0;      ///< metadata was found and removed
        int alreadyClean = 0;  ///< nothing to remove; left untouched
        int failed = 0;        ///< could not be stripped; metadata may remain
        /// Inline images (BI/ID/EI) with JPEG or JPEG 2000 data, which may
        /// carry metadata: content streams are not rewritten, so they are
        /// NOT stripped. Not part of total().
        int inlineNotProcessed = 0;
        int thumbnailsRemoved = 0;  ///< page /Thumb images removed (not part of total())
        int metadataFree() const { return stripped + alreadyClean; }
        int total() const { return stripped + alreadyClean + failed; }
    };

    /// What happened to one image.
    enum class ImageResult { Stripped, AlreadyClean, Failed, NotFound };

    explicit ExifStripper(std::shared_ptr<pdfe::core::MuPdfContext> context);
    ~ExifStripper();

    // Non-copyable
    ExifStripper(const ExifStripper&) = delete;
    ExifStripper& operator=(const ExifStripper&) = delete;

    /// Strip metadata from a single image on a page by XObject name.
    ImageResult stripImageResult(const pdfe::core::Page* page, const pdfe::core::Document* doc,
                                 const std::string& xobjectName);

    /// True when the image is free of metadata afterwards (Stripped or
    /// AlreadyClean); false when there is no such image or it could not be
    /// stripped (Failed).
    bool stripImage(const pdfe::core::Page* page, const pdfe::core::Document* doc,
                    const std::string& xobjectName);

    /// Strip metadata from all images on a page, with the per-outcome counts.
    StripReport stripPageImagesReport(const pdfe::core::Page* page, const pdfe::core::Document* doc);

    /// stripPageImagesReport(...).metadataFree(): the number of images free of
    /// metadata afterwards (stripped plus already clean). Images that could
    /// not be stripped are not counted.
    int stripPageImages(const pdfe::core::Page* page, const pdfe::core::Document* doc);

    /// Every page of the document, with the per-outcome counts.
    StripReport stripAllImagesReport(pdfe::core::Document* doc);

    /// stripAllImagesReport(doc).metadataFree().
    int stripAllImages(pdfe::core::Document* doc);

    /// The user-facing summary of a report: "Metadata removed from N image(s).",
    /// then how many had none and how many could not be processed.
    static QString describe(const StripReport& report);

    /// Strip EXIF from a QImage in memory (static utility, for insert pipeline).
    /// This creates a clean copy without metadata.
    static QImage stripExif(const QImage& image);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pdfe::image
