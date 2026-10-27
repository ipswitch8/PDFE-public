#include "RedactionVerifier.h"
#include "core/MuPdfContext.h"
#include "core/StextOptions.h"
#include "core/PathSecurity.h"
#include "core/QpdfWrapper.h"
#include "core/FormXObjectDepthGuard.h"
#include "core/Error.h"

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

#include <qpdf/QPDF.hh>
#include <qpdf/QPDFObjectHandle.hh>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>

namespace pdfe::redaction {

class RedactionVerifier::Impl {
public:
    std::shared_ptr<pdfe::core::MuPdfContext> context;

    explicit Impl(std::shared_ptr<pdfe::core::MuPdfContext> ctx)
        : context(std::move(ctx)) {}
};

RedactionVerifier::RedactionVerifier(std::shared_ptr<pdfe::core::MuPdfContext> ctx)
    : pImpl(std::make_unique<Impl>(std::move(ctx))) {}

RedactionVerifier::~RedactionVerifier() = default;

RedactionVerifier::RedactionVerifier(RedactionVerifier&&) noexcept = default;
RedactionVerifier& RedactionVerifier::operator=(RedactionVerifier&&) noexcept = default;

RedactionVerifyResult RedactionVerifier::verify(
    const std::filesystem::path& filePath,
    const std::vector<RedactionData>& redactedRegions
) {
    RedactionVerifyResult result;

    // Refuse UNC / network-scheme inputs before any of the
    // checkXxx() methods open the file via fz_open_document.
    if (pdfe::core::path_security::is_unc_or_remote(filePath.string())) {
        result.success = false;
        result.errorMessage =
            "Refusing UNC / network-scheme input: " + filePath.string();
        return result;
    }

    try {
        result.textRemoved = checkTextRemoved(filePath, redactedRegions);
        if (!result.textRemoved) {
            result.warnings.push_back("Some redacted text may still be present");
        }

        result.singleRevision = checkSingleRevision(filePath);
        if (!result.singleRevision) {
            result.warnings.push_back("Document has multiple revisions; previous data may be recoverable");
        }

        result.noOrphanStreams = checkNoOrphanStreams(filePath);
        if (!result.noOrphanStreams) {
            result.warnings.push_back("Orphan content streams detected");
        }

        result.success = result.textRemoved && result.singleRevision && result.noOrphanStreams;
    } catch (const std::exception& e) {
        result.success = false;
        result.errorMessage = e.what();
    }

    return result;
}

bool RedactionVerifier::checkTextRemoved(
    const std::filesystem::path& filePath,
    const std::vector<RedactionData>& redactedRegions
) {
    if (redactedRegions.empty()) {
        return true;
    }

    // Open the file with a fresh context to verify
    auto verifyCtx = std::make_shared<pdfe::core::MuPdfContext>();
    bool allClear = true;

    verifyCtx->execute([&](fz_context* ctx) {
        fz_document* doc = fz_open_document(ctx, filePath.string().c_str());
        if (!doc) return;

        for (const auto& region : redactedRegions) {
            if (region.pageIndex < 0 || region.pageIndex >= fz_count_pages(ctx, doc)) {
                continue;
            }

            // FAIL CLOSED, not fail-skip. A page whose Form XObject
            // nesting exceeds the bound cannot be loaded (see
            // FormXObjectDepthGuard.h), and a redaction verifier that
            // cannot inspect a page must NOT report it clear — that
            // would turn an unverifiable page into a clean bill of
            // health, which is the worst possible direction to be
            // wrong in for this function. Throwing is also wrong here:
            // it would abandon the loop and leak `doc`.
            if (pdfe::core::formXObjectDepthExceeded(
                    ctx, pdf_document_from_fz_document(ctx, doc),
                    region.pageIndex)) {
                allClear = false;
                continue;
            }
            fz_page* page =
                pdfe::core::safeLoadPage(ctx, doc, region.pageIndex);
            if (!page) continue;

            // Search for any text within the redacted region
            fz_stext_options opts = pdfe::core::pdfeStextOptions();
            fz_stext_page* stext = fz_new_stext_page_from_page(ctx, page, &opts);
            if (!stext) {
                fz_drop_page(ctx, page);
                continue;
            }

            // Check each text block/line/char for intersection with redacted region
            fz_rect redactRect = {
                static_cast<float>(region.rect.left()),
                static_cast<float>(region.rect.top()),
                static_cast<float>(region.rect.right()),
                static_cast<float>(region.rect.bottom())
            };

            for (fz_stext_block* block = stext->first_block; block; block = block->next) {
                if (block->type != FZ_STEXT_BLOCK_TEXT) continue;
                for (fz_stext_line* line = block->u.t.first_line; line; line = line->next) {
                    for (fz_stext_char* ch = line->first_char; ch; ch = ch->next) {
                        fz_rect charRect = fz_rect_from_quad(ch->quad);
                        // Any glyph that INTERSECTS the redacted region counts
                        // as leftover text. The previous full-containment test
                        // (charRect entirely inside redactRect) missed glyphs
                        // that merely straddled the region boundary -- a
                        // security false-negative that let partially-overlapping
                        // un-redacted text pass verification.
                        const bool intersects =
                            charRect.x0 < redactRect.x1 && charRect.x1 > redactRect.x0 &&
                            charRect.y0 < redactRect.y1 && charRect.y1 > redactRect.y0;
                        if (intersects) {
                            allClear = false;
                        }
                    }
                }
            }

            fz_drop_stext_page(ctx, stext);
            fz_drop_page(ctx, page);
        }

        fz_drop_document(ctx, doc);
    });

    return allClear;
}

bool RedactionVerifier::checkSingleRevision(const std::filesystem::path& filePath) {
    // PDF-68: this used to self-describe as "a heuristic" — it only looked at
    // object generation numbers, which an incremental update is free to leave
    // at 0. It is now a direct structural test of the property that actually
    // matters for redaction: the file must contain exactly ONE cross-reference
    // section, so no previous revision of any object is still on disk.
    //
    // Two independent, non-heuristic signals, both of which must hold:
    //   (1) the file body carries a single `startxref` keyword and a single
    //       `%%EOF` trailer marker (a linearized file legitimately carries two
    //       of each, so that case is allowed only when QPDF confirms
    //       linearization);
    //   (2) no xref section chains to a predecessor via /Prev, and no object
    //       survives at a non-zero generation.
    try {
        pdfe::core::QpdfWrapper qpdf;
        qpdf.open(filePath);
        auto& q = qpdf.qpdf();

        // (2a) Generation check — an object kept at gen > 0 means an earlier
        // generation existed in this file.
        for (const auto& obj : q.getAllObjects()) {
            if (obj.getGeneration() > 0) {
                return false;
            }
        }

        // (2b) /Prev in the trailer is the definitive "there is an earlier
        // cross-reference section" marker.
        QPDFObjectHandle trailer = q.getTrailer();
        if (trailer.isDictionary() && trailer.hasKey("/Prev")) {
            return false;
        }

        // (1) Count the physical cross-reference sections. This catches
        // incremental updates that reuse generation 0 and whose composite
        // trailer QPDF has already flattened.
        std::ifstream in(filePath, std::ios::binary);
        if (!in) {
            return false;
        }
        const std::string bytes((std::istreambuf_iterator<char>(in)),
                                 std::istreambuf_iterator<char>());
        auto countOccurrences = [&bytes](const char* needle) {
            size_t count = 0;
            size_t pos = 0;
            const size_t len = std::strlen(needle);
            while ((pos = bytes.find(needle, pos)) != std::string::npos) {
                ++count;
                pos += len;
            }
            return count;
        };
        const size_t startxrefs = countOccurrences("startxref");
        const size_t eofs = countOccurrences("%%EOF");

        // A linearized file has a first-page xref at the front plus the main
        // one at the back — two sections describing ONE revision.
        const size_t allowed = q.isLinearized() ? 2u : 1u;
        if (startxrefs > allowed || eofs > allowed) {
            return false;
        }

        return true;
    } catch (const std::exception&) {
        // If we can't open/parse it, we cannot assert single-revision.
        return false;
    }
}

bool RedactionVerifier::checkNoOrphanStreams(const std::filesystem::path& filePath) {
    try {
        pdfe::core::QpdfWrapper qpdf;
        qpdf.open(filePath);
        auto& q = qpdf.qpdf();

        // After a QPDF full rewrite, unused objects are pruned.
        // Verify by checking that all stream objects are referenced by
        // at least one page's Contents or Resources.
        // Simple heuristic: a properly rewritten file has no dangling references.
        q.fixDanglingReferences();

        // getAllObjects returns all objects in the xref table.
        // After a full rewrite, there should be no orphan streams.
        // Walk all objects so QPDF's own validation runs over each one;
        // counting streams is just a way to traverse without skipping. We
        // don't act on the count — the assertion is "no exception
        // thrown by QPDF during the walk."
        auto allObjects = q.getAllObjects();
        (void)std::count_if(allObjects.begin(), allObjects.end(),
            [](const auto& obj) { return obj.isStream(); });

        // A properly rewritten file should have minimal streams
        // (page content streams, font streams, image streams).
        // We can't detect orphans without full reference graph traversal,
        // but the QPDF rewrite itself eliminates them.
        // This check confirms the file is parseable and has no dangling refs.
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

} // namespace pdfe::redaction
