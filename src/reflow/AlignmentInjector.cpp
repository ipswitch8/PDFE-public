#include "AlignmentInjector.h"
#include "core/MuPdfContext.h"
#include "core/FormXObjectDepthGuard.h"
#include "core/PageContentGuard.h"
#include "core/PathSecurity.h"
#include "signatures/DocMdpDirectWriteGuard.h"

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace pdfe::reflow {

struct AlignmentInjector::Impl {
    std::shared_ptr<pdfe::core::MuPdfContext> ctx;
};

AlignmentInjector::AlignmentInjector(std::shared_ptr<pdfe::core::MuPdfContext> ctx)
    : impl_(std::make_unique<Impl>())
{
    impl_->ctx = std::move(ctx);
    if (!impl_->ctx) {
        throw std::invalid_argument("AlignmentInjector: context must not be null");
    }
}

AlignmentInjector::~AlignmentInjector() = default;

namespace {

bool streamIsAlignMarked(fz_context* ctx, pdf_obj* streamObj)
{
    if (!streamObj) return false;
    fz_buffer* buf = nullptr;
    fz_try(ctx) { buf = pdf_load_stream(ctx, streamObj); }
    fz_catch(ctx) { buf = nullptr; }
    if (!buf) return false;

    unsigned char* data = nullptr;
    size_t len = fz_buffer_storage(ctx, buf, &data);
    bool marked = false;
    if (data && len > 0) {
        const std::string marker = AlignmentInjector::kStartMarker;
        const size_t scan = std::min<size_t>(len, 128);
        for (size_t i = 0; i + marker.size() <= scan; ++i) {
            if (std::memcmp(data + i, marker.data(), marker.size()) == 0) {
                marked = true;
                break;
            }
        }
    }
    fz_drop_buffer(ctx, buf);
    return marked;
}

void stripMarkedFromPage(fz_context* ctx, pdf_document* /*doc*/,
                        pdf_page* page)
{
    pdf_obj* contents = pdf_dict_get(ctx, page->obj, PDF_NAME(Contents));
    if (!contents) return;

    if (!pdf_is_array(ctx, contents)) {
        if (streamIsAlignMarked(ctx, contents)) {
            pdf_dict_del(ctx, page->obj, PDF_NAME(Contents));
        }
        return;
    }
    const int n = pdf_array_len(ctx, contents);
    for (int i = n - 1; i >= 0; --i) {
        pdf_obj* entry = pdf_array_get(ctx, contents, i);
        if (streamIsAlignMarked(ctx, entry)) {
            pdf_array_delete(ctx, contents, i);
        }
    }
    if (pdf_array_len(ctx, contents) == 0) {
        pdf_dict_del(ctx, page->obj, PDF_NAME(Contents));
    }
}

void ensureF1Helvetica(fz_context* ctx, pdf_document* doc, pdf_page* page)
{
    pdf_obj* resources = pdf_dict_get(ctx, page->obj, PDF_NAME(Resources));
    if (!resources) {
        resources = pdf_new_dict(ctx, doc, 2);
        pdf_dict_put(ctx, page->obj, PDF_NAME(Resources), resources);
    }
    pdf_obj* fonts = pdf_dict_get(ctx, resources, PDF_NAME(Font));
    if (!fonts) {
        fonts = pdf_new_dict(ctx, doc, 1);
        pdf_dict_put(ctx, resources, PDF_NAME(Font), fonts);
    }
    if (pdf_dict_gets(ctx, fonts, "F1")) return;
    pdf_obj* f1 = pdf_new_dict(ctx, doc, 4);
    pdf_dict_put(ctx, f1, PDF_NAME(Type), PDF_NAME(Font));
    pdf_dict_put(ctx, f1, PDF_NAME(Subtype), PDF_NAME(Type1));
    pdf_dict_put_name(ctx, f1, PDF_NAME(BaseFont), "Helvetica");
    pdf_dict_put_name(ctx, f1, PDF_NAME(Encoding), "WinAnsiEncoding");
    pdf_dict_puts(ctx, fonts, "F1", f1);
}

void appendStream(fz_context* ctx, pdf_document* doc, pdf_page* page,
                  const std::string& body)
{
    // Append so the aligned text runs at IDENTITY CTM even on N-up / imposed
    // pages: appendContentIsolatingAmbientCtm brackets the existing page
    // content in `q ... Q`, so its outer `Q` restores the page-initial identity
    // CTM before this fragment runs. A bare append would inherit an ambient
    // `cm` (e.g. "0.5 0 0 0.5 0 0 cm") and render the text half-size. The
    // fragment is itself balanced `q ... Q`, so the guard is a no-op for the
    // OUTBOUND direction. (Register H1 / content-stream-state-continuity audit,
    // pipeline content-stream-continuity phase-2.)
    pdfe::core::appendContentIsolatingAmbientCtm(
        ctx, doc, page->obj, body.data(), body.size());
}

/// The page walk shared by apply() and applyThroughHandles(), so the two forms
/// cannot drift. Runs INSIDE an fz_try — it may fz_throw, so it constructs no
/// C++ object whose destructor matters across that boundary beyond the ones
/// the original inline code already had.
void applyToOpenDocument(
    fz_context* ctx, pdf_document* doc,
    const std::filesystem::path& input,
    const std::filesystem::path& output,
    const std::unordered_map<int,
                             std::vector<const AlignmentInjector::PageBlock*>>&
        byPage)
{
    // F24 REMEDIATION (security-audit gate, finding 1). Alignment
    // apply and remove both rewrite page CONTENT STREAMS —
    // DocMdpChangeClass::Other, permitted at no certification level
    // (ISO 32000-1 Table 254). This engine never goes through
    // pdfe::core::Document, so it never met the F24 save guard and
    // could overwrite a certified original in place unannounced.
    // Same policy: refuse in place, warn and proceed elsewhere.
    char refusal[512] = {0};
    if (pdfe::signatures::directWriteRefused(
            ctx, doc, input, output,
            pdfe::signatures::DocMdpChangeClass::Other,
            refusal, sizeof(refusal))) {
        fz_throw(ctx, FZ_ERROR_ARGUMENT, "%s", refusal);
    }

    const int total = pdf_count_pages(ctx, doc);
    for (int i = 0; i < total; ++i) {
        pdf_page* page = pdfe::core::safeLoadPdfPage(ctx, doc, i);
        fz_try(ctx) {
            stripMarkedFromPage(ctx, doc, page);
            auto it = byPage.find(i);
            if (it != byPage.end() && !it->second.empty()) {
                ensureF1Helvetica(ctx, doc, page);
                for (const auto* pb : it->second) {
                    const std::string fragment =
                        AlignmentWriter::emitFragment(pb->params);
                    if (!fragment.empty()) {
                        appendStream(ctx, doc, page, fragment);
                    }
                }
            }
        } fz_always(ctx) {
            fz_drop_page(ctx, reinterpret_cast<fz_page*>(page));
        } fz_catch(ctx) {
            fz_rethrow(ctx);
        }
    }
}

} // namespace

// ===========================================================================
// WIN-87 site #5, phase-57 — the by-HANDLE form of apply().
//
// apply() above takes two PATHS and resolves both inside MuPDF:
// pdf_open_document() opens `input` by name, and pdf_save_document() opens
// `output` by name (and, on Windows, DELETES it first — MuPDF's own comment at
// source/fitz/output.c cites upstream bug 701797 for that link-swap class).
// MainWindow's alignment flush runs with the user's document DELIBERATELY
// CLOSED so Windows will permit the in-place rewrite, so both of those names
// were unheld and fully predictable for the whole flush.
//
// This form takes streams the caller has already opened and never resolves a
// name at all. It is otherwise the same routine — deliberately so: the two
// share their page walk through applyToOpenDocument() below rather than being
// copies that can drift.
//
// pdf_write_document IS pdf_save_document minus the path here. Verified for
// this caller in MuPDF 1.26.10's own source rather than assumed (the same
// checks Document::save's migration recorded): both end in
// do_pdf_save_document(); pdf_save_document additionally opens the output and
// runs a do_appearance synthesis pass, and neither call site sets
// do_appearance (both zero-initialise their options). `bias` is set only under
// do_incremental, which a full rewrite never sets, so the two emit identical
// output for a document with a header preamble.
//
// OWNERSHIP. `input` is fclosed by this function on every path. `output` is
// handed to fz_new_output_with_file_ptr, and fz_drop_output fcloses it on
// every path including the throwing one. Neither may be closed by the caller.
// ===========================================================================
void AlignmentInjector::applyThroughHandles(
    std::FILE* input, const std::filesystem::path& inputPath,
    std::FILE* output, const std::filesystem::path& outputPathForGuard,
    const std::vector<PageBlock>& blocks)
{
    // RAII lives OUT here, never inside an fz_try: MuPDF's setjmp/longjmp does
    // not unwind C++ destructors. MuPdfContext::execute converts an fz_throw
    // into a C++ exception at its own boundary, outside the fz_catch, so
    // ordinary unwinding is correct at this level.
    struct InputGuard {
        std::FILE* fp;
        ~InputGuard() { if (fp) std::fclose(fp); }
    } inGuard{input};

    if (!input || !output) {
        if (output) std::fclose(output);
        throw std::runtime_error(
            "AlignmentInjector::applyThroughHandles: null stream");
    }

    std::unordered_map<int, std::vector<const PageBlock*>> byPage;
    for (const auto& b : blocks) byPage[b.pageIndex].push_back(&b);

    impl_->ctx->execute([&](fz_context* ctx) {
        pdf_document* doc = nullptr;
        fz_stream* in = nullptr;
        fz_output* out = nullptr;
        fz_var(doc);
        fz_var(in);
        fz_var(out);
        fz_try(ctx) {
            // `out` IS CONSTRUCTED FIRST, AND THE ORDER IS THE POINT. This
            // header promises that fz_drop_output fcloses `output` on every
            // path including the throwing one, and that the caller must never
            // close it. That promise was FALSE for a throw before `out`
            // existed: fz_drop_output(ctx, NULL) is a no-op, so `output` was
            // never closed. Reachable via pdf_open_document_with_stream on a
            // damaged source and via applyToOpenDocument's F24 refusal.
            //
            // The consequence is worse than a handle leak. `output` is a
            // duplicate of a StagingFile handle, and StagingFile unlinks via
            // FileDispositionInfo — which takes effect when the LAST handle
            // closes. A leaked duplicate therefore keeps the staging file alive
            // on disk for the life of the process, at an unpredictable name, in
            // the user's document directory. Found by the non-gate security
            // review; constructing `out` first makes the contract true rather
            // than aspirational, since fz_new_output_with_file_ptr cannot fail
            // for a non-NULL FILE* except by OOM.
            out = fz_new_output_with_file_ptr(ctx, output);
            in = fz_open_file_ptr_no_close(ctx, input);
            doc = pdf_open_document_with_stream(ctx, in);
            applyToOpenDocument(ctx, doc, inputPath, outputPathForGuard, byPage);
            pdf_write_options opts{};
            pdf_write_document(ctx, doc, out, &opts);
            fz_close_output(ctx, out);
        }
        fz_always(ctx) {
            fz_drop_output(ctx, out);   // fcloses `output`, success or throw
            pdf_drop_document(ctx, doc);
            fz_drop_stream(ctx, in);
        }
        fz_catch(ctx) {
            fz_rethrow(ctx);
        }
    });
}

void AlignmentInjector::apply(const std::filesystem::path& input,
                              const std::filesystem::path& output,
                              const std::vector<PageBlock>& blocks)
{
    if (pdfe::core::path_security::is_unc_or_remote(input.string())) {
        throw std::runtime_error(
            "AlignmentInjector::apply: refusing UNC / network-scheme input: "
            + input.string());
    }

    // Group blocks by page to keep page-walk time bounded.
    std::unordered_map<int, std::vector<const PageBlock*>> byPage;
    for (const auto& b : blocks) byPage[b.pageIndex].push_back(&b);

    impl_->ctx->execute([&](fz_context* ctx) {
        pdf_document* doc = pdf_open_document(ctx, input.string().c_str());
        fz_try(ctx) {
            applyToOpenDocument(ctx, doc, input, output, byPage);
            pdf_save_document(ctx, doc, output.string().c_str(), nullptr);
        } fz_always(ctx) {
            pdf_drop_document(ctx, doc);
        } fz_catch(ctx) {
            fz_rethrow(ctx);
        }
    });
}

void AlignmentInjector::remove(const std::filesystem::path& input,
                               const std::filesystem::path& output)
{
    if (pdfe::core::path_security::is_unc_or_remote(input.string())) {
        throw std::runtime_error(
            "AlignmentInjector::remove: refusing UNC / network-scheme input: "
            + input.string());
    }

    impl_->ctx->execute([&](fz_context* ctx) {
        pdf_document* doc = pdf_open_document(ctx, input.string().c_str());
        fz_try(ctx) {
            // F24 REMEDIATION (security-audit gate, finding 1). Alignment
            // apply and remove both rewrite page CONTENT STREAMS —
            // DocMdpChangeClass::Other, permitted at no certification level
            // (ISO 32000-1 Table 254). This engine never goes through
            // pdfe::core::Document, so it never met the F24 save guard and
            // could overwrite a certified original in place unannounced.
            // Same policy: refuse in place, warn and proceed elsewhere.
            char refusal[512] = {0};
            if (pdfe::signatures::directWriteRefused(
                    ctx, doc, input, output,
                    pdfe::signatures::DocMdpChangeClass::Other,
                    refusal, sizeof(refusal))) {
                fz_throw(ctx, FZ_ERROR_ARGUMENT, "%s", refusal);
            }

            int total = pdf_count_pages(ctx, doc);
            for (int i = 0; i < total; ++i) {
                pdf_page* page = pdfe::core::safeLoadPdfPage(ctx, doc, i);
                fz_try(ctx) {
                    stripMarkedFromPage(ctx, doc, page);
                } fz_always(ctx) {
                    fz_drop_page(ctx, reinterpret_cast<fz_page*>(page));
                } fz_catch(ctx) {
                    fz_rethrow(ctx);
                }
            }
            pdf_save_document(ctx, doc, output.string().c_str(), nullptr);
        } fz_always(ctx) {
            pdf_drop_document(ctx, doc);
        } fz_catch(ctx) {
            fz_rethrow(ctx);
        }
    });
}

} // namespace pdfe::reflow
