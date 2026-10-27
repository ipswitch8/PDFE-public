#pragma once

#include <memory>
#include <string>
#include <vector>
#include <utility>

namespace pdfe::core {
class MuPdfContext;
class Document;
class Page;
}

namespace pdfe::fonts {
struct SubstitutionInfo;
}

namespace pdfe::textedit {

struct TextSpan;

/// What the content-stream encoder could NOT represent in the font it was
/// given (phase-75 / WIN-49 prerequisite).
///
/// WHY THIS EXISTS. Both encoding paths used to fail silently, and silence is
/// the whole defect: a codepoint outside a simple font's single-byte encoding
/// was replaced by `'?'`, and a CID lookup that resolved to glyph 0 (.notdef)
/// was written as two raw NUL bytes. Both produce a structurally valid PDF
/// that has lost the user's text, with nothing anywhere saying so — so a CJK
/// edit "succeeded" and saved `?` or U+0000. Recording them here makes the
/// loss observable to the caller and assertable by a test; the encoder still
/// emits its best-effort bytes so the page geometry is unchanged.
struct EncodingDiagnostics {
    /// Codepoints >= 256 handed to a simple (single-byte) font. Emitted as
    /// '?' — a visible, recorded fallback rather than a silent one.
    std::vector<char32_t> unencodableSimple;
    /// Codepoints whose CID lookup resolved to 0 (.notdef) in a composite
    /// font. Emitted as the 2-byte CID 0 the PDF requires, but recorded.
    std::vector<char32_t> notdefCid;

    bool empty() const {
        return unencodableSimple.empty() && notdefCid.empty();
    }
    void clear() {
        unencodableSimple.clear();
        notdefCid.clear();
    }
};

/// Edits a PDF page's content stream to replace text.
///
/// Uses MuPDF's pdf_filter_page_contents() with a sanitize filter
/// to selectively remove old text and inject replacement text using
/// the page's existing embedded fonts.
class ContentStreamEditor {
public:
    explicit ContentStreamEditor(pdfe::core::MuPdfContext* context);
    ~ContentStreamEditor();

    /// Apply text edits to a page's content stream.
    ///
    /// @param page     The page to edit (friend access to fzPage())
    /// @param document The document (friend access to fzDocument())
    /// @param allSpans All spans from TextEditModel (needed for position/font info)
    /// @param modifications Vector of (spanIndex, newText) pairs.
    ///                      spanIndex indexes into allSpans.
    /// @param textRenderMode PDF text rendering mode to emit (-1 = don't emit Tr,
    ///                       0 = fill, 3 = invisible). Use 3 for image-dominated
    ///                       pages to keep OCR text invisible.
    void applyEdits(
        const pdfe::core::Page* page,
        const pdfe::core::Document* document,
        const std::vector<TextSpan>& allSpans,
        const std::vector<std::pair<int, std::u32string>>& modifications,
        int textRenderMode = -1
    );

    /// Apply text edits with font substitution support.
    ///
    /// When substitutions are provided, emits Tf/Tz operators for font-switched
    /// spans. For spans without substitution, emits Tf with the original font.
    /// @param textRenderMode PDF text rendering mode (-1 = don't emit, 3 = invisible)
    void applyEditsWithSubstitution(
        const pdfe::core::Page* page,
        const pdfe::core::Document* document,
        const std::vector<TextSpan>& allSpans,
        const std::vector<std::pair<int, std::u32string>>& modifications,
        const std::vector<pdfe::fonts::SubstitutionInfo>& substitutions,
        int textRenderMode = -1
    );

    /// Pipeline F phase-2: snapshot/restore the page's /Contents
    /// object. Used by EditTextCommand to losslessly undo edits that
    /// the find-and-replace pipeline cannot reverse (e.g. the
    /// empty-replacement case where the entire span is removed and
    /// the editor has no anchor to inject the original text back).
    /// Returns an opaque void* (a kept pdf_obj* reference) that must
    /// be released via dropContentsSnapshot when no longer needed.
    /// Returns nullptr on failure (no /Contents on the page, etc.).
    void* captureContentsSnapshot(
        pdfe::core::Document* document, int pageIndex);

    /// Restore a snapshot taken by captureContentsSnapshot. Safe to
    /// call with nullptr (no-op).
    void restoreContentsSnapshot(
        pdfe::core::Document* document, int pageIndex, void* snapshot);

    /// Drop a snapshot's pdf_obj reference. Safe to call with nullptr.
    void dropContentsSnapshot(void* snapshot);

    /// Codepoints the most recent applyEdits/applyEditsWithSubstitution call
    /// could not encode in the font it was given. Empty after a clean edit.
    /// Reset at the start of every apply call, so it always describes the
    /// last one rather than accumulating across edits.
    const EncodingDiagnostics& lastEncodingDiagnostics() const;

private:
    pdfe::core::MuPdfContext* context_;

    class Impl;
    std::unique_ptr<Impl> pImpl;
};

/// Output of canonicalTextMatrixForReplacement(): a (Tfs, Tm) pair that
/// reproduces the visual rendering of the matched original character in
/// PDF user-space convention. Tm is forced to the canonical [1, 0, 0,
/// -1, e, f] form used by every typeset body-text BT block; Tfs is the
/// pre-CTM font size derived from the visible point size and the active
/// CTM scale. See memory/textedit-tm-sign-bug.md for why the previous
/// `Tm = trm × ctm⁻¹ / fontSize` algebra produced upside-down output.
struct CanonicalTextMatrixResult {
    float Tfs;       ///< Pre-CTM font size to emit via op_Tf
    float Tm_a;      ///< Always +1 in the canonical form
    float Tm_b;      ///< Always 0
    float Tm_c;      ///< Always 0
    float Tm_d;      ///< Always -1 (canonical Y-flip matches surrounding)
    float Tm_e;      ///< Pre-CTM x position
    float Tm_f;      ///< Pre-CTM y position
};

/// Compute the canonical (Tfs, Tm) pair for a replacement character whose
/// original render had text-rendering-matrix `trm` under the active
/// transformation matrix `ctm`, displaying at `visibleFontSize` points
/// after the CTM scale was applied.
///
/// Inputs are 6-component MuPDF affine matrices [a, b, c, d, e, f]:
///   (x', y') = (a*x + c*y + e, b*x + d*y + f)
///
/// The function assumes a diagonal CTM (no rotation or shear). For a
/// rotated/sheared content scope it falls back to identity-style Tm,
/// which will misposition rotated text — this hasn't been exercised by
/// any production text-edit path yet.
CanonicalTextMatrixResult canonicalTextMatrixForReplacement(
    const float trm[6], const float ctm[6], float visibleFontSize);

} // namespace pdfe::textedit
