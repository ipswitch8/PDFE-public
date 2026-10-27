// -*- coding: utf-8 -*-
#include "UnencryptedWrapperProbe.h"

#include "MuPdfContext.h"

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

#include <string>
#include <vector>

namespace pdfe::core {

namespace {

std::string nameOrString(fz_context* ctx, pdf_obj* obj)
{
    if (!obj) return {};
    if (pdf_is_name(ctx, obj)) {
        const char* n = pdf_to_name(ctx, obj);
        return n ? std::string(n) : std::string();
    }
    if (pdf_is_string(ctx, obj)) {
        size_t len = 0;
        const char* s = pdf_to_string(ctx, obj, &len);
        return s ? std::string(s, len) : std::string();
    }
    return {};
}

// Is `filespec` a file specification whose relationship to the document is
// "this is the encrypted payload"? That is the decisive marker; `/Collection`
// on its own only says "portfolio".
bool isEncryptedPayloadSpec(fz_context* ctx, pdf_obj* filespec)
{
    if (!pdf_is_dict(ctx, filespec)) return false;
    pdf_obj* rel = pdf_dict_gets(ctx, filespec, "AFRelationship");
    return nameOrString(ctx, rel) == "EncryptedPayload";
}

// Fill the payload details from a filespec known to be an encrypted payload.
void describePayload(fz_context* ctx, pdf_obj* filespec,
                     UnencryptedWrapperInfo* out)
{
    // /UF is the Unicode name and is preferred over the legacy /F when both
    // are present, per ISO 32000 §7.11.3.
    std::string name = nameOrString(ctx, pdf_dict_get(ctx, filespec, PDF_NAME(UF)));
    if (name.empty())
        name = nameOrString(ctx, pdf_dict_get(ctx, filespec, PDF_NAME(F)));
    out->payloadFileName = name;

    pdf_obj* ef = pdf_dict_get(ctx, filespec, PDF_NAME(EF));
    if (!pdf_is_dict(ctx, ef)) return;
    pdf_obj* stream = pdf_dict_get(ctx, ef, PDF_NAME(F));
    if (!pdf_is_dict(ctx, stream)) {
        stream = pdf_dict_get(ctx, ef, PDF_NAME(UF));
    }
    if (!pdf_is_dict(ctx, stream)) return;

    pdf_obj* ep = pdf_dict_gets(ctx, stream, "EP");
    if (!pdf_is_dict(ctx, ep)) return;
    out->cryptoFilterName =
        nameOrString(ctx, pdf_dict_get(ctx, ep, PDF_NAME(Name)));
    pdf_obj* v = pdf_dict_get(ctx, ep, PDF_NAME(V));
    if (pdf_is_number(ctx, v)) out->payloadVersion = pdf_to_int(ctx, v);
}

// Walk a /Names name-tree's leaf array, calling `visit` with each VALUE.
// Depth-capped: a name tree is attacker-controlled and can be made cyclic.
void forEachNameTreeValue(fz_context* ctx, pdf_obj* node, int depth,
                          UnencryptedWrapperInfo* out)
{
    if (depth > 32 || !pdf_is_dict(ctx, node)) return;

    pdf_obj* names = pdf_dict_get(ctx, node, PDF_NAME(Names));
    if (pdf_is_array(ctx, names)) {
        const int n = pdf_array_len(ctx, names);
        // Entries alternate key, value.
        for (int i = 1; i < n; i += 2) {
            pdf_obj* spec = pdf_array_get(ctx, names, i);
            if (isEncryptedPayloadSpec(ctx, spec)) {
                out->hasEncryptedPayload = true;
                describePayload(ctx, spec, out);
                return;
            }
        }
    }

    pdf_obj* kids = pdf_dict_get(ctx, node, PDF_NAME(Kids));
    if (pdf_is_array(ctx, kids)) {
        const int n = pdf_array_len(ctx, kids);
        for (int i = 0; i < n && !out->hasEncryptedPayload; ++i) {
            forEachNameTreeValue(ctx, pdf_array_get(ctx, kids, i), depth + 1,
                                 out);
        }
    }
}

std::string buildSummary(const UnencryptedWrapperInfo& info)
{
    std::string s =
        "This document is an unencrypted WRAPPER: the page you are looking at "
        "is a cover sheet, and the real content is carried as an encrypted "
        "payload attachment that PDFE cannot open.";
    if (!info.payloadFileName.empty()) {
        s += " Payload: \"" + info.payloadFileName + "\".";
    }
    if (!info.cryptoFilterName.empty()) {
        s += " It requires the cryptographic handler \"" +
             info.cryptoFilterName + "\".";
    }
    if (info.payloadVersion > 0) {
        s += " Encrypted-payload version " +
             std::to_string(info.payloadVersion) + ".";
    }
    if (!info.hasCollection) {
        s += " (The catalog declares no /Collection, which is unusual for a "
             "wrapper — treat the visible content with extra suspicion.)";
    }
    return s;
}

// The real work, writing into storage the CALLER owns.
//
// The out-parameter is not a style choice. This runs inside an fz_try, and
// MuPDF reports failure with fz_throw, which is longjmp and does not run C++
// destructors — an UnencryptedWrapperInfo constructed on this side of the
// setjmp would leak its std::strings on any malformed-object error. The path
// overload below therefore declares the struct in its own frame, outside the
// guarded scope, and passes a pointer in.
void probeInto(fz_context* ctx, pdf_document* doc, UnencryptedWrapperInfo* out)
{
    if (!ctx || !doc || !out) return;

    pdf_obj* root = pdf_dict_get(ctx, pdf_trailer(ctx, doc), PDF_NAME(Root));
    if (!pdf_is_dict(ctx, root)) return;

    UnencryptedWrapperInfo& info = *out;

    info.hasCollection =
        pdf_is_dict(ctx, pdf_dict_get(ctx, root, PDF_NAME(Collection)));

    // (1) The catalog's associated-files array is where a wrapper is REQUIRED
    //     to declare the payload, so it is checked first.
    pdf_obj* af = pdf_dict_gets(ctx, root, "AF");
    if (pdf_is_array(ctx, af)) {
        const int n = pdf_array_len(ctx, af);
        for (int i = 0; i < n; ++i) {
            pdf_obj* spec = pdf_array_get(ctx, af, i);
            if (isEncryptedPayloadSpec(ctx, spec)) {
                info.hasEncryptedPayload = true;
                describePayload(ctx, spec, &info);
                break;
            }
        }
    }

    // (2) And also the embedded-files name tree. BOTH are walked because
    //     neither alone is sufficient: a producer may attach the payload
    //     without listing it in /AF, and a reader that only looked at /AF
    //     would then miss the very document this exists to warn about. The
    //     same "union of two reachability paths" argument PdfSignatureProbe
    //     makes about signature widgets.
    if (!info.hasEncryptedPayload) {
        pdf_obj* names = pdf_dict_get(ctx, root, PDF_NAME(Names));
        if (pdf_is_dict(ctx, names)) {
            forEachNameTreeValue(
                ctx, pdf_dict_get(ctx, names, PDF_NAME(EmbeddedFiles)), 0,
                &info);
        }
    }

    info.isWrapper = info.hasEncryptedPayload;
    if (info.isWrapper) info.summary = buildSummary(info);
}

}  // namespace

UnencryptedWrapperInfo probeUnencryptedWrapper(fz_context* ctx,
                                               pdf_document* doc)
{
    UnencryptedWrapperInfo info;
    probeInto(ctx, doc, &info);
    return info;
}

UnencryptedWrapperInfo probeUnencryptedWrapper(
    const std::shared_ptr<MuPdfContext>& ctx,
    const std::filesystem::path& file)
{
    // Declared HERE, in the unguarded frame, so a longjmp out of MuPDF cannot
    // skip its destructor. See probeInto's comment.
    UnencryptedWrapperInfo info;
    if (!ctx) return info;

    const std::string path = file.string();

    // lint-unc-gate: gated-by-caller probeUnencryptedWrapper
    //
    // GitHub #184. The UNC / network-path refusal for this open lives in the
    // CALLER, not here. MainWindow::openFile (src/ui/MainWindow.cpp) refuses a
    // UNC `fileName` and returns early, ~580 lines before it reaches this
    // probe with that same path. So the guard is real, it is just in another
    // translation unit -- which is why lint_unc_gate.py's 60-line proximity
    // window could never see it, and why WIDENING that window would have been
    // the wrong repair.
    //
    // A second refusal here was considered and rejected. It would be dead on
    // every reachable path, and a dead branch whose only purpose is to quiet a
    // linter trains the next reader to treat the linter as noise.
    //
    // The directive above is NOT a suppression. lint_unc_gate.py re-derives
    // the caller set of probeUnencryptedWrapper() across src/ on every run and
    // fails if ANY caller lacks an is_unc_or_remote guard, if the symbol stops
    // having callers, or if this justification is deleted. Add a caller that
    // does not gate and the lint goes red here. That is a stronger claim than
    // proximity, not a waiver of it.
    pdf_document* doc = nullptr;
    try {
        doc = ctx->executeReturn<pdf_document*>(
            [&path](fz_context* c) -> pdf_document* {
                return pdf_open_document(c, path.c_str());
            });
    } catch (...) {
        // Swallowed on purpose: see the header. An advisory probe must never
        // be the reason a readable document fails to open, so an unparseable
        // file simply answers "not a wrapper" and the ordinary open path
        // reports whatever is actually wrong with it.
        return UnencryptedWrapperInfo{};
    }
    if (!doc) return info;

    try {
        ctx->execute([doc, &info](fz_context* c) { probeInto(c, doc, &info); });
    } catch (...) {
        info = UnencryptedWrapperInfo{};
    }

    try {
        ctx->execute([doc](fz_context* c) { pdf_drop_document(c, doc); });
    } catch (...) {
        // Nothing useful to do; the context is being torn down either way.
    }
    return info;
}

}  // namespace pdfe::core
