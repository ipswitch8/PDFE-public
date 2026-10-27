#include "OptimizeEngine.h"
#include "../core/MuPdfContext.h"
#include "../core/AtomicFileReplace.h"
#include "../core/PathSecurity.h"
#include "../core/QpdfWrapper.h"
#include "../signatures/DocMdpDirectWriteGuard.h"

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <stdexcept>

namespace pdfe::optimize {

namespace {

uint64_t fileSize(const std::filesystem::path& p)
{
    std::error_code ec;
    auto sz = std::filesystem::file_size(p, ec);
    return ec ? 0 : static_cast<uint64_t>(sz);
}

// Map OptimizeOptions to pdf_write_options. Garbage level:
//   0 = none, 1 = remove unreferenced, 2 = compact xref, 3 = deduplicate
//   objects, 4 = also deduplicate streams.
int garbageLevel(const OptimizeOptions& opts)
{
    if (opts.mergeDuplicates) return 4;
    if (opts.removeUnused)    return 1;
    return 0;
}

// RECOMPRESSION SCOPE (pipeline 3f8a1c72 Phase 1) — and this site's rationale
// is NOT the one the other write sites carry, which is why it is written out
// rather than cross-referenced.
//
// Everywhere else (Document::save, PageOperations, EncryptionManager, the
// staged external-rewrite bakes) do_compress_images/do_compress_fonts are left
// ON UNCONDITIONALLY, and that is safe because MuPDF's copystream()
// (pdf-write.c:~809) only deflates a stream with NO existing /Filter: anything
// already carrying /DCTDecode, /JPXDecode, /FlateDecode, /CCITTFaxDecode or
// /JBIG2Decode is copied byte-for-byte, so the flags cannot cause a lossy
// re-encode.
//
// Here they are USER-CHOSEN (`recompressImages` / `subsetFonts`), because
// recompressing is the optimizer's entire purpose. The same MuPDF guard still
// applies, so the ceiling on what these flags can do is identical; the
// difference is that this is the one site where the user asked for it.
//
// This comment was ADDED 2026-08-12 (WIN-87 phase-59). The guard that requires
// it (F19DocClaims) named three files by hand and this was not one of them, so
// a fourth site setting the flags carried no scope note for as long as the
// guard has existed. The guard now ENUMERATES the sites instead.
void fillWriteOptions(pdf_write_options& wo, const OptimizeOptions& opts)
{
    std::memset(&wo, 0, sizeof(wo));
    wo.do_compress        = opts.compressStreams ? 1 : 0;
    wo.do_compress_images = opts.recompressImages ? 1 : 0;
    wo.do_compress_fonts  = opts.subsetFonts ? 1 : 0;
    wo.do_garbage         = garbageLevel(opts);
    wo.do_clean           = opts.sanitizeStreams ? 1 : 0;
    wo.do_sanitize        = opts.sanitizeStreams ? 1 : 0;
    // NOT wo.do_linear. MuPDF 1.26.10 REMOVED linearization: setting do_linear
    // makes pdf_save_document throw
    //     "argument error: Linearisation is no longer supported"
    // and the whole optimize fails. GitHub #144 reported this branch as merely
    // untested; running it for the first time showed the shipping "Linearize
    // (Fast Web View)" checkbox has been failing outright. Zero coverage is
    // why nobody knew. Linearization is applied after this write instead —
    // see linearizeInPlace() and its caller in run().
}

/// Linearize `file` in place using QPDF, which still supports it.
///
/// MuPDF cannot do this any more (see fillWriteOptions), so the optimizer runs
/// its MuPDF pass first and then re-writes the result through QPDF. That costs
/// a second full rewrite, but only when the user actually ticks the box.
///
/// This also answers the open question in GitHub #144. QpdfWrapper::save(path,
/// linearize=true) had four tests and NO production caller, leaving it either
/// dead configuration or a supported API whose caller was missing. It was the
/// latter: this is the caller.
///
/// QPDF writes to a sibling temp path and then publishes, rather than writing
/// over the file it is reading — the writer would otherwise be consuming bytes
/// it is simultaneously replacing.
///
/// WIN-87, and this function was a REGRESSION of that closed class. Its first
/// version staged to `<file>.lin.tmp` and published with a bare
/// `std::filesystem::rename(tmp, file)`. Both halves are the substitution shape
/// the WIN-87 sweep exists to reject:
///
///   * the staging name was DERIVED FROM THE TARGET, so anyone who can predict
///     the output path can predict it exactly and plant a file there;
///   * the publish RE-RESOLVED BOTH PATHS BY NAME with nothing held, so the
///     bytes that landed were whatever occupied `tmp` at rename time, not the
///     bytes QPDF wrote.
///
/// `tools/gate/check_win87_publish_shapes.py` caught it on every push after the
/// commit that introduced it; the sweep was doing its job and the red went
/// unread. It is fixed here the way the class was closed elsewhere.
///
/// WHY NOT SIMPLY `createBeside`. That holds an exclusive handle from creation
/// through publication, which is the stronger primitive — but QPDF opens its
/// OUTPUT BY NAME, so handing it a held handle locks the producer out of the
/// file it is meant to write (the self-inflicted-lock class this codebase has
/// already hit three times). So the intermediate uses
/// `unpredictableSiblingPath` — 128 bits of CSPRNG an attacker cannot aim at,
/// which is weaker than exclusivity and is stated rather than glossed — and the
/// PUBLISH still goes through a real StagingFile handle, following the same
/// pin/copy/commit shape as WIN-87 site #7 in BatchEngine. The copy is the
/// price of the producer being by-path.
void linearizeInPlace(const std::filesystem::path& file)
{
    std::error_code ec;

    const auto tmp = pdfe::core::StagingFile::unpredictableSiblingPath(file, ec);
    if (tmp.empty()) {
        throw std::runtime_error(
            "linearize: could not create an unpredictable staging name: "
            + ec.message());
    }

    // Remove the intermediate on every exit, including the throwing ones.
    struct TmpGuard {
        const std::filesystem::path& p;
        bool armed = true;
        ~TmpGuard() {
            if (armed) { std::error_code rm; std::filesystem::remove(p, rm); }
        }
    } tmpGuard{tmp};

    {
        pdfe::core::QpdfWrapper qpdf;
        qpdf.open(file);
        qpdf.save(tmp, /*linearize=*/true);
    }   // close before publishing

    // PIN the intermediate with a deny-write read handle, so the bytes about to
    // be published are the ones QPDF wrote and not whatever is at that name now.
    std::error_code pinEc;
    std::FILE* src = pdfe::core::openDenyWriteReadStream(tmp, pinEc);
    if (!src) {
        throw std::runtime_error(
            "linearize: could not pin the linearized intermediate before "
            "publishing it (original left intact): " + pinEc.message());
    }
    struct SrcGuard {
        std::FILE* fp;
        ~SrcGuard() { if (fp) std::fclose(fp); }
    } srcGuard{src};

    auto staged = pdfe::core::StagingFile::createBeside(file, ec);
    if (!staged) {
        throw std::runtime_error(
            "linearize: could not create the output staging file "
            "(original left intact): " + ec.message());
    }

    {
        std::FILE* dst = staged->openDuplicateStream(ec);
        if (!dst) {
            throw std::runtime_error(
                "linearize: could not open the output staging stream "
                "(original left intact): " + ec.message());
        }
        bool copyOk = true;
        char buf[64 * 1024];
        size_t n = 0;
        while ((n = std::fread(buf, 1, sizeof(buf), src)) > 0) {
            if (std::fwrite(buf, 1, n, dst) != n) { copyOk = false; break; }
        }
        if (std::ferror(src)) copyOk = false;
        if (std::fflush(dst) != 0) copyOk = false;
        std::fclose(dst);
        if (!copyOk) {
            throw std::runtime_error(
                "linearize: failed to stage the linearized output "
                "(original left intact)");
        }
    }

    {
        std::error_code flushEc;
        staged->flushToStableStorage(flushEc);   // best-effort
    }

    // Release the pin BEFORE the publish: Windows will not unlink the
    // intermediate while this process holds it open.
    std::fclose(srcGuard.fp);
    srcGuard.fp = nullptr;

    if (!staged->commitOverWithRetry(file, ec)) {
        throw std::runtime_error(
            "linearize: failed to move the linearized output into place "
            "(original left intact): " + ec.message());
    }
}

} // namespace

OptimizeEngine::OptimizeEngine(std::shared_ptr<pdfe::core::MuPdfContext> context)
    : context_(std::move(context))
{
    if (!context_) throw std::invalid_argument("OptimizeEngine: null context");
}

OptimizeEngine::~OptimizeEngine() = default;

OptimizeResult OptimizeEngine::run(const std::filesystem::path& input,
                                   const std::filesystem::path& output,
                                   const OptimizeOptions& opts)
{
    OptimizeResult r;

    // Refuse UNC / network-scheme inputs before fileSize / exists
    // canonicalize them. Same rationale as Document::open's gate.
    if (pdfe::core::path_security::is_unc_or_remote(input.string())) {
        r.errorMessage =
            "Refusing UNC / network-scheme input: " + input.string();
        return r;
    }

    r.sizeBeforeBytes = fileSize(input);

    if (!std::filesystem::exists(input)) {
        r.errorMessage = "Input file does not exist: " + input.string();
        return r;
    }
    if (input == output) {
        r.errorMessage = "Input and output paths must differ";
        return r;
    }

    try {
        context_->execute([&](fz_context* ctx) {
            pdf_document* doc = pdf_open_document(ctx, input.string().c_str());
            fz_try(ctx) {
                // F24 REMEDIATION (security-audit gate, finding 1). Optimize
                // rewrites the whole file — object renumbering, stream
                // recompression, /Clean and /Sanitize rewrite content streams,
                // /Linear reorders the file. Every one of those is
                // DocMdpChangeClass::Other, permitted at no certification
                // level (ISO 32000-1 Table 254), and all of them destroy the
                // byte prefix a /ByteRange vouches for. This engine never goes
                // through pdfe::core::Document, so it never met the F24 save
                // guard.
                //
                // run() already refuses `input == output` textually; this
                // check is BOTH broader (it resolves "./a.pdf" and, on NTFS,
                // "A.PDF" to the same file) and different in kind — it is
                // about the certification, and it also warns on the
                // write-elsewhere path, which the textual check has nothing to
                // say about.
                char refusal[512] = {0};
                if (pdfe::signatures::directWriteRefused(
                        ctx, doc, input, output,
                        pdfe::signatures::DocMdpChangeClass::Other,
                        refusal, sizeof(refusal))) {
                    fz_throw(ctx, FZ_ERROR_ARGUMENT, "%s", refusal);
                }

                pdf_write_options wo;
                fillWriteOptions(wo, opts);
                pdf_save_document(ctx, doc, output.string().c_str(), &wo);
            } fz_always(ctx) {
                pdf_drop_document(ctx, doc);
            } fz_catch(ctx) {
                fz_rethrow(ctx);
            }
        });
    } catch (const std::exception& e) {
        r.errorMessage = e.what();
        return r;
    }

    // Linearization runs as a SECOND pass, outside the fz_try/execute lambda
    // above. It has to be outside: QPDF reports failure by throwing a C++
    // exception, and MuPDF's fz_try is setjmp/longjmp, so a throw crossing that
    // lambda would skip the fz_always cleanup entirely.
    if (opts.linearize) {
        try {
            linearizeInPlace(output);
        } catch (const std::exception& e) {
            // The MuPDF pass already wrote `output`, so a file exists — but it
            // is NOT what the user asked for. Report failure rather than
            // returning success with a silently non-linearized file, which is
            // the failure mode that let this branch stay broken unnoticed.
            r.errorMessage = std::string("Linearization failed: ") + e.what();
            return r;
        }
    }

    r.sizeAfterBytes = fileSize(output);
    return r;
}

OptimizeResult OptimizeEngine::runSingleOption(
    const std::filesystem::path& input,
    const std::filesystem::path& output,
    const OptimizeOptions& opts)
{
    // Same implementation — kept as a named entry point for clarity in
    // per-category benchmark loops.
    return run(input, output, opts);
}

} // namespace pdfe::optimize
