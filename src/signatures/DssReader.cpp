// -*- coding: utf-8 -*-
// Pipeline F phase-10: DssReader implementation. See header.
#include "DssReader.h"

#include "core/Document.h"
#include "core/MuPdfContext.h"

extern "C" {
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
}

namespace pdfe::signatures {

namespace {

// Read the resolved-stream bytes for an indirect or direct stream object.
// Returns the raw decoded payload (the same DER bytes that were originally
// embedded). On any failure returns an empty vector and sets @p ok to
// false; the caller decides whether one bad blob aborts the whole walk.
std::vector<uint8_t> readStreamBytes(fz_context* ctx, pdf_obj* obj, bool& ok)
{
    ok = false;
    std::vector<uint8_t> out;
    if (!obj) return out;
    if (!pdf_is_stream(ctx, obj)) return out;

    fz_buffer* buf = nullptr;
    fz_try(ctx) {
        // pdf_load_stream resolves indirect refs and decodes any stream
        // filters. We always store /DSS streams uncompressed (see
        // LtvDssWriter), but tolerate the general case so a /DSS written
        // by another tool with /Flate compression still round-trips.
        buf = pdf_load_stream(ctx, obj);
    }
    fz_catch(ctx) {
        // Swallow: a bad stream in the middle of the array shouldn't
        // bring down the whole read. Leaves out empty + ok=false.
        return out;
    }
    if (!buf) return out;

    // fz_buffer_storage is a borrowed view (does NOT transfer ownership);
    // fz_drop_buffer below frees the underlying bytes. fz_buffer_extract
    // would transfer ownership and require an extra fz_free(ctx, data),
    // which the previous version of this code missed — that was a leak
    // on every DSS stream read.
    unsigned char* data = nullptr;
    size_t len = fz_buffer_storage(ctx, buf, &data);
    if (data && len > 0) {
        out.assign(data, data + len);
        ok = true;
    }
    fz_drop_buffer(ctx, buf);
    return out;
}

// Walk a /Certs / /OCSPs / /CRLs array of stream refs and append each
// stream's bytes to @p sink. Silently skips entries that don't resolve
// to a stream — same tolerance the writer side has when round-tripping
// /DSS dicts produced by other tools.
void readStreamArray(fz_context* ctx, pdf_obj* arr,
                     std::vector<std::vector<uint8_t>>& sink)
{
    if (!arr || !pdf_is_array(ctx, arr)) return;
    int n = pdf_array_len(ctx, arr);
    for (int i = 0; i < n; ++i) {
        pdf_obj* entry = pdf_array_get(ctx, arr, i);
        bool ok = false;
        auto bytes = readStreamBytes(ctx, entry, ok);
        if (ok && !bytes.empty()) {
            sink.push_back(std::move(bytes));
        }
    }
}

// Fill @p out with the /Cert /OCSP /CRL streams of one /VRI/<key> sub-dict.
// hasDss is left at the caller's discretion — VRI sub-entries don't have
// their own hasDss flag; the parent struct tracks that.
void readVriSubEntry(fz_context* ctx, pdf_obj* entryDict, DssEmbedded& out)
{
    if (!entryDict || !pdf_is_dict(ctx, entryDict)) return;
    readStreamArray(ctx, pdf_dict_gets(ctx, entryDict, "Cert"), out.certs);
    readStreamArray(ctx, pdf_dict_gets(ctx, entryDict, "OCSP"), out.ocsps);
    readStreamArray(ctx, pdf_dict_gets(ctx, entryDict, "CRL"),  out.crls);
}

} // namespace

bool DssReader::readDssEmbedded(pdfe::core::Document* doc, DssEmbedded& out)
{
    out = {};
    if (!doc) return false;

    auto context = doc->context();
    if (!context) return false;
    fz_document* fzDoc = doc->fzDocument();
    if (!fzDoc) return false;

    bool gotDss = false;
    try {
        context->execute([&](fz_context* ctx) {
            pdf_document* pdoc = pdf_document_from_fz_document(ctx, fzDoc);
            if (!pdoc) return;
            pdf_obj* trailer = pdf_trailer(ctx, pdoc);
            if (!trailer) return;
            pdf_obj* root = pdf_dict_get(ctx, trailer, PDF_NAME(Root));
            if (!root) return;
            pdf_obj* dss = pdf_dict_gets(ctx, root, "DSS");
            if (!dss || !pdf_is_dict(ctx, dss)) return;
            gotDss = true;

            // Top-level arrays first.
            readStreamArray(ctx, pdf_dict_gets(ctx, dss, "Certs"), out.certs);
            readStreamArray(ctx, pdf_dict_gets(ctx, dss, "OCSPs"), out.ocsps);
            readStreamArray(ctx, pdf_dict_gets(ctx, dss, "CRLs"),  out.crls);

            // /VRI sub-dict — keyed by uppercase-hex SHA-1 of contents hex.
            pdf_obj* vri = pdf_dict_gets(ctx, dss, "VRI");
            if (!vri || !pdf_is_dict(ctx, vri)) return;
            int n = pdf_dict_len(ctx, vri);
            for (int i = 0; i < n; ++i) {
                pdf_obj* keyObj = pdf_dict_get_key(ctx, vri, i);
                pdf_obj* valObj = pdf_dict_get_val(ctx, vri, i);
                if (!keyObj || !valObj) continue;
                const char* keyName = pdf_to_name(ctx, keyObj);
                if (!keyName || !*keyName) continue;
                DssEmbedded sub;
                readVriSubEntry(ctx, valObj, sub);
                // Even an empty sub-entry is meaningful — it tells the
                // verifier "this signature was acknowledged in the LTV
                // augmentation, just without revocation data".
                out.vriEntries.insert(QString::fromUtf8(keyName),
                                       std::move(sub));
            }
        });
    } catch (const std::exception&) {
        // Hard MuPDF failure — return false but leave out empty.
        out = {};
        return false;
    }

    out.hasDss = gotDss;
    return gotDss;
}

} // namespace pdfe::signatures
