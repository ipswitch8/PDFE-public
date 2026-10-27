#include "TextExtractor.h"
#include "Page.h"
#include "MuPdfContext.h"
#include "StextOptions.h"
#include "Error.h"
#include "FontNameUtil.h"

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

namespace pdfe::core {

// ─── Impl ────────────────────────────────────────────────────────────────────

class TextExtractor::Impl {
public:
    std::shared_ptr<MuPdfContext> context;

    explicit Impl(std::shared_ptr<MuPdfContext> ctx)
        : context(std::move(ctx)) {}

    /// Convert fz_rect to QRectF.
    static QRectF toQRectF(const fz_rect& r) {
        return QRectF(
            static_cast<double>(r.x0),
            static_cast<double>(r.y0),
            static_cast<double>(r.x1 - r.x0),
            static_cast<double>(r.y1 - r.y0)
        );
    }

    /// Convert fz_quad to QRectF (bounding box of the quad).
    static QRectF quadToQRectF(const fz_quad& q) {
        float x0 = std::min({q.ul.x, q.ur.x, q.ll.x, q.lr.x});
        float y0 = std::min({q.ul.y, q.ur.y, q.ll.y, q.lr.y});
        float x1 = std::max({q.ul.x, q.ur.x, q.ll.x, q.lr.x});
        float y1 = std::max({q.ul.y, q.ur.y, q.ll.y, q.lr.y});
        return QRectF(
            static_cast<double>(x0),
            static_cast<double>(y0),
            static_cast<double>(x1 - x0),
            static_cast<double>(y1 - y0)
        );
    }

    /// Extract structured text from page's fz_page handle.
    /// All MuPDF API calls are done within executeReturn to handle
    /// setjmp/longjmp error translation.
    std::vector<TextBlock> extractFromPage(fz_page* fzPage) {
        // Step 1: Get the fz_stext_page from MuPDF (inside execute scope for error handling).
        // We extract all POD data from the stext_page and convert to our types,
        // then drop the stext_page — all within the lambda to avoid leaks.
        return context->executeReturn<std::vector<TextBlock>>(
            [this, fzPage](fz_context* ctx) -> std::vector<TextBlock> {
                // Create stext page — no C++ RAII objects on stack here
                // (vector<TextBlock> return is fine since it's outside fz_try in executeReturn)
                fz_stext_page* stextPage = nullptr;
                fz_stext_options opts = pdfe::core::pdfeStextOptions();
                stextPage = fz_new_stext_page_from_page(ctx, fzPage, &opts);

                std::vector<TextBlock> result;

                // MuPDF's fz_font.name is a char[32] buffer, so fz_font_name()
                // truncates a /BaseFont longer than 31 chars (dropping any style
                // keyword past char 31 — the "long font name loses italic in the
                // edit overlay" defect). Build a per-page map of the untruncated
                // /BaseFont so each char carries the full name. Best-effort: an
                // empty map just means we keep the truncated fz_font_name.
                const BaseFontMap baseFontMap = buildBaseFontMap(ctx, fzPage);

                // Per-font italic/bold from the /FontDescriptor — the
                // name-independent style source carried onto each TextChar so
                // the edit overlay no longer depends on style keywords being
                // present (and intact) in the font name.
                const FontStyleMap fontStyleMap = buildFontStyleMap(ctx, fzPage);

                // Traverse the stext structure: blocks → lines → chars
                fz_stext_block* block = stextPage->first_block;
                while (block) {
                    if (block->type == FZ_STEXT_BLOCK_TEXT) {
                        TextBlock textBlock;
                        textBlock.boundingBox = toQRectF(block->bbox);
                        textBlock.type = TextBlockType::Paragraph;

                        fz_stext_line* line = block->u.t.first_line;
                        while (line) {
                            TextLine textLine;
                            textLine.boundingBox = toQRectF(line->bbox);
                            // R2-21a auto-plumbing SOURCE: MuPDF fz_stext_line::wmode
                            // (0 = horizontal, 1 = vertical) captured onto
                            // core::TextLine::writingMode. This is the head of the
                            // extraction -> text-edit writing-mode edge: it is carried
                            // onto textedit::TextSpan::writingMode in the TextEditModel
                            // ctor and consumed by TextEditTool::selectSpan, which drives
                            // the overlay's vertical/RTL caret geometry (R2-21b) with no
                            // manual setOverlayVerticalMode() call.
                            textLine.writingMode = line->wmode;

                            fz_stext_char* ch = line->first_char;
                            while (ch) {
                                TextChar textChar;
                                textChar.codepoint = static_cast<char32_t>(ch->c);
                                textChar.origin = QPointF(
                                    static_cast<double>(ch->origin.x),
                                    static_cast<double>(ch->origin.y)
                                );
                                textChar.quad = quadToQRectF(ch->quad);
                                textChar.fontSize = ch->size;
                                // sRGB fill color (drop the alpha in the top 8 bits).
                                // Carried through so a text edit can preserve the
                                // original glyph color instead of falling back to
                                // black / the ambient fill.
                                textChar.color = ch->argb & 0x00FFFFFFu;

                                // Extract font name, recovering the untruncated
                                // /BaseFont when MuPDF's char[32] buffer clipped
                                // it (see baseFontMap above).
                                if (ch->font) {
                                    const char* name = fz_font_name(ctx, ch->font);
                                    if (name) {
                                        const std::string fzName(name);
                                        textChar.fontName =
                                            resolveUntruncatedFontName(
                                                baseFontMap, fzName);
                                        const FontStyleFlags style =
                                            resolveFontStyle(fontStyleMap, fzName);
                                        textChar.isItalic = style.isItalic;
                                        textChar.isBold = style.isBold;
                                    }
                                }

                                textLine.chars.push_back(std::move(textChar));
                                ch = ch->next;
                            }

                            if (!textLine.chars.empty()) {
                                textBlock.lines.push_back(std::move(textLine));
                            }
                            line = line->next;
                        }

                        if (!textBlock.lines.empty()) {
                            result.push_back(std::move(textBlock));
                        }
                    } else if (block->type == FZ_STEXT_BLOCK_IMAGE) {
                        // Image block — record position but no text
                        TextBlock imageBlock;
                        imageBlock.boundingBox = toQRectF(block->bbox);
                        imageBlock.type = TextBlockType::Image;
                        result.push_back(std::move(imageBlock));
                    }

                    block = block->next;
                }

                // Drop the stext page
                fz_drop_stext_page(ctx, stextPage);

                return result;
            }
        );
    }

    /// Extract plain text from page's fz_page handle.
    std::string plainTextFromPage(fz_page* fzPage) {
        return context->executeReturn<std::string>(
            [fzPage](fz_context* ctx) -> std::string {
                fz_stext_options opts = pdfe::core::pdfeStextOptions();
                fz_stext_page* stextPage = fz_new_stext_page_from_page(ctx, fzPage, &opts);

                // Use fz_new_buffer_from_stext_page for plain text extraction
                fz_buffer* buf = fz_new_buffer_from_stext_page(ctx, stextPage);

                // Get buffer data
                unsigned char* data = nullptr;
                size_t len = fz_buffer_storage(ctx, buf, &data);
                std::string result(reinterpret_cast<const char*>(data), len);

                // Cleanup
                fz_drop_buffer(ctx, buf);
                fz_drop_stext_page(ctx, stextPage);

                return result;
            }
        );
    }
};

// ─── TextExtractor public interface ──────────────────────────────────────────

TextExtractor::TextExtractor(std::shared_ptr<MuPdfContext> context)
    : pImpl(std::make_unique<Impl>(std::move(context)))
{
}

TextExtractor::~TextExtractor() = default;

TextExtractor::TextExtractor(TextExtractor&&) noexcept = default;
TextExtractor& TextExtractor::operator=(TextExtractor&&) noexcept = default;

std::vector<TextBlock> TextExtractor::extract(const Page* page) {
    if (!page) {
        throw PdfeException("TextExtractor::extract: page is null");
    }

    // We need access to the fz_page handle from Page's pimpl.
    // Page::render() already shows the pattern: the Impl has fzPage.
    // But we can't access Page::Impl directly from here.
    // Instead, we use a rendering-based approach: extract text by having
    // Page expose its context and fz_page handle.
    //
    // Since Page::Impl is only accessible from Page.cpp, and we need fz_page*,
    // we'll use the approach of having Page provide a method that calls back
    // into TextExtractor with the fz_page*.
    //
    // For now, we expose fz_page* through Page's friend relationship.
    // The Page class already has a friend declaration for Document.
    // We'll add TextExtractor as a friend too.

    // Alternative approach: use the MuPDF API to get fz_page from document+index.
    // But that creates a new fz_page handle, which is wasteful.
    //
    // Best approach: Page exposes fzPage() accessor for internal use.
    // We'll add this in Page.h.

    // For now, we call page's internal accessor (added as part of this sub-task).
    return pImpl->extractFromPage(page->fzPage());
}

std::string TextExtractor::extractPlainText(const Page* page) {
    if (!page) {
        throw PdfeException("TextExtractor::extractPlainText: page is null");
    }

    return pImpl->plainTextFromPage(page->fzPage());
}

bool TextExtractor::hasText(const Page* page) {
    if (!page) return false;
    return page->hasTextContent();
}

} // namespace pdfe::core
