#include "TextEditCommands.h"
#include "ContentStreamEditor.h"
#include "fonts/FontFamilyResolver.h"
#include "fonts/FontSubstitutionEngine.h"
#include "fonts/PdfFontEmbedder.h"
#include "fonts/VectorSubsetDetector.h"
#include "tre/AutoMissingGlyphResolver.h"
#include "core/Document.h"
#include "core/Page.h"
#include "core/MuPdfContext.h"

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

#include <QDebug>
#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

namespace pdfe::textedit {

pdfe::fonts::SubstitutionInfo
buildSubstitutionFromFormattingRuns(
    std::vector<FormattingRun> fmtRuns,
    const std::unordered_map<std::string, std::string>& pathToResName,
    const TextSpan& spanInfo)
{
    // Sort by charStart so the emission walk is deterministic. The
    // model layer does not validate input ordering — callers may pass
    // unsorted runs (e.g., when reconstructed from a clipboard paste).
    std::sort(fmtRuns.begin(), fmtRuns.end(),
        [](const FormattingRun& a, const FormattingRun& b) {
            return a.charStart < b.charStart;
        });

    pdfe::fonts::SubstitutionInfo sub;
    sub.spanIndex = 0;
    sub.horizontalScale = 1.0f;
    sub.originalFontSize = spanInfo.fontSize;
    sub.originalFontName = spanInfo.fontName;

    for (const auto& run : fmtRuns) {
        pdfe::fonts::RunSegment seg;
        seg.charStart = run.charStart;
        seg.charEnd = run.charEnd;
        seg.fontSize = (run.fontSize > 0.0f)
            ? run.fontSize : spanInfo.fontSize;

        if (!run.preInstalledResName.empty()) {
            // Pre-installed PDF font (e.g. Type 3 bundle from
            // SynthesizedType3FontBundler) — caller has already added it
            // to the page's /Font dict. Reference it directly without
            // going through the embed path.
            seg.fontResName = run.preInstalledResName;
        } else if (!run.fontFilePath.empty()) {
            auto resIt = pathToResName.find(run.fontFilePath);
            seg.fontResName = (resIt != pathToResName.end())
                ? resIt->second : spanInfo.fontName;
        } else {
            seg.fontResName = spanInfo.fontName;
        }

        sub.runSegments.push_back(std::move(seg));
    }

    if (!sub.runSegments.empty()) {
        sub.substituteResName = sub.runSegments.front().fontResName;
    }

    return sub;
}

EditTextCommand::EditTextCommand(
    pdfe::core::Document* document,
    std::shared_ptr<pdfe::core::MuPdfContext> context,
    int pageIndex,
    int spanIndex,
    const TextSpan& spanInfo,
    const std::u32string& oldText,
    const std::u32string& newText)
    : document_(document)
    , context_(std::move(context))
    , pageIndex_(pageIndex)
    , spanIndex_(spanIndex)
    , spanInfo_(spanInfo)
    , oldText_(oldText)
    , newText_(newText)
    , newFormattingRuns_(spanInfo.formattingRuns)
{
    // oldFormattingRuns_ is empty (original state before any formatting)
}

EditTextCommand::~EditTextCommand()
{
    dropContentsSnapshot();
}

void EditTextCommand::execute()
{
    spanInfo_.formattingRuns = newFormattingRuns_;
    // Pipeline F phase-2: snapshot the page /Contents BEFORE the edit
    // when newText is empty. The find-and-replace pipeline can only
    // modify existing text runs — once the run is gone, undo's
    // applyText(oldText_) has no anchor to inject into. The snapshot
    // is the only way to losslessly restore the empty-replacement
    // case. Non-empty edits use the existing applyText path on undo,
    // which preserves merge semantics for typing.
    if (newText_.empty()) {
        captureContentsSnapshot();
    }
    applyText(newText_);
    executed_ = true;
}

void EditTextCommand::undo()
{
    spanInfo_.formattingRuns = oldFormattingRuns_;
    if (contentsSnapshot_) {
        // Empty-replacement undo path: restore /Contents directly.
        restoreContentsSnapshot();
    } else {
        // Non-empty edit: ContentStreamEditor's text-filter callback
        // matches by bbox, and a non-empty newText_ leaves a text run
        // at that bbox (just with different glyphs). On undo, the
        // anchor still exists for the filter to match against, so
        // applyText(oldText_) reliably re-emits the original glyph
        // sequence. No snapshot needed for this path.
        applyText(oldText_);
    }
}

void EditTextCommand::redo()
{
    spanInfo_.formattingRuns = newFormattingRuns_;
    // Re-apply the original new state. If we snapshot-restored on
    // undo, applyText(newText_=="") will re-empty the span. If
    // newText_ is non-empty, the normal find-and-replace path runs.
    applyText(newText_);
}

void EditTextCommand::captureContentsSnapshot()
{
    if (contentsSnapshot_) return;  // already snapshotted
    ContentStreamEditor editor(context_.get());
    contentsSnapshot_ = editor.captureContentsSnapshot(
        document_, pageIndex_);
}

void EditTextCommand::restoreContentsSnapshot()
{
    if (!contentsSnapshot_) return;
    ContentStreamEditor editor(context_.get());
    editor.restoreContentsSnapshot(
        document_, pageIndex_, contentsSnapshot_);
}

void EditTextCommand::dropContentsSnapshot()
{
    if (!contentsSnapshot_) return;
    ContentStreamEditor editor(context_.get());
    editor.dropContentsSnapshot(contentsSnapshot_);
    contentsSnapshot_ = nullptr;
}

std::string EditTextCommand::description() const
{
    return "Edit Text";
}

int EditTextCommand::mergeId() const
{
    // All EditTextCommands on the same span can merge (for typing)
    return 1000 + spanIndex_;
}

bool EditTextCommand::mergeWith(const Command* other)
{
    auto* otherEdit = dynamic_cast<const EditTextCommand*>(other);
    if (!otherEdit)
        return false;

    // Only merge if same page and span
    if (otherEdit->pageIndex_ != pageIndex_ || otherEdit->spanIndex_ != spanIndex_)
        return false;

    // Absorb: our newText becomes the other's newText
    // (we keep our oldText and oldFormattingRuns as the original baseline)
    newText_ = otherEdit->newText_;
    newFormattingRuns_ = otherEdit->newFormattingRuns_;
    return true;
}

void EditTextCommand::applyText(const std::u32string& text)
{
    auto page = document_->page(pageIndex_);
    if (!page) return;

    ContentStreamEditor editor(context_.get());

    // Build a span vector with just our span (at index 0)
    std::vector<TextSpan> spans = { spanInfo_ };

    // The modification targets span 0 in our local vector
    std::vector<std::pair<int, std::u32string>> mods;
    mods.emplace_back(0, text);

    // Route per-span, not per-page (defects #1 + #5). The decision is made
    // from the provenance of the SPAN being edited, never from a page-wide
    // image-area or any-font-subsetted verdict:
    //
    //   #1: a span whose glyphs originate from an OCR invisible-text layer
    //       (glyphless placeholder font over a scanned bitmap) keeps the
    //       Tr=3 invisible path — MainWindow's ImageReplacer paints the
    //       visible glyphs into the bitmap layer. A span drawn with a real
    //       font is genuine vector text and takes the visible path regardless
    //       of how much of the page is image area. This stops a real heading
    //       (e.g. a bold "Invoice") from vanishing when edited on an
    //       image-heavy page.
    //
    //   #5: the auto-resolution / Type 3 synthesis path runs only when THIS
    //       span's own font is subsetted (and may lack a typed codepoint).
    //       A span using a complete font (e.g. Arial) never pays for the
    //       expensive path just because some OTHER font on the page is
    //       subsetted.
    const bool ocrInvisible = spanInfo_.isOcrInvisibleLayer();
    const bool spanFontSubsetted =
        pdfe::fonts::VectorSubsetDetector::hasSubsetPrefix(spanInfo_.fontName);
    qDebug() << "[TextEdit] applyText: page=" << pageIndex_
             << "ocrInvisibleLayer=" << ocrInvisible
             << "spanFontSubsetted=" << spanFontSubsetted
             << "span.fontName=" << QString::fromStdString(spanInfo_.fontName)
             << "span.fontSize=" << spanInfo_.fontSize
             << "formattingRuns=" << spanInfo_.formattingRuns.size();
    if (ocrInvisible) {
        qDebug() << "[TextEdit] applyText: using Tr=3 invisible path";
        editor.applyEdits(page.get(), document_, spans, mods, /*textRenderMode=*/3);
    } else {
        // Check if we have formatting runs that require font embedding
        auto fmtRuns = spanInfo_.formattingRuns;  // copy so we can fill missing fontFilePath
        std::vector<pdfe::fonts::SubstitutionInfo> substitutions;

        // Pipeline G phase-6 (now per-span, defect #5): when the SPAN's own
        // embedded font is subsetted and the user supplied no per-character
        // formatting (typical typing case), auto-resolve unencodable
        // codepoints in the new text. The resolver picks Type 3 PDF fonts
        // (synthesized glyphs preserve the embedded font's visual identity)
        // when the EnrichedAtlas can supply the codepoint, and falls back to
        // a metric-compatible system font otherwise. The returned runs are
        // appended to `fmtRuns` so the existing
        // `buildSubstitutionFromFormattingRuns` path below handles emission
        // unchanged. Gating on the span's OWN subset prefix (not a page-wide
        // "any font subsetted" verdict) keeps complete-font spans out of this
        // expensive path.
        //
        // phase-75 (WIN-49 prerequisite): the `spanFontSubsetted` gate is why
        // the resolver never ran for CJK. It was written for the subset-font
        // case — an embedded font whose glyphs were stripped — and it is
        // correct for that. But the commonest document in existence references
        // a NON-embedded standard-14 font (Helvetica, Times-Roman), which is
        // not subsetted, so the gate was false, the resolver never ran, no
        // substitute font was embedded, and ContentStreamEditor's simple-font
        // encoder wrote '?' for every CJK codepoint. The path was reachable in
        // principle and unreachable for exactly the input that needed it.
        //
        // Any codepoint at or above U+0100 is outside every simple (single-
        // byte) PDF font's encoding by construction, so its presence is a
        // sufficient reason to ask the resolver regardless of subsetting.
        // Below that, nothing changes: Latin-1 still encodes directly and the
        // gate behaves exactly as before.
        const bool textNeedsBeyondSimpleEncoding =
            std::any_of(text.begin(), text.end(),
                        [](char32_t cp) { return cp >= 256; });
        if (fmtRuns.empty() &&
            (spanFontSubsetted || textNeedsBeyondSimpleEncoding) &&
            !text.empty())
        {
            auto autoRuns = pdfe::tre::AutoMissingGlyphResolver::resolve(
                context_.get(), page.get(), document_,
                spanInfo_.fontName, spanInfo_.fontSize, text,
                spanInfo_.isItalic, spanInfo_.isBold);
            if (!autoRuns.empty()) {
                qDebug() << "[TextEdit] applyText: AutoMissingGlyphResolver "
                            "produced" << autoRuns.size()
                         << "runs for unencodable codepoints in text="
                         << QString::fromUcs4(
                                reinterpret_cast<const char32_t*>(text.data()),
                                static_cast<int>(text.size()));
                fmtRuns = std::move(autoRuns);
            }
        }

        if (!fmtRuns.empty()) {
            // D8-1 full-coverage gap fill. The per-run emission walk in
            // applyEditsWithSubstitution emits ONLY the character indices a
            // run segment covers and silently drops the rest (there is no
            // segment for them, and no repositioning, so survivors collapse
            // leftward). When the substitution runs cover just a SUBSET of
            // the text — e.g. AutoMissingGlyphResolver returns runs only for
            // the subset font's stripped glyphs (Q,u,t in "Quote"), leaving
            // the chars the embedded font still renders (o,e) uncovered —
            // those uncovered chars vanish and the edited word collapses
            // (the shipped "Invoice" heading bug). Fill the gaps with runs
            // carrying no font override; buildSubstitutionFromFormattingRuns
            // maps an override-less run to the span's ORIGINAL font, so the
            // segments cover [0,len) contiguously and every character emits.
            {
                std::sort(fmtRuns.begin(), fmtRuns.end(),
                    [](const FormattingRun& a, const FormattingRun& b) {
                        return a.charStart < b.charStart;
                    });
                std::vector<FormattingRun> filled;
                filled.reserve(fmtRuns.size() * 2 + 1);
                const int textLen = static_cast<int>(text.size());
                int cursor = 0;
                auto addGap = [&](int s, int e) {
                    if (e <= s) return;
                    FormattingRun gap;
                    gap.charStart = s;
                    gap.charEnd = e;
                    gap.fontSize = spanInfo_.fontSize;  // original size
                    // No fontFilePath / fontFamily / preInstalledResName →
                    // buildSubstitutionFromFormattingRuns falls back to the
                    // span's original embedded font for this range.
                    filled.push_back(gap);
                };
                for (const auto& r : fmtRuns) {
                    if (r.charStart > cursor) addGap(cursor, r.charStart);
                    filled.push_back(r);
                    cursor = std::max(cursor, r.charEnd);
                }
                addGap(cursor, textLen);
                fmtRuns = std::move(filled);
            }

            // UI-TE-SAVE-01 defensive resolution: a FormattingRun built from
            // toolbar input may carry a fontFamily without fontFilePath
            // (e.g., when the upstream caller forgot to invoke the resolver).
            // Without a path, embedFont below silently skips the run and the
            // family change never reaches the saved content stream. Resolve
            // here so the contract "fontFamily set ⇒ family change persists"
            // holds regardless of whether the caller pre-resolved the path.
            pdfe::fonts::FontFamilyResolver resolver;
            for (auto& run : fmtRuns) {
                if (run.fontFilePath.empty() && !run.fontFamily.empty()) {
                    run.fontFilePath = resolver.resolve(
                        run.fontFamily, run.bold, run.italic);
                    if (!run.fontFilePath.empty()) {
                        qDebug() << "[TextEdit] applyText: resolved missing fontFilePath for "
                                 << QString::fromStdString(run.fontFamily)
                                 << " -> " << QString::fromStdString(run.fontFilePath);
                    }
                }
            }

            // Embed unique fonts from formatting runs
            pdfe::fonts::PdfFontEmbedder embedder(context_.get());
            std::unordered_map<std::string, std::string> pathToResName;

            for (const auto& run : fmtRuns) {
                if (run.fontFilePath.empty()) continue;
                if (pathToResName.count(run.fontFilePath)) continue;

                auto info = embedder.embedFont(page.get(), document_, run.fontFilePath);
                if (info.has_value()) {
                    pathToResName[run.fontFilePath] = info->resourceName;
                }
            }

            // UI-TE-SAVE-08 phase-3: build one RunSegment per FormattingRun
            // (delegated to buildSubstitutionFromFormattingRuns so tests can
            // verify the contract without driving a full PDF save). Replaces
            // the dominant-run shortcut that lost all but one font in
            // multi-format spans. Runs without a resolved/embedded path fall
            // back to the span's original font — never silently dropped.
            substitutions.push_back(
                buildSubstitutionFromFormattingRuns(
                    fmtRuns, pathToResName, spanInfo_));
        }

        // Note: Previously this branch added an automatic Tz (horizontal
        // text-scale) to compress/stretch the new text to fit the original
        // line width — i.e., charRatio = originalLen / newLen applied as
        // horizontalScale. That made saved text look horribly squashed when
        // the user added many characters and absurdly stretched when they
        // removed many. Editing semantics are "type natural text" — the
        // saved glyphs should render at the embedded font's native widths
        // regardless of length change. We let the line overflow or fall
        // short, same as any other text editor; future work could add an
        // optional re-flow but the default must be natural advance widths.
        //
        // For vector PDFs the embedded source font is reused directly via
        // the no-substitution path in ContentStreamEditor — no Tz, no font
        // swap. Glyphs the embedded subset doesn't contain render as
        // .notdef (a subset-extension pass is the long-term fix).

        editor.applyEditsWithSubstitution(page.get(), document_, spans, mods, substitutions);
    }

    // Mark page dirty so Document::createDisplayList() drops cached page tree
    document_->markPageDirty(pageIndex_);
}

// ─── ApplyFormattingCommand ───────────────────────────────────────

ApplyFormattingCommand::ApplyFormattingCommand(
    TextEditModel* model,
    int spanIndex,
    std::vector<FormattingRun> oldRuns,
    std::vector<FormattingRun> newRuns,
    std::string description)
    : model_(model)
    , spanIndex_(spanIndex)
    , oldRuns_(std::move(oldRuns))
    , newRuns_(std::move(newRuns))
    , description_(std::move(description))
{
}

void ApplyFormattingCommand::execute() {
    // Idempotent re-apply. The mutation has already happened in
    // TextEditTool::applyFormattingField at construction-site time,
    // so this is effectively a no-op on the first invocation. Kept
    // for redo-after-undo correctness — the model is restored to
    // newRuns_ regardless of what oldRuns_ left behind.
    applyRuns(newRuns_);
}

void ApplyFormattingCommand::undo() {
    applyRuns(oldRuns_);
}

void ApplyFormattingCommand::redo() {
    applyRuns(newRuns_);
}

std::string ApplyFormattingCommand::description() const {
    return description_;
}

void ApplyFormattingCommand::applyRuns(const std::vector<FormattingRun>& runs) {
    if (!model_) return;
    if (spanIndex_ < 0 || spanIndex_ >= model_->spanCount()) return;
    model_->mutableSpan(spanIndex_).formattingRuns = runs;
}

} // namespace pdfe::textedit
