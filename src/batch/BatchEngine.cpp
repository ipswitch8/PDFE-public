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

// Watermark resources (GitHub #448, #449). The watermark never writes into a
// resource dictionary it may share with something else, and never reuses a
// resource name the page already has:
//
//  - ownPageResources() gives the page its OWN /Resources dict: a shallow copy
//    of the effective dict (possibly inherited from the /Pages node, possibly
//    shared by several pages), put on the page. Nothing is dropped, so every
//    resource the page used before is still reachable.
//  - ownCategory() does the same for one category (/ExtGState, /Font,
//    /XObject) inside it before an entry is added.
//  - freeResourceName() picks a key the category does not hold yet.
pdf_obj* ownPageResources(fz_context* ctx, pdf_document* doc, pdf_obj* pageObj)
{
    pdf_obj* effective = pdf_dict_get_inheritable(ctx, pageObj, PDF_NAME(Resources));
    pdf_obj* own = pdf_is_dict(ctx, effective) ? pdf_copy_dict(ctx, effective)
                                               : pdf_new_dict(ctx, doc, 4);
    pdf_dict_put_drop(ctx, pageObj, PDF_NAME(Resources), own);
    return own;
}

pdf_obj* ownCategory(fz_context* ctx, pdf_document* doc, pdf_obj* resources, pdf_obj* key)
{
    pdf_obj* current = pdf_dict_get(ctx, resources, key);
    pdf_obj* own = pdf_is_dict(ctx, current) ? pdf_copy_dict(ctx, current)
                                             : pdf_new_dict(ctx, doc, 2);
    pdf_dict_put_drop(ctx, resources, key, own);
    return own;
}

std::string freeResourceName(fz_context* ctx, pdf_obj* category, const char* prefix)
{
    std::string name = prefix;
    for (int n = 1; pdf_dict_gets(ctx, category, name.c_str()); ++n)
        name = std::string(prefix) + std::to_string(n);
    return name;
}

// Body of a PDF literal string: ( ) and backslash are escaped so the
// watermark text can never end the string and inject operators.
std::string pdfLiteralBody(const std::string& text)
{
    std::string out;
    out.reserve(text.size() + 8);
    for (char c : text) {
        if (c == '(' || c == ')' || c == '\\') out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

// Advance width of `text` in base-14 Helvetica (the watermark's own font) at
// `fontSize`. Falls back to an average glyph width if the font cannot load.
float helveticaWidth(fz_context* ctx, const std::string& text, float fontSize)
{
    float width = 0.0f;
    fz_font* font = nullptr;
    fz_var(font);
    fz_var(width);
    fz_try(ctx) {
        font = fz_new_base14_font(ctx, "Helvetica");
        for (unsigned char c : text)
            width += fz_advance_glyph(ctx, font, fz_encode_character(ctx, font, c), 0);
        width *= fontSize;
    }
    fz_always(ctx) {
        fz_drop_font(ctx, font);
    }
    fz_catch(ctx) {
        width = 0.55f * fontSize * static_cast<float>(text.size());
    }
    return width;
}

// Placement of a watermark item (GitHub #450, #451). The item is placed in the
// RENDERED page frame, the frame the user sees (CropBox origin and /Rotate
// applied, Y down), and taken into user space with the inverse page transform
// once, as docs/image-placement-model.md prescribes for every placement.
//
// The item is a w x h box in its own Y-up units; `localCentre` is the box
// centre in those units. It is rotated CCW (as seen) by `rotationDeg` about
// that centre, and its rotated bounding box is put at the anchor named by
// `position`: "center" (also any unknown value), "top-left", "top-right",
// "bottom-left" or "bottom-right", 5% of the shorter page side from the edges.
//
// Returns the matrix item units (Y up) -> user space.
fz_matrix watermarkPlacement(const fz_rect& view, const fz_matrix& userToView,
                             float w, float h, fz_point localCentre,
                             float rotationDeg, const std::string& position)
{
    const float pw = view.x1 - view.x0;
    const float ph = view.y1 - view.y0;
    const float rad = rotationDeg * 3.14159265f / 180.0f;
    const float aw = std::fabs(w * std::cos(rad)) + std::fabs(h * std::sin(rad));
    const float ah = std::fabs(w * std::sin(rad)) + std::fabs(h * std::cos(rad));
    const float margin = 0.05f * std::min(pw, ph);

    float cx = view.x0 + pw / 2.0f;
    float cy = view.y0 + ph / 2.0f;
    const bool left   = position == "top-left"    || position == "bottom-left";
    const bool right  = position == "top-right"   || position == "bottom-right";
    const bool top    = position == "top-left"    || position == "top-right";
    const bool bottom = position == "bottom-left" || position == "bottom-right";
    if (left)   cx = view.x0 + margin + aw / 2.0f;
    if (right)  cx = view.x1 - margin - aw / 2.0f;
    if (top)    cy = view.y0 + margin + ah / 2.0f;
    if (bottom) cy = view.y1 - margin - ah / 2.0f;

    fz_matrix m = fz_translate(-localCentre.x, -localCentre.y);
    m = fz_concat(m, fz_rotate(rotationDeg));
    m = fz_concat(m, fz_make_matrix(1, 0, 0, -1, cx, cy));   // Y up -> rendered (Y down)
    m = fz_concat(m, fz_invert_matrix(userToView));           // rendered -> user space
    return m;
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

    // Resource names and the content string live in THIS frame, not the
    // lambda's: the lambda body runs inside MuPdfContext::execute's fz_try
    // (setjmp/longjmp), and a longjmp skips the destructors of its locals
    // (tools/gate/fz_try_raii_scan.py).
    std::string gsName;
    std::string resName;
    std::string content;
    // The text watermark's ambient text-state reset (100 Tz, 0 Tc/Tw/Ts/Tr) so
    // an ambient -100 Tz can't mirror it. A pure constant, so it is built once
    // here, outside the guarded region, for the same reason as the names above.
    const std::string wmBasis = pdfe::core::knownTextStateBasis();

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

        // Everything below owns MuPDF objects. They are released in the one
        // fz_always, so a failure on any page (or in the save) still drops the
        // document, the watermark image and its PDF object, and the font.
        fz_image* wmImage = nullptr;
        pdf_obj* wmImageObj = nullptr;
        pdf_obj* wmFont = nullptr;   // the text watermark's Helvetica, made once
        fz_var(wmImage);
        fz_var(wmImageObj);
        fz_var(wmFont);
        fz_try(ctx) {
            if (useImage) {
                wmImage = fz_new_image_from_file(ctx, ext->imagePath.c_str());
                wmImageObj = pdf_add_image(ctx, doc, wmImage);
            }

            int pageCount = pdf_count_pages(ctx, doc);
            for (int i = 0; i < pageCount; ++i) {
                if (!pageMatchesExt(i, pageCount)) continue;

                pdf_page* page = pdfe::core::safeLoadPdfPage(ctx, doc, i);
                fz_try(ctx) {
                    // The rendered frame (CropBox origin, /Rotate, Y down) and the
                    // transform user space -> rendered frame. Placement happens in
                    // the rendered frame and goes back to user space once, so the
                    // watermark sits where the user sees the page (GitHub #450).
                    fz_rect box;
                    fz_matrix userToView;
                    pdf_page_transform(ctx, page, &box, &userToView);
                    const fz_rect view = fz_transform_rect(box, userToView);
                    const float viewW = view.x1 - view.x0;

                    // The page's own copy of its effective resources, so inherited
                    // and shared resources are kept and never edited in place
                    // (GitHub #449); fresh names only (GitHub #448).
                    pdf_obj* resources = ownPageResources(ctx, doc, page->obj);

                    // Graphics state for opacity, under a name the page lacks.
                    pdf_obj* extgstate = ownCategory(ctx, doc, resources, PDF_NAME(ExtGState));
                    gsName = freeResourceName(ctx, extgstate, "PdfeWmGS");
                    pdf_obj* gs = pdf_dict_puts_dict(ctx, extgstate, gsName.c_str(), 2);
                    pdf_dict_put_real(ctx, gs, PDF_NAME(ca), config.opacity);
                    pdf_dict_put_real(ctx, gs, PDF_NAME(CA), config.opacity);

                    char num[512];
                    if (useImage) {
                        // Image watermark: 40% of the rendered page width, aspect
                        // kept, rotated by config.rotation and put at
                        // config.position (GitHub #451).
                        pdf_obj* xobjects = ownCategory(ctx, doc, resources, PDF_NAME(XObject));
                        resName = freeResourceName(ctx, xobjects, "PdfeWmImg");
                        pdf_dict_puts(ctx, xobjects, resName.c_str(), wmImageObj);

                        const int imgW = (wmImage->w > 0) ? wmImage->w : 100;
                        const int imgH = (wmImage->h > 0) ? wmImage->h : 100;
                        const float w = viewW * 0.40f;
                        const float h = w * (static_cast<float>(imgH) / static_cast<float>(imgW));
                        const fz_matrix m = fz_concat(
                            fz_scale(w, h),
                            watermarkPlacement(view, userToView, w, h, fz_make_point(w / 2.0f, h / 2.0f),
                                               config.rotation, config.position));
                        snprintf(num, sizeof(num),
                            "%.4f %.4f %.4f %.4f %.4f %.4f cm ", m.a, m.b, m.c, m.d, m.e, m.f);
                        content = "q /" + gsName + " gs " + num + "/" + resName + " Do Q";
                    } else {
                        // Text watermark in its own Helvetica, under a name the
                        // page lacks: a page font that happens to be called like
                        // the old fixed name is never borrowed (GitHub #449).
                        pdf_obj* fonts = ownCategory(ctx, doc, resources, PDF_NAME(Font));
                        resName = freeResourceName(ctx, fonts, "PdfeWmF");
                        if (!wmFont) {
                            pdf_obj* f = pdf_new_dict(ctx, doc, 4);
                            pdf_dict_put(ctx, f, PDF_NAME(Type), PDF_NAME(Font));
                            pdf_dict_put(ctx, f, PDF_NAME(Subtype), PDF_NAME(Type1));
                            pdf_dict_put_name(ctx, f, PDF_NAME(BaseFont), "Helvetica");
                            pdf_dict_put_name(ctx, f, PDF_NAME(Encoding), "WinAnsiEncoding");
                            wmFont = pdf_add_object_drop(ctx, doc, f);
                        }
                        pdf_dict_puts(ctx, fonts, resName.c_str(), wmFont);

                        // Fill mode, rotation and position through the text matrix.
                        // The text box (advance width x Helvetica ascent..descent)
                        // is centred on the anchor and rotated about its centre.
                        const float fs = config.fontSize;
                        const float tw = helveticaWidth(ctx, config.text, fs);
                        const float asc = 0.718f * fs;
                        const float desc = -0.207f * fs;
                        const fz_matrix tm = watermarkPlacement(
                            view, userToView, tw, asc - desc,
                            fz_make_point(tw / 2.0f, (asc + desc) / 2.0f),
                            config.rotation, config.position);
                        // Reset ambient text state to a known basis (wmBasis, built
                        // above the guarded region); the "0.5 g" grey below still
                        // overrides the basis's black fill.
                        content = "q " + wmBasis + "0.5 g /" + gsName + " gs BT /" + resName + " ";
                        snprintf(num, sizeof(num), "%.1f Tf %.4f %.4f %.4f %.4f %.4f %.4f Tm ",
                                 fs, tm.a, tm.b, tm.c, tm.d, tm.e, tm.f);
                        content += num;
                        content += "(" + pdfLiteralBody(config.text) + ") Tj ET Q";
                    }

                    // Append so the watermark runs at identity CTM even on N-up /
                    // imposed pages (both modes place with absolute user-space
                    // matrices, which the guard makes exact).
                    pdfe::core::appendContentIsolatingAmbientCtm(
                        ctx, doc, page->obj, content.data(), content.size());
                }
                fz_always(ctx) {
                    fz_drop_page(ctx, reinterpret_cast<fz_page*>(page));
                }
                fz_catch(ctx) {
                    fz_rethrow(ctx);
                }
            }

            pdf_save_document(ctx, doc, outputFile.string().c_str(), nullptr);
        }
        fz_always(ctx) {
            pdf_drop_obj(ctx, wmFont);
            pdf_drop_obj(ctx, wmImageObj);
            fz_drop_image(ctx, wmImage);
            pdf_drop_document(ctx, doc);
        }
        fz_catch(ctx) {
            fz_rethrow(ctx);
        }
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
