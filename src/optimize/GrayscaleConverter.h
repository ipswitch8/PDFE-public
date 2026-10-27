#pragma once

#include <memory>
#include <string>

namespace pdfe::core {
class Document;
class MuPdfContext;
}

namespace pdfe::optimize {

/// Pipeline E phase-7 (Bucket 3e): convert every page's colored
/// content (image XObjects, text colors, vector paint operators) to
/// DeviceGray, in place on a loaded Document.
///
/// Implementation delegates per-page work to MuPDF's
/// pdf_recolor_page(num_comp=1) which natively handles JPEG/Flate
/// re-encoding of color images, repainting RGB / CMYK text and
/// vector ops as DeviceGray, and updating the page's resource dict
/// references. Save the document afterwards to persist the change.
class GrayscaleConverter {
public:
    /// Result struct so the caller can report progress / failures.
    struct Result {
        bool ok = false;
        int pagesConverted = 0;
        int pagesFailed = 0;
        std::string errorMessage;
    };

    /// Convert all pages of `doc` to DeviceGray. Returns a Result with
    /// the number of pages successfully recolored. If a single page
    /// throws inside MuPDF the converter records the failure but
    /// continues with subsequent pages — partial conversion is
    /// preferable to bailing on a multi-page document.
    static Result convertDocumentToGrayscale(pdfe::core::Document* doc);

    /// Convert a single page index to DeviceGray. Returns true on
    /// success. Useful for tests and for selective conversion.
    static bool convertPageToGrayscale(pdfe::core::Document* doc,
                                       int pageIndex);
};

} // namespace pdfe::optimize
