#include "metadata/MetadataManager.h"
#include "core/MuPdfContext.h"
#include "core/Document.h"
#include "compliance/XmpMetadataWriter.h"

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

#include <vector>

namespace pdfe::metadata {

class MetadataManager::Impl {
public:
    explicit Impl(std::shared_ptr<pdfe::core::MuPdfContext> context)
        : context_(std::move(context)) {}

    // Apply a single Info field edit WITHOUT triggering an XMP re-sync, so a
    // batch write can sync once at the end instead of per field.
    void writeFieldRaw(pdfe::core::Document* doc, const std::string& key,
                       const std::string& value) {
        context_->execute([&](fz_context* ctx) {
            pdf_document* pdfDoc = pdf_document_from_fz_document(ctx, doc->fzDocument());
            if (!pdfDoc) {
                return;
            }

            pdf_obj* trailer = pdf_trailer(ctx, pdfDoc);
            pdf_obj* info = pdf_dict_get(ctx, trailer, PDF_NAME(Info));

            if (!info) {
                info = pdf_new_dict(ctx, pdfDoc, 8);
                pdf_dict_put(ctx, trailer, PDF_NAME(Info), info);
            }

            // Map "info:Title" -> "Title" for dict key
            std::string dictKey = key;
            if (dictKey.substr(0, 5) == "info:") {
                dictKey = dictKey.substr(5);
            }

            if (value.empty()) {
                pdf_dict_dels(ctx, info, dictKey.c_str());
            } else {
                pdf_obj* strObj = pdf_new_text_string(ctx, value.c_str());
                pdf_dict_puts(ctx, info, dictKey.c_str(), strObj);
            }
        });
    }

    // If the document already carries an XMP /Metadata packet, refresh the XMP
    // fields mirrored from /Info so the two stores stay consistent. No-op when
    // no XMP packet exists (never fabricates one on a plain Info edit).
    void syncXmpFromInfo(pdfe::core::Document* doc) {
        pdfe::compliance::XmpMetadataWriter writer(context_);
        writer.syncFromInfo(doc);
    }

    std::shared_ptr<pdfe::core::MuPdfContext> context_;
};

MetadataManager::MetadataManager(std::shared_ptr<pdfe::core::MuPdfContext> context)
    : impl_(std::make_unique<Impl>(std::move(context))) {
}

MetadataManager::~MetadataManager() = default;

std::string MetadataManager::readField(pdfe::core::Document* doc, const std::string& key) {
    if (!doc) {
        return "";
    }

    std::string result;

    // `grown` lives in the CALLER's frame, not in the lambda. The lambda body
    // below is the body of MuPdfContext::execute's fz_try, which is
    // setjmp/longjmp: a longjmp abandons that frame without running
    // destructors, so a std::vector declared inside it would leak its buffer on
    // any MuPDF error (follow-up phase-68; tools/gate/fz_try_raii_scan.py would
    // flag it, and the CI baseline gate would fail on it).
    std::vector<char> grown;

    impl_->context_->execute([&](fz_context* ctx) {
        // fz_lookup_metadata returns the number of bytes needed INCLUDING the
        // terminator, and that count is LARGER than the buffer it was given
        // when the value did not fit. The previous implementation ignored the
        // return value entirely and kept whatever had been copied into a fixed
        // 256-byte buffer -- silently truncating, and able to cut a UTF-8
        // sequence mid-codepoint so the survivor was not even valid UTF-8.
        // Worse, EditMetadataCommand snapshots the pre-edit state through this
        // same reader, so an over-long value was truncated on the way IN and
        // the truncation was then written back by undo: data loss caused by a
        // read.
        char probe[256];
        const int needed = fz_lookup_metadata(ctx, doc->fzDocument(),
                                              key.c_str(), probe, sizeof(probe));
        if (needed <= 0) {
            return;  // -1: key absent or unrecognised. Not an error.
        }
        if (static_cast<size_t>(needed) <= sizeof(probe)) {
            result.assign(probe);  // fitted, and MuPDF terminated it
            return;
        }

        // Did not fit. Ask for exactly what MuPDF said it needs and re-read.
        grown.assign(static_cast<size_t>(needed), '\0');
        const int second = fz_lookup_metadata(ctx, doc->fzDocument(),
                                              key.c_str(), grown.data(),
                                              grown.size());
        if (second <= 0 || static_cast<size_t>(second) > grown.size()) {
            // The value changed size between the two calls, or vanished. Report
            // nothing rather than a half-read value: a wrong answer here is
            // written back by undo.
            return;
        }
        result.assign(grown.data());
    });

    return result;
}

void MetadataManager::writeField(pdfe::core::Document* doc, const std::string& key, const std::string& value) {
    if (!doc) {
        return;
    }

    // Overwriting an /Info entry ORPHANS the previous value rather than erasing
    // it. Saved incrementally, the old bytes are still in the file and are
    // trivially recoverable, which for metadata means the author, title or
    // keywords the user just removed are still there. writeMetadata() and
    // clearMetadata() have always forced a full rewrite for this reason;
    // writeField() did not, so the single-field path had the weaker guarantee
    // of the two -- the asymmetry was the defect, not the incremental save.
    doc->markRequiresFullRewrite();

    impl_->writeFieldRaw(doc, key, value);
    // Keep any existing XMP packet consistent with the edited /Info value.
    impl_->syncXmpFromInfo(doc);
}

DocumentMetadata MetadataManager::readMetadata(pdfe::core::Document* doc) {
    if (!doc) {
        return DocumentMetadata{};
    }

    DocumentMetadata metadata;
    metadata.title = readField(doc, "info:Title");
    metadata.author = readField(doc, "info:Author");
    metadata.subject = readField(doc, "info:Subject");
    metadata.keywords = readField(doc, "info:Keywords");
    metadata.creator = readField(doc, "info:Creator");
    metadata.producer = readField(doc, "info:Producer");
    metadata.creationDate = readField(doc, "info:CreationDate");
    metadata.modDate = readField(doc, "info:ModDate");

    return metadata;
}

void MetadataManager::writeMetadata(pdfe::core::Document* doc, const DocumentMetadata& metadata) {
    if (!doc) {
        return;
    }
    // Overwriting /Info orphans the previous values (recoverable if appended
    // incrementally); force a full-rewrite save (pipeline b2e47d91, Tier 1).
    doc->markRequiresFullRewrite();

    impl_->writeFieldRaw(doc, "info:Title", metadata.title);
    impl_->writeFieldRaw(doc, "info:Author", metadata.author);
    impl_->writeFieldRaw(doc, "info:Subject", metadata.subject);
    impl_->writeFieldRaw(doc, "info:Keywords", metadata.keywords);
    impl_->writeFieldRaw(doc, "info:Creator", metadata.creator);
    impl_->writeFieldRaw(doc, "info:Producer", metadata.producer);
    impl_->writeFieldRaw(doc, "info:CreationDate", metadata.creationDate);
    impl_->writeFieldRaw(doc, "info:ModDate", metadata.modDate);

    // Sync the XMP packet once after the full batch of Info edits.
    impl_->syncXmpFromInfo(doc);
}

void MetadataManager::clearMetadata(pdfe::core::Document* doc) {
    if (!doc) {
        return;
    }
    // Clearing /Info removes (potentially sensitive) metadata; force a
    // full-rewrite save so the old values are scrubbed (pipeline b2e47d91).
    doc->markRequiresFullRewrite();

    impl_->context_->execute([&](fz_context* ctx) {
        pdf_document* pdfDoc = pdf_document_from_fz_document(ctx, doc->fzDocument());
        if (!pdfDoc) {
            return;
        }

        pdf_obj* trailer = pdf_trailer(ctx, pdfDoc);
        pdf_obj* info = pdf_dict_get(ctx, trailer, PDF_NAME(Info));

        if (info) {
            // Remove all standard metadata fields
            pdf_dict_dels(ctx, info, "Title");
            pdf_dict_dels(ctx, info, "Author");
            pdf_dict_dels(ctx, info, "Subject");
            pdf_dict_dels(ctx, info, "Keywords");
            pdf_dict_dels(ctx, info, "Creator");
            pdf_dict_dels(ctx, info, "Producer");
            pdf_dict_dels(ctx, info, "CreationDate");
            pdf_dict_dels(ctx, info, "ModDate");
        }
    });

    // If an XMP packet exists, refresh it from the now-empty /Info so the mirrored
    // dc:*/xmp:*/pdf:* fields are dropped too (keeps the two stores consistent).
    impl_->syncXmpFromInfo(doc);
}

} // namespace pdfe::metadata
