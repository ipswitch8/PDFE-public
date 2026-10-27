#pragma once

#include <QImage>
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
/// Uses a QImage roundtrip approach: extract image → decode to QImage
/// (which naturally strips EXIF) → re-encode → replace in the PDF's
/// XObject dictionary.
class ExifStripper {
public:
    explicit ExifStripper(std::shared_ptr<pdfe::core::MuPdfContext> context);
    ~ExifStripper();

    // Non-copyable
    ExifStripper(const ExifStripper&) = delete;
    ExifStripper& operator=(const ExifStripper&) = delete;

    /// Strip metadata from a single image on a page by XObject name.
    bool stripImage(const pdfe::core::Page* page, const pdfe::core::Document* doc,
                    const std::string& xobjectName);

    /// Strip metadata from all images on a page. Returns count of images stripped.
    int stripPageImages(const pdfe::core::Page* page, const pdfe::core::Document* doc);

    /// Strip metadata from all images in the document. Returns total count stripped.
    int stripAllImages(pdfe::core::Document* doc);

    /// Strip EXIF from a QImage in memory (static utility, for insert pipeline).
    /// This creates a clean copy without metadata.
    static QImage stripExif(const QImage& image);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pdfe::image
