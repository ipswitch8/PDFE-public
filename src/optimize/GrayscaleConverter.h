#pragma once

#include <memory>
#include <string>
#include <vector>

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

    // -------------------------------------------------------------------
    // GitHub #355: undo support for "Convert to Grayscale".
    //
    // The conversion rewrites every page's /Contents and /Resources in place,
    // so undo needs the originals. These three helpers snapshot, restore and
    // release them. Each snapshot element is an OPAQUE holder (defined in the
    // .cpp) that deep-copies the two objects into a standalone pdf_document
    // pinning its own MuPdfContext — exactly the teardown-safe mechanism used
    // by Document::retainPageObj and ImageManager's content-ref holders — so a
    // holder survives a full-rewrite save AND the owning Document's
    // destruction. GrayscaleCommand owns the snapshot and drives these.
    // -------------------------------------------------------------------

    /// Deep-copy every page's original /Contents and /Resources. Returns one
    /// opaque holder per page (index-aligned); a null element means that page
    /// could not be snapshotted. Call BEFORE converting.
    static std::vector<void*> snapshotPageContent(pdfe::core::Document* doc);

    /// Graft the snapshot back onto the live pages (non-consuming: the holders
    /// stay owned by the caller). Restores the original colour content.
    static void restorePageContent(pdfe::core::Document* doc,
                                   const std::vector<void*>& snapshot);

    /// Drop every holder. Safe even if the owning Document is already gone
    /// (each holder carries its own pinned context). Clears the vector.
    static void releaseSnapshot(std::vector<void*>& snapshot);
};

} // namespace pdfe::optimize
