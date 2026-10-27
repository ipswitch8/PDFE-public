#include "BatchEngine.h"
#include "core/MuPdfContext.h"
#include "core/Document.h"
#include "core/FormXObjectDepthGuard.h"
#include "core/Page.h"
#include "core/PathSecurity.h"
#include "core/AtomicFileReplace.h"
#include "core/ContentStreamPreamble.h"
#include "core/PageContentGuard.h"
// GitHub #99 / WIN-75 — keep the machine awake for the whole batch.
#include "core/SleepInhibitor.h"
#include "compliance/ComplianceManager.h"
#include "compliance/ComplianceTypes.h"
#include "redaction/RedactionManager.h"
#include "redaction/RedactionData.h"
#include "encryption/EncryptionManager.h"
#include "encryption/EncryptionSettings.h"
#include "signatures/DocMdpDirectWriteGuard.h"

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace pdfe::batch {

namespace {

// Ensure page has /Resources /Font /F1 Helvetica so the text watermark
// content stream can reference /F1 safely.
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

} // namespace

BatchEngine::BatchEngine(std::shared_ptr<pdfe::core::MuPdfContext> context)
    : context_(std::move(context))
{
}

BatchEngine::~BatchEngine() = default;

void BatchEngine::setProgressCallback(ProgressCallback cb) {
    progressCb_ = std::move(cb);
}

void BatchEngine::setErrorCallback(ErrorCallback cb) {
    errorCb_ = std::move(cb);
}

void BatchEngine::cancel() {
    cancelled_.store(true);
}

bool BatchEngine::isCancelled() const {
    return cancelled_.load();
}

BatchResult BatchEngine::process(
    const std::vector<std::filesystem::path>& files,
    const std::vector<BatchAction>& actions)
{
    // GitHub #99 / WIN-75 — a batch is the archetypal walk-away operation:
    // hundreds of documents, minutes to hours, no keyboard or mouse activity.
    // Without this the machine's idle-sleep policy suspends it part-way through.
    //
    // Declared FIRST in the function body so it is raised before any work and,
    // being the last local destroyed, released after all of it — including on
    // an early return or an exception escaping processFile. The execution state
    // is per-thread, and this runs on whatever thread drives the batch, which
    // is the thread doing the work. See src/core/SleepInhibitor.h.
    const pdfe::core::ScopedSleepInhibit keepSystemAwake;

    BatchResult result;
    result.totalFiles = static_cast<int>(files.size());
    cancelled_.store(false);

    for (int i = 0; i < static_cast<int>(files.size()); ++i) {
        if (cancelled_.load()) {
            result.cancelled = true;
            break;
        }

        if (progressCb_) {
            progressCb_(i, result.totalFiles, files[i].filename().string());
        }

        auto fileResult = processFile(files[i], actions);
        if (fileResult.success) {
            result.successCount++;
        } else {
            result.errorCount++;
            if (errorCb_) {
                errorCb_(i, files[i].filename().string(), fileResult.errorMessage);
            }
        }
        result.fileResults.push_back(std::move(fileResult));
    }

    return result;
}

BatchFileResult BatchEngine::processFile(
    const std::filesystem::path& file,
    const std::vector<BatchAction>& actions)
{
    BatchFileResult result;
    result.filePath = file.string();

    // Refuse UNC / remote-scheme inputs before std::filesystem::exists
    // canonicalizes them. Same rationale as Document::open's gate.
    if (pdfe::core::path_security::is_unc_or_remote(file.string())) {
        result.errorMessage =
            "Refusing UNC / network-scheme input (PDFE only opens local files)";
        return result;
    }

    if (!std::filesystem::exists(file)) {
        result.errorMessage = "File not found";
        return result;
    }

    auto outputDir = file.parent_path() / "batch_output";
    std::filesystem::create_directories(outputDir);
    auto outputFile = outputDir / file.filename();

    std::filesystem::path currentInput = file;

    for (const auto& action : actions) {
        if (cancelled_.load()) {
            result.errorMessage = "Cancelled";
            return result;
        }

        // WIN-87 site #7, phase-58: UNPREDICTABLE intermediate names.
        //
        // This used to be "temp_<n>_<inputfilename>" in a directory whose name
        // is also derived from the input ("<dir>/batch_output"), so every
        // intermediate in the chain had a name an attacker who knows the input
        // could compute exactly. Each action READS the previous intermediate BY
        // NAME, so substituting one has the next action parse attacker bytes and
        // the chain publish them as the batch output.
        //
        // These cannot be StagingFile handles: each action opens its OUTPUT by
        // name (Document::saveAs, QPDF), so an exclusive handle would lock the
        // producer out of the file it is meant to write. The achievable property
        // here is UNPREDICTABILITY, not exclusivity — 128 bits of CSPRNG the
        // attacker cannot aim at — and that difference is stated on
        // unpredictableSiblingPath rather than glossed.
        std::error_code tmpEc;
        auto tempOutput = pdfe::core::StagingFile::unpredictableSiblingPath(
            outputDir / file.filename(), tmpEc);
        if (tempOutput.empty()) {
            result.errorMessage =
                "Could not create an unpredictable name for the batch "
                "intermediate (nothing written): " + tmpEc.message();
            if (currentInput != file) {
                std::error_code rmec;
                std::filesystem::remove(currentInput, rmec);
            }
            return result;
        }

        try {
            switch (action.type) {
            case BatchActionType::AddWatermark:
                applyWatermark(currentInput, tempOutput,
                    std::get<WatermarkConfig>(action.config));
                break;
            case BatchActionType::FlattenAnnotations:
                applyFlattenAnnotations(currentInput, tempOutput);
                break;
            case BatchActionType::ConvertToPdfA:
                applyConvertToPdfA(currentInput, tempOutput,
                    std::get<PdfAConfig>(action.config));
                break;
            case BatchActionType::SetPermissions:
                applySetPermissions(currentInput, tempOutput,
                    std::get<PermissionsConfig>(action.config));
                break;
            }

            if (currentInput != file) {
                std::filesystem::remove(currentInput);
            }
            currentInput = tempOutput;
            result.actionsCompleted++;

        } catch (const std::exception& e) {
            result.errorMessage = std::string("Action ") +
                std::to_string(result.actionsCompleted) + ": " + e.what();
            if (currentInput != file) {
                std::filesystem::remove(currentInput);
            }
            return result;
        }
    }

    if (currentInput != file && std::filesystem::exists(currentInput)) {
        // F10 / WIN-85: this used to be a bare throwing std::filesystem::rename.
        // Two problems: (a) it threw OUT of process() past the try/catch above,
        // so a locked output file produced an escaping filesystem_error instead
        // of a BatchResult carrying an error message; (b) it had no anti-A/V
        // retry. Both fixes are still in force below.
        //
        // WIN-87 site #7, phase-58: what WIN-85 could not fix is that the helper
        // it routed to takes PATHS, so the last intermediate was re-resolved by
        // name at rename time with nothing held. The publish now goes through an
        // exclusive handle:
        //   * PIN the last intermediate with a deny-write read handle, so the
        //     bytes about to be published are the ones this process wrote and
        //     not whatever is at that name afterwards;
        //   * COPY them into a StagingFile beside the real output;
        //   * PUBLISH by renaming THROUGH that staging handle.
        //
        // The copy is the cost of the actions writing by name. It is bounded by
        // the output size and it is the price of the producers being by-path;
        // pretending otherwise would mean handing a guessable name to a by-path
        // replace, which is the defect.
        std::error_code pinEc;
        std::FILE* src = pdfe::core::openDenyWriteReadStream(currentInput, pinEc);
        if (!src) {
            result.success = false;
            result.errorMessage =
                "Could not pin the batch intermediate before publishing it "
                "(output left intact): " + pinEc.message();
            std::error_code rmec;
            std::filesystem::remove(currentInput, rmec);
            return result;
        }
        struct SrcGuard {
            std::FILE* fp;
            ~SrcGuard() { if (fp) std::fclose(fp); }
        } srcGuard{src};

        std::error_code stageEc;
        auto staged = pdfe::core::StagingFile::createBeside(outputFile, stageEc);
        if (!staged) {
            result.success = false;
            result.errorMessage =
                "Could not create the batch output staging file "
                "(output left intact): " + stageEc.message();
            std::error_code rmec;
            std::filesystem::remove(currentInput, rmec);
            return result;
        }
        {
            std::FILE* dst = staged->openDuplicateStream(stageEc);
            if (!dst) {
                result.success = false;
                result.errorMessage =
                    "Could not open the batch output staging stream "
                    "(output left intact): " + stageEc.message();
                std::error_code rmec;
                std::filesystem::remove(currentInput, rmec);
                return result;
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
                result.success = false;
                result.errorMessage =
                    "Failed to stage the batch output (output left intact)";
                std::error_code rmec;
                std::filesystem::remove(currentInput, rmec);
                return result;
            }
        }
        {
            std::error_code flushEc;
            staged->flushToStableStorage(flushEc);  // best-effort
        }
        // Release the pin BEFORE the publish: a handle this process holds on the
        // source does not block the rename of a DIFFERENT file, but the
        // intermediate is removed immediately after and Windows will not unlink
        // it while we hold it open.
        std::fclose(srcGuard.fp);
        srcGuard.fp = nullptr;

        std::error_code ec;
        if (!staged->commitOverWithRetry(outputFile, ec)) {
            result.success = false;
            result.errorMessage =
                "Failed to move batch output into place (output left intact): "
                + ec.message();
            std::error_code rmec;
            std::filesystem::remove(currentInput, rmec);  // orphan scratch cleanup
            return result;
        }
        std::error_code rmec;
        std::filesystem::remove(currentInput, rmec);  // the intermediate is done
    }

    result.success = true;
    return result;
}

namespace {

// Shared implementation used by both applyWatermark (legacy) and
// applyWatermarkExt (Phase 4e). When `ext` is null, the legacy path runs
// (text mode, all pages). When non-null, it controls mode/image/range.
void applyWatermarkImpl(
    const std::shared_ptr<pdfe::core::MuPdfContext>& context_,
    const std::filesystem::path& file,
    const std::filesystem::path& outputFile,
    const WatermarkConfig& config,
    const WatermarkExtConfig* ext)
{
    // Refuse UNC / network-scheme inputs before pdf_open_document. The
    // public BatchEngine::applyWatermark{,Ext} entry points reach here
    // without going through Document::open (so Phase-1's gate does not
    // fire); add the gate here to close the bypass.
    if (pdfe::core::path_security::is_unc_or_remote(file.string())) {
        throw std::runtime_error(
            "Refusing UNC / network-scheme watermark input: " + file.string());
    }

    const bool useImage = ext && ext->mode == "image" && !ext->imagePath.empty();

    auto pageMatchesExt = [&](int pageIndex, int totalPages) -> bool {
        if (pageIndex < 0 || pageIndex >= totalPages) return false;
        if (!ext) return true;
        const int oneBased = pageIndex + 1;
        const std::string& rm = ext->rangeMode;
        if (rm == "all" || rm.empty()) return true;
        if (rm == "odd")  return (oneBased % 2) == 1;
        if (rm == "even") return (oneBased % 2) == 0;
        if (rm == "explicit") {
            return std::find(ext->explicitPages.begin(),
                             ext->explicitPages.end(),
                             pageIndex) != ext->explicitPages.end();
        }
        return true;
    };

    context_->execute([&](fz_context* ctx) {
        pdf_document* doc = pdf_open_document(ctx, file.string().c_str());

        // F24 REMEDIATION (security-audit gate, finding 1). A watermark is a
        // page CONTENT-STREAM edit — DocMdpChangeClass::Other, which ISO
        // 32000-1 Table 254 permits at no certification level. Before this
        // check, a batch watermark aimed at a certified document overwrote it
        // in place with no refusal and no warning, because this engine never
        // goes through pdfe::core::Document and so never met the save guard.
        //
        // Same policy as the Document-side guard: refuse in place, warn and
        // proceed when writing elsewhere.
        {
            char refusal[512] = {0};
            if (pdfe::signatures::directWriteRefused(
                    ctx, doc, file, outputFile,
                    pdfe::signatures::DocMdpChangeClass::Other,
                    refusal, sizeof(refusal))) {
                pdf_drop_document(ctx, doc);
                fz_throw(ctx, FZ_ERROR_ARGUMENT, "%s", refusal);
            }
        }

        fz_image* wmImage = nullptr;
        pdf_obj* wmImageObj = nullptr;
        if (useImage) {
            fz_try(ctx) {
                wmImage = fz_new_image_from_file(ctx, ext->imagePath.c_str());
                wmImageObj = pdf_add_image(ctx, doc, wmImage);
            } fz_catch(ctx) {
                if (wmImage) { fz_drop_image(ctx, wmImage); wmImage = nullptr; }
                pdf_drop_document(ctx, doc);
                fz_rethrow(ctx);
            }
        }

        int pageCount = pdf_count_pages(ctx, doc);
        for (int i = 0; i < pageCount; ++i) {
            if (!pageMatchesExt(i, pageCount)) continue;

            pdf_page* page = pdfe::core::safeLoadPdfPage(ctx, doc, i);
            fz_rect mediabox = pdf_bound_page(ctx, page, FZ_MEDIA_BOX);
            const float pw = mediabox.x1 - mediabox.x0;
            const float ph = mediabox.y1 - mediabox.y0;
            // Centre on the page in ABSOLUTE user space, honouring a non-zero
            // MediaBox origin. pdf_bound_page NORMALISES the box to (0,0), but
            // the content coordinate system is absolute, so read the raw
            // MediaBox origin from the page dict (otherwise the watermark is
            // offset by the origin on imposed / cropped pages).
            float mbOx = 0.0f, mbOy = 0.0f;
            {
                pdf_obj* mbArr = pdf_dict_get_inheritable(
                    ctx, page->obj, PDF_NAME(MediaBox));
                if (mbArr && pdf_array_len(ctx, mbArr) == 4) {
                    mbOx = pdf_to_real(ctx, pdf_array_get(ctx, mbArr, 0));
                    mbOy = pdf_to_real(ctx, pdf_array_get(ctx, mbArr, 1));
                }
            }
            const float cx = mbOx + pw / 2.0f;
            const float cy = mbOy + ph / 2.0f;

            char buf[2048];
            if (useImage) {
                // Image watermark — draw via Image XObject centered on page.
                // Target image rendered at 40% of page width, preserving aspect.
                const int imgW = (wmImage->w > 0) ? wmImage->w : 100;
                const int imgH = (wmImage->h > 0) ? wmImage->h : 100;
                float targetW = pw * 0.40f;
                float targetH = targetW * ((float)imgH / (float)imgW);
                float x = cx - targetW / 2.0f;
                float y = cy - targetH / 2.0f;
                snprintf(buf, sizeof(buf),
                    "q /GS0 gs %.2f 0 0 %.2f %.2f %.2f cm /WMImg Do Q",
                    targetW, targetH, x, y);
            } else {
                // Text watermark — render in fill mode, centered. Rotation
                // is applied via the text matrix (Tm). The Phase 4
                // implementation used "%.1f Tr" with the rotation value as
                // text-rendering-mode which left text invisible (Tr values
                // outside [0,7] are effectively no-op/invisible in MuPDF
                // extraction). That was a latent bug; now fixed so the
                // watermark is actually visible and extractable.
                const float rad = config.rotation * 3.14159265f / 180.0f;
                const float cr = std::cos(rad);
                const float sr = std::sin(rad);
                // Reset ambient text state to a known basis (100 Tz, 0 Tc/Tw/Ts/
                // Tr) so an ambient -100 Tz can't mirror the watermark; the
                // "0.5 g" grey below still overrides the basis's black fill.
                const std::string wmBasis = pdfe::core::knownTextStateBasis();
                snprintf(buf, sizeof(buf),
                    "q %s0.5 g /GS0 gs BT /F1 %.1f Tf "
                    "%.4f %.4f %.4f %.4f %.2f %.2f Tm "
                    "(%s) Tj ET Q",
                    wmBasis.c_str(),
                    config.fontSize,
                    cr, sr, -sr, cr,
                    cx - config.fontSize * 2, cy,
                    config.text.c_str());
            }

            pdf_obj* resources = pdf_dict_get(ctx, page->obj, PDF_NAME(Resources));
            if (!resources) {
                resources = pdf_new_dict(ctx, reinterpret_cast<pdf_document*>(doc), 2);
                pdf_dict_put(ctx, page->obj, PDF_NAME(Resources), resources);
            }

            // Graphics state for opacity
            pdf_obj* extgstate = pdf_dict_get(ctx, resources, PDF_NAME(ExtGState));
            if (!extgstate) {
                extgstate = pdf_new_dict(ctx, reinterpret_cast<pdf_document*>(doc), 1);
                pdf_dict_put(ctx, resources, PDF_NAME(ExtGState), extgstate);
            }
            pdf_obj* gs0 = pdf_new_dict(ctx, reinterpret_cast<pdf_document*>(doc), 2);
            pdf_dict_put_real(ctx, gs0, PDF_NAME(ca), config.opacity);
            pdf_dict_put_real(ctx, gs0, PDF_NAME(CA), config.opacity);
            pdf_dict_puts(ctx, extgstate, "GS0", gs0);

            // Text-mode font resource hookup
            if (!useImage) {
                ensureF1Helvetica(ctx, doc, page);
            }

            // Image resource hookup
            if (useImage && wmImageObj) {
                pdf_obj* xobj = pdf_dict_get(ctx, resources, PDF_NAME(XObject));
                if (!xobj) {
                    xobj = pdf_new_dict(ctx, reinterpret_cast<pdf_document*>(doc), 1);
                    pdf_dict_put(ctx, resources, PDF_NAME(XObject), xobj);
                }
                pdf_dict_puts(ctx, xobj, "WMImg", wmImageObj);
            }

            // Append so the watermark runs at identity CTM even on N-up /
            // imposed pages (image watermark places via an absolute `cm`; the
            // guard is a harmless balanced no-op for the text watermark).
            pdfe::core::appendContentIsolatingAmbientCtm(
                ctx, reinterpret_cast<pdf_document*>(doc), page->obj,
                buf, strlen(buf));

            fz_drop_page(ctx, reinterpret_cast<fz_page*>(page));
        }

        if (wmImage) fz_drop_image(ctx, wmImage);
        pdf_save_document(ctx, doc, outputFile.string().c_str(), nullptr);
        pdf_drop_document(ctx, doc);
    });
}

} // namespace

void BatchEngine::applyWatermark(
    const std::filesystem::path& file,
    const std::filesystem::path& outputFile,
    const WatermarkConfig& config)
{
    applyWatermarkImpl(context_, file, outputFile, config, nullptr);
}

void BatchEngine::applyWatermarkExt(
    const std::filesystem::path& file,
    const std::filesystem::path& outputFile,
    const WatermarkConfig& base,
    const WatermarkExtConfig& ext)
{
    applyWatermarkImpl(context_, file, outputFile, base, &ext);
}

void BatchEngine::applyFlattenAnnotations(
    const std::filesystem::path& file,
    const std::filesystem::path& outputFile)
{
    auto doc = pdfe::core::Document::open(context_, file);
    if (!doc) throw std::runtime_error("Failed to open document");

    // PDF-53: this used to call applyPageRedactions() once per page with NO
    // redaction marks present. pdf_redact_page() with zero redaction
    // annotations does nothing, so "Flatten Annotations" was a silent no-op —
    // every annotation survived in the output. Bake them for real.
    pdfe::redaction::RedactionManager mgr(context_);
    mgr.flattenAnnotations(doc.get());

    doc->save(outputFile);
}

void BatchEngine::applyConvertToPdfA(
    const std::filesystem::path& file,
    const std::filesystem::path& outputFile,
    const PdfAConfig& config)
{
    auto doc = pdfe::core::Document::open(context_, file);
    if (!doc) throw std::runtime_error("Failed to open document");

    pdfe::compliance::ComplianceManager mgr(context_);

    pdfe::compliance::PdfALevel level = pdfe::compliance::PdfALevel::PdfA_2b;
    if (config.level == 1) {
        level = (config.conformance == 'a')
            ? pdfe::compliance::PdfALevel::PdfA_1a
            : pdfe::compliance::PdfALevel::PdfA_1b;
    } else if (config.level == 2) {
        if (config.conformance == 'a') level = pdfe::compliance::PdfALevel::PdfA_2a;
        else if (config.conformance == 'u') level = pdfe::compliance::PdfALevel::PdfA_2u;
        else level = pdfe::compliance::PdfALevel::PdfA_2b;
    } else if (config.level == 3) {
        level = pdfe::compliance::PdfALevel::PdfA_3b;
    }

    auto result = mgr.convertToPdfA(doc.get(), level, outputFile);
    if (!result.success && result.hasErrors()) {
        throw std::runtime_error("PDF/A conversion failed: " + result.errorMessage);
    }
}

void BatchEngine::applySetPermissions(
    const std::filesystem::path& file,
    const std::filesystem::path& outputFile,
    const PermissionsConfig& config)
{
    auto doc = pdfe::core::Document::open(context_, file);
    if (!doc) throw std::runtime_error("Failed to open document");

    pdfe::encryption::EncryptionSettings settings;
    settings.algorithm = pdfe::encryption::EncryptionAlgorithm::AES_256;
    settings.ownerPassword = config.ownerPassword;
    settings.permissions.print = config.allowPrint;
    settings.permissions.copy = config.allowCopy;
    settings.permissions.modify = config.allowEdit;
    settings.permissions.annotate = config.allowEdit;
    settings.permissions.formFill = config.allowEdit;
    settings.permissions.assemble = config.allowEdit;

    pdfe::encryption::EncryptionManager encMgr(context_);
    encMgr.saveEncrypted(doc.get(), outputFile, settings);
}

} // namespace pdfe::batch
