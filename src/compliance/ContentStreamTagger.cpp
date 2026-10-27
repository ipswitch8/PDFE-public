#include "ContentStreamTagger.h"
#include "../core/MuPdfContext.h"
#include "../core/Document.h"

#include <cstring>
#include <sstream>

extern "C" {
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
}

namespace pdfe::compliance {

class ContentStreamTagger::Impl {
public:
    explicit Impl(std::shared_ptr<pdfe::core::MuPdfContext> context)
        : context_(std::move(context))
    {}

    std::shared_ptr<pdfe::core::MuPdfContext> context_;
};

ContentStreamTagger::ContentStreamTagger(std::shared_ptr<pdfe::core::MuPdfContext> context)
    : impl_(std::make_unique<Impl>(std::move(context)))
{}

ContentStreamTagger::~ContentStreamTagger() = default;

const char* artifactTypeToName(ArtifactType type) {
    switch (type) {
        case ArtifactType::Pagination: return "Pagination";
        case ArtifactType::Layout:     return "Layout";
        case ArtifactType::Page:       return "Page";
        case ArtifactType::Background: return "Background";
    }
    return "Pagination";
}

const char* artifactSubtypeToName(ArtifactSubtype subtype) {
    switch (subtype) {
        case ArtifactSubtype::Header:    return "Header";
        case ArtifactSubtype::Footer:    return "Footer";
        case ArtifactSubtype::Watermark: return "Watermark";
        case ArtifactSubtype::None:      break;
    }
    return nullptr;   // /Subtype omitted
}

namespace {

// "/Artifact <</Type /Pagination /Subtype /Header /BBox [x0 y0 x1 y1]>> BDC"
// — deliberately WITHOUT /MCID. Artifacts are not structure content; giving
// one an MCID would put page furniture into the ParentTree and hand it to a
// screen reader as document text, which is the exact failure this marking
// exists to prevent.
std::string artifactBdc(const ArtifactRegion& r) {
    std::ostringstream os;
    os << "/Artifact <</Type /" << artifactTypeToName(r.type);
    if (const char* sub = artifactSubtypeToName(r.subtype)) {
        os << " /Subtype /" << sub;
    }
    if (r.hasBBox) {
        os << " /BBox [" << r.bbox[0] << ' ' << r.bbox[1] << ' '
           << r.bbox[2] << ' ' << r.bbox[3] << ']';
    }
    os << ">> BDC\n";
    return os.str();
}

} // namespace

void ContentStreamTagger::tagPageContent(
    pdfe::core::Document* doc,
    int pageIndex,
    const std::vector<MarkedContentRegion>& regions)
{
    if (!doc || regions.empty()) return;

    impl_->context_->execute([&](fz_context* ctx) {
        pdf_document* pdfDoc = pdf_document_from_fz_document(ctx, doc->fzDocument());
        if (!pdfDoc) return;

        int count = pdf_count_pages(ctx, pdfDoc);
        if (pageIndex < 0 || pageIndex >= count) return;

        pdf_obj* pageObj = pdf_lookup_page_obj(ctx, pdfDoc, pageIndex);
        if (!pageObj) return;

        // Read existing content stream
        fz_buffer* existingBuf = nullptr;
        pdf_obj* contents = pdf_dict_get(ctx, pageObj, PDF_NAME(Contents));
        if (!contents) return;

        fz_try(ctx) {
            if (pdf_is_array(ctx, contents)) {
                // Concatenate all content streams
                existingBuf = fz_new_buffer(ctx, 1024);
                int streamCount = pdf_array_len(ctx, contents);
                for (int i = 0; i < streamCount; ++i) {
                    pdf_obj* streamObj = pdf_array_get(ctx, contents, i);
                    fz_buffer* part = pdf_load_stream(ctx, streamObj);
                    unsigned char* partData = nullptr;
                    size_t partLen = fz_buffer_storage(ctx, part, &partData);
                    fz_append_data(ctx, existingBuf, partData, partLen);
                    fz_append_byte(ctx, existingBuf, '\n');
                    fz_drop_buffer(ctx, part);
                }
            } else {
                existingBuf = pdf_load_stream(ctx, contents);
            }
        }
        fz_catch(ctx) {
            fz_drop_buffer(ctx, existingBuf);
            return;
        }

        // Get existing content as string
        unsigned char* existingData = nullptr;
        size_t existingLen = fz_buffer_storage(ctx, existingBuf, &existingData);
        std::string existingContent(reinterpret_cast<const char*>(existingData), existingLen);

        // Build new content stream with BDC/EMC wrapping
        // Strategy: wrap entire existing content in first region's BDC/EMC,
        // then append remaining regions as empty tagged sections.
        // This is a simplified approach -- full implementation would parse and
        // wrap individual operators.
        std::ostringstream newContent;

        // A region tagged Artifact is page furniture: it gets "/Artifact ...
        // BDC" with NO /MCID, because an artifact is outside the structure
        // tree and must not occupy a ParentTree slot. Everything else gets the
        // structure type plus its /MCID.
        auto emitBdc = [&](const MarkedContentRegion& region) {
            if (region.tag == StructureTag::Artifact) {
                newContent << "/Artifact <</Type /Pagination>> BDC\n";
            } else {
                newContent << "/" << structureTagToName(region.tag)
                           << " <</MCID " << region.mcid << ">> BDC\n";
            }
        };

        // Wrap entire existing content in first region's tag
        if (!regions.empty()) {
            emitBdc(regions[0]);
            newContent << existingContent;
            newContent << "\nEMC\n";

            // Add additional tagged sections for remaining regions
            for (size_t i = 1; i < regions.size(); ++i) {
                emitBdc(regions[i]);
                newContent << "EMC\n";
            }
        }

        // Replace page content stream
        std::string newStr = newContent.str();
        fz_buffer* newBuf = fz_new_buffer_from_copied_data(
            ctx,
            reinterpret_cast<const unsigned char*>(newStr.data()),
            newStr.size()
        );

        pdf_obj* newStream = pdf_add_stream(ctx, pdfDoc, newBuf, nullptr, 0);
        fz_drop_buffer(ctx, newBuf);
        fz_drop_buffer(ctx, existingBuf);

        pdf_dict_put(ctx, pageObj, PDF_NAME(Contents), newStream);
        pdf_drop_obj(ctx, newStream);
    });
}

int ContentStreamTagger::markArtifacts(
    pdfe::core::Document* doc,
    int pageIndex,
    const std::vector<ArtifactRegion>& regions)
{
    if (!doc || regions.empty()) return 0;

    // BUILT IN THIS FRAME, NOT INSIDE THE LAMBDA (GitHub #118's hazard class).
    //
    // `os` and `added` used to be declared inside the executeReturn lambda,
    // whose whole body runs inside the fz_try in MuPdfContext. MuPDF's error
    // path is setjmp/longjmp, so an fz_throw from any of the pdf_* calls below
    // jumps to that fz_catch and skips every destructor in between — leaking
    // both the stringstream's buffer and the string's, which for a page with
    // real artifact content is well past the small-string optimisation.
    //
    // Nothing here needs `ctx`: artifactBdc is pure string formatting. So the
    // fix is not an out-parameter (as XmpMetadataWriter needed) but simply
    // hoisting the whole construction out, leaving the lambda with no
    // non-trivially-destructible local at all. `added` is then destroyed by
    // ordinary C++ unwinding after executeReturn returns, whatever MuPDF did.
    std::ostringstream os;
    for (const auto& r : regions) {
        // q/Q brackets each artifact so its graphics state cannot leak
        // into the page content it was appended after.
        os << "q\n" << artifactBdc(r);
        if (!r.contentOperators.empty()) {
            os << r.contentOperators;
            if (r.contentOperators.back() != '\n') os << '\n';
        }
        os << "EMC\nQ\n";
    }
    const std::string added = os.str();

    return impl_->context_->executeReturn<int>([&](fz_context* ctx) -> int {
        pdf_document* pdfDoc = pdf_document_from_fz_document(ctx, doc->fzDocument());
        if (!pdfDoc) return 0;

        int count = pdf_count_pages(ctx, pdfDoc);
        if (pageIndex < 0 || pageIndex >= count) return 0;

        pdf_obj* pageObj = pdf_lookup_page_obj(ctx, pdfDoc, pageIndex);
        if (!pageObj) return 0;

        int emitted = 0;
        fz_buffer* buf = nullptr;
        fz_var(buf);
        fz_try(ctx) {
            buf = fz_new_buffer_from_copied_data(
                ctx,
                reinterpret_cast<const unsigned char*>(added.data()),
                added.size());
            pdf_obj* newStream = pdf_add_stream(ctx, pdfDoc, buf, nullptr, 0);

            // APPEND rather than replace: the page's existing content (which
            // may already be marked) must survive untouched.
            pdf_obj* contents = pdf_dict_get(ctx, pageObj, PDF_NAME(Contents));
            if (contents && pdf_is_array(ctx, contents)) {
                pdf_array_push(ctx, contents, newStream);
            } else {
                pdf_obj* arr = pdf_new_array(ctx, pdfDoc, 2);
                if (contents) pdf_array_push(ctx, arr, contents);
                pdf_array_push(ctx, arr, newStream);
                pdf_dict_put(ctx, pageObj, PDF_NAME(Contents), arr);
                pdf_drop_obj(ctx, arr);
            }
            pdf_drop_obj(ctx, newStream);
            emitted = static_cast<int>(regions.size());
        }
        fz_always(ctx) {
            fz_drop_buffer(ctx, buf);
        }
        fz_catch(ctx) {
            emitted = 0;
        }
        return emitted;
    });
}

bool ContentStreamTagger::pageHasMarkedContent(pdfe::core::Document* doc, int pageIndex) const {
    if (!doc) return false;

    return impl_->context_->executeReturn<bool>([doc, pageIndex](fz_context* ctx) -> bool {
        pdf_document* pdfDoc = pdf_document_from_fz_document(ctx, doc->fzDocument());
        if (!pdfDoc) return false;

        int count = pdf_count_pages(ctx, pdfDoc);
        if (pageIndex < 0 || pageIndex >= count) return false;

        pdf_obj* pageObj = pdf_lookup_page_obj(ctx, pdfDoc, pageIndex);
        if (!pageObj) return false;

        pdf_obj* contents = pdf_dict_get(ctx, pageObj, PDF_NAME(Contents));
        if (!contents) return false;

        bool hasMarked = false;
        fz_buffer* buf = nullptr;

        fz_try(ctx) {
            if (pdf_is_array(ctx, contents)) {
                if (pdf_array_len(ctx, contents) > 0) {
                    buf = pdf_load_stream(ctx, pdf_array_get(ctx, contents, 0));
                }
            } else {
                buf = pdf_load_stream(ctx, contents);
            }

            if (buf) {
                unsigned char* data = nullptr;
                size_t len = fz_buffer_storage(ctx, buf, &data);
                std::string content(reinterpret_cast<const char*>(data), len);
                hasMarked = content.find("BDC") != std::string::npos ||
                            content.find("BMC") != std::string::npos;
            }
        }
        fz_always(ctx) {
            fz_drop_buffer(ctx, buf);
        }
        fz_catch(ctx) {
            // Ignore errors
        }

        return hasMarked;
    });
}

} // namespace pdfe::compliance
