#pragma once

#include "TextEditModel.h"
#include "tre/TreTypes.h"
#include "tre/EnrichedAtlas.h"
#include <QFont>
#include <QObject>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QTimer>
#include <functional>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace pdfe::fonts {
class FontFamilyResolver;
class EmbeddedFontAnalyzer;
}

class QMouseEvent;
class QKeyEvent;
class QPainter;
class QScreen;
class QWidget;

namespace pdfe::core {
class Document;
class MuPdfContext;
}

namespace pdfe::textedit {

class ContentStreamEditor;

/// Reliable mapping from PDF font names to Qt family names.
/// Qt's font alias system is unreliable on Windows (e.g. QFont("Helvetica")
/// silently resolves to a serif font). This table maps normalized PDF names
/// to known-good Qt family names.
/// @param strippedName PDF font name with subset prefix already removed
/// @return Mapped Qt family name, or empty string if no mapping found
const QString& mapPdfFontToQtFamily(const QString& strippedName);

/// Interactive tool for selecting and editing text on a PDF canvas.
///
/// Follows the AnnotationTool interface pattern from Phase 3.
/// Click selects a span, double-click selects a word, typing modifies text.
class TextEditTool : public QObject {
    Q_OBJECT

public:
    explicit TextEditTool(
        pdfe::core::Document* document,
        std::shared_ptr<pdfe::core::MuPdfContext> context,
        QObject* parent = nullptr
    );
    ~TextEditTool() override;

    // Tool lifecycle
    void activate();
    void deactivate();
    void setPage(int pageIndex);

    // Mouse events (pdfPoint is in PDF coordinates)
    void onMousePress(QMouseEvent* event, int pageIndex, QPointF pdfPoint);
    void onMouseMove(QMouseEvent* event, int pageIndex, QPointF pdfPoint);
    void onMouseRelease(QMouseEvent* event, int pageIndex, QPointF pdfPoint);
    void onMouseDoubleClick(QMouseEvent* event, int pageIndex, QPointF pdfPoint);

    // Keyboard
    void onKeyPress(QKeyEvent* event);
    void onTextInput(const QString& text);

    // ─── Input-method (IME) support — WIN-49, follow-up phase-76 ────────────
    //
    // Preedit ("composition") text is held HERE, deliberately OUTSIDE
    // TextEditModel.  onKeyPress snapshots the active span's text before the
    // key and calls pushIncrementalEdit() on ANY difference afterwards, so a
    // preedit written into the model would push one undo command per keystroke
    // of a composition the user has not committed and may still cancel.
    // Keeping the composition out of the model makes that unrepresentable
    // rather than merely avoided: setPreeditText has no path to
    // TextEditModel::replaceCharRange at all.
    //
    // Only commitPreedit() mutates the document, and it does so through the
    // ordinary insert path, so committed IME text carries the same undo/redo
    // semantics as typed text.

    /// Store the in-flight composition. Never touches the model and never
    /// pushes an undo command. @p cursorInPreedit is the IME's caret offset
    /// within @p text (clamped). No-op when not editing.
    void setPreeditText(const std::u32string& text, int cursorInPreedit);

    /// Drop the in-flight composition without touching the document.
    void clearPreedit();

    /// Commit @p text, first deleting @p replaceLength characters starting
    /// @p replaceFrom characters from the caret (Qt's QInputMethodEvent
    /// replacement semantics, which are relative to the current cursor and may
    /// be negative). Clears the preedit, then routes through the normal
    /// insert path so undo/redo behave exactly as for typed input.
    void commitPreedit(const std::u32string& text,
                       int replaceFrom, int replaceLength);

    [[nodiscard]] const std::u32string& preeditText() const { return preeditText_; }
    [[nodiscard]] int preeditCursor() const { return preeditCursor_; }
    [[nodiscard]] bool hasPreedit() const { return !preeditText_.empty(); }

    /// Active span text, for Qt::ImSurroundingText. Empty when not editing.
    [[nodiscard]] std::u32string surroundingText() const;

    /// Caret index within surroundingText(), for Qt::ImCursorPosition.
    [[nodiscard]] int cursorPositionForIme() const;

    /// Caret rectangle in SCREEN (widget) coordinates as captured by the most
    /// recent onPaintOverlay, for Qt::ImCursorRectangle — this is the actual
    /// painted caret, not a recomputation that could drift from it. Returns
    /// false when the overlay has not painted a caret yet (headless, or no
    /// active span), leaving @p out untouched.
    [[nodiscard]] bool caretScreenRect(QRectF& out) const;

    // Paint overlay (screen coordinates; zoom and offset applied by caller)
    void onPaintOverlay(QPainter& painter, int pageIndex,
                        float zoom, QPointF pageOrigin, float pageHeightPt);

    // Commit/cancel the current edit
    void commitEdit();
    void cancelEdit();

    /// Push the current in-memory edit state to the textEdited callback
    /// without resetting activeSpanIndex_.  Used to keep the UndoManager
    /// (and the visual undoAction enabled state) in sync with the user's
    /// keyboard input on a per-character basis.  EditTextCommand's
    /// mergeId() / mergeWith() collapse the resulting commands into a
    /// single undo step for consecutive characters on the same span.
    /// Pipeline b3f19d72 phase-3 (P7-BUG-1): without this, undoAction
    /// stays disabled until commitEdit fires (Enter / span switch /
    /// blur), which contradicts user-expected typing-undo semantics.
    void pushIncrementalEdit();

    // State queries
    [[nodiscard]] bool isEditing() const { return activeSpanIndex_ >= 0; }
    [[nodiscard]] int currentPageIndex() const { return currentPageIndex_; }

    /// Index of the editable text span containing @p pdfPoint (PDF coords), or
    /// -1 when no live-font span is under the point. Used by PdfCanvas (defect
    /// #4) to decide, per click, whether a location has live vector text (route
    /// to this span tool) or not (a scanned image block — route to the region
    /// tool). Returns -1 when no model is loaded.
    [[nodiscard]] int spanIndexAtPoint(QPointF pdfPoint) const {
        return model_ ? model_->spanAtPoint(pdfPoint) : -1;
    }

    /// Read-only access to the currently-active span, or nullptr when
    /// no text-edit session is active. Used by MainWindow to hand the
    /// span's bbox + text off to AlignmentPipeline at alignment-change
    /// time.
    [[nodiscard]] const TextSpan* activeSpan() const {
        if (activeSpanIndex_ < 0 || !model_) return nullptr;
        return &model_->span(activeSpanIndex_);
    }

    /// Test seam: number of editable text spans the model detected for
    /// the current page (0 when no model / no spans).  Lets UI tests
    /// confirm the page actually has hit-testable vector-text spans
    /// before attributing a no-op edit to a product bug vs an empty page.
    [[nodiscard]] int spanCountForTesting() const {
        return model_ ? model_->spanCount() : 0;
    }

    /// Test seam: index of the currently-active (selected) span, or -1.
    [[nodiscard]] int activeSpanIndexForTesting() const {
        return activeSpanIndex_;
    }

    /// Test seam: writing mode of span i (0 = horizontal, 1 = vertical), or 0
    /// if OOR. Lets a test assert the extraction -> TextSpan writing-mode
    /// CARRIER (R2-21a) without reaching into the private model.
    [[nodiscard]] int spanWritingModeForTesting(int i) const {
        if (!model_ || i < 0 || i >= model_->spanCount()) return 0;
        return model_->span(i).writingMode;
    }

    /// Test seam: drive the REAL span-selection path (selectSpan) by index so a
    /// test can assert its side effects — notably the R2-21a SINK that sets the
    /// overlay writing mode from the span's carried writingMode. Returns false
    /// when no model / index OOR. This exercises production selectSpan(); it is
    /// the same code onMousePress reaches on a live click.
    bool selectSpanForTesting(int i) {
        if (!model_ || i < 0 || i >= model_->spanCount()) return false;
        selectSpan(i);
        return true;
    }

    /// Test seam: PDF-coordinate bounding box of span i (empty if OOR).
    [[nodiscard]] QRectF spanBboxForTesting(int i) const {
        if (!model_ || i < 0 || i >= model_->spanCount()) return {};
        return model_->span(i).boundingBox;
    }

    /// Test seam: the current text of span i as a QString (empty if OOR).
    /// Lets UI tests confirm a typed edit actually mutated the span text
    /// (the EditTextCommand pipeline) independently of canvas pixel-diff,
    /// which is unreliable for small renders / off-viewport spans.
    [[nodiscard]] QString spanTextForTesting(int i) const {
        if (!model_ || i < 0 || i >= model_->spanCount()) return {};
        const std::u32string& t = model_->span(i).text;
        return QString::fromUcs4(
            reinterpret_cast<const char32_t*>(t.data()),
            static_cast<int>(t.size()));
    }

    /// Test seam: [selectionStart, selectionEnd) within the active span.
    [[nodiscard]] int selectionStartForTesting() const { return selectionStart_; }
    [[nodiscard]] int selectionEndForTesting() const { return selectionEnd_; }

    /// Test seam: the formatting-run font size (pt) at the START of the current
    /// selection — i.e. the size applyFontSize() just wrote to the selected
    /// run. Unlike the toolbar spinbox (which emitSelectionFormatting resets to
    /// the cursor-position run, just past the selection), this reflects the
    /// size actually applied to the selection, so a native-UI test can confirm
    /// a font-size change landed before asserting the canvas re-rendered
    /// (CI-LIMITATION-WINDOWS-VALIDATION-UI-E2E-FLAKES item #2). Returns 0 when
    /// not editing / no active span.
    [[nodiscard]] float selectionRunFontSizeForTesting() const;

    // Copy prevention
    void setCopyPrevented(bool prevented) { copyPrevented_ = prevented; }

    /// Override the overlay font family for all spans on this page.
    /// Used by MainWindow to set a FontMatcher-determined family for
    /// image-dominated pages where the PDF font name is meaningless.
    /// Pass empty string to clear the override (reverts to PDF font name mapping).
    void setOverlayFontFamily(const QString& family) { overlayFontOverride_ = family; }

    /// Override the overlay font size for all spans on this page.
    /// Used by MainWindow to set a FontMatcher-detected font size for
    /// image-dominated pages where span.fontSize is from the invisible
    /// OCR text layer and not meaningful. Pass 0 to clear.
    void setOverlayFontSize(float sizePt) { overlayFontSizePtOverride_ = sizePt; }

    /// Read back the active overlay font-size override (0 when unset).
    /// Test seam (pipeline c4a9e271 Phase 2): MainWindow's textEditSelectionDiag()
    /// surfaces this so native-UI tests can deterministically verify the per-line
    /// size the clicked line resolved to actually reached setOverlayFontSize(),
    /// complementing the screenshot height-ratio proof.
    float overlayFontSizePtOverride() const { return overlayFontSizePtOverride_; }

    /// Per-word style override for overlay rendering.
    /// Defines the character range and detected formatting for a word.
    struct OverlayWordStyle {
        int charStart = 0;               ///< Character index in span text
        int charEnd = 0;                 ///< Character index (exclusive)
        pdfe::tre::WordStyleFlags style; ///< Detected bold/italic/underline/strikethrough
    };

    /// Set per-word style overrides for the overlay.
    /// Used on image-dominated pages where per-word bold/italic/underline/
    /// strikethrough detection has been performed via image pixel analysis.
    void setOverlayWordStyles(std::vector<OverlayWordStyle> styles) {
        overlayWordStyles_ = std::move(styles);
    }

    /// Set a glyph atlas for overlay rendering on image-dominated pages.
    /// When set, the overlay renders characters using cropped images from
    /// the original scan instead of system fonts, providing pixel-perfect
    /// visual consistency with the scanned document.
    void setOverlayAtlas(std::shared_ptr<pdfe::tre::EnrichedAtlas> atlas) {
        overlayEnrichedAtlas_ = std::move(atlas);
    }

    /// Set HOCR line bounding boxes (in PDF coordinates) for accurate overlay
    /// positioning on scanned pages. The PDF text layer (Tr=3 invisible text)
    /// is often positioned above the actual scan text; these HOCR-derived boxes
    /// provide the true visual text positions for correct overlay alignment.
    void setOcrLineBboxesPdf(std::vector<QRectF> bboxes) {
        ocrLineBboxesPdf_ = std::move(bboxes);
    }

    // Formatting API — applies to current selection or entire span
    void applyFontFamily(const QString& family);
    void applyFontSize(float size);
    void applyBold(bool bold);
    void applyItalic(bool italic);
    void applyUnderline(bool underline);
    void applyStrikethrough(bool strikethrough);
    void applyTextColor(const QColor& color);
    void applyAlignment(Qt::Alignment alignment);

    // Callback fired when text is committed
    std::function<void(int pageIndex, int spanIndex,
                       std::u32string oldText, std::u32string newText,
                       TextSpan spanInfo)> textEdited;

    /// UI-TE-UNDO-07: Callback fired by applyFormattingField after the
    /// model has been mutated. MainWindow uses this to construct an
    /// ApplyFormattingCommand and push it onto the UndoManager so
    /// formatting changes (bold, italic, font family/size, underline,
    /// strikethrough, text color) become reversible via Ctrl+Z. The
    /// snapshots are full-span FormattingRun vectors taken immediately
    /// before and after the model mutation. The model pointer is
    /// passed alongside so the receiver does not need a back-reference
    /// to the tool's internal model.
    std::function<void(TextEditModel* model,
                       int spanIndex,
                       std::vector<FormattingRun> oldRuns,
                       std::vector<FormattingRun> newRuns,
                       std::string description)> formattingApplied;

    /// Build the QFont used for rendering modified text overlays.
    /// Reused by text drawing, cursor positioning, and selection highlighting
    /// to ensure all three use identical metrics.
    /// @param overlayOverride  If non-empty, use this family instead of span.fontName.
    /// @param fontSizeOverride  If > 0, use this size (pt) instead of span.fontSize.
    /// @param visibleExtentRatio  Visible-glyph-extent / metric-em ratio of the
    ///     source font. Used to scale the substitute font so its rendered
    ///     glyph extent matches the surrounding body text. Caller should
    ///     query @ref visibleExtentRatioForFont with span.fontName for an
    ///     accurate value; default 0.85 matches typical Latin metrics.
    /// @param profileSerif  Serif classification from the source font's
    ///     EmbeddedFontAnalyzer FontMetricProfile (PANOSE / measured serif
    ///     extent): 1 = serif, 0 = sans, -1 = unknown. When >= 0 it drives the
    ///     serif-vs-sans fallback family choice (used when the requested family
    ///     misresolves); the font-name fragment heuristic is consulted only
    ///     when this is -1. Member callers should pass
    ///     @ref serifClassForFont(span.fontName). (pipeline d4e7f921 phase 3)
    static QFont buildOverlayFont(const TextSpan& span, float zoom,
                                   const QString& overlayOverride = {},
                                   float fontSizeOverride = 0.0f,
                                   float visibleExtentRatio = 0.85f,
                                   int profileSerif = -1);

    /// Returns the cached visible-glyph-extent / metric-em ratio for a font
    /// name on the current page. Lazily analyzes the page's embedded fonts
    /// on first call. Returns 0.85 (typical Latin) if the font isn't found
    /// in the cache (system fonts, image-replaced text, etc.).
    /// Public so internal callers and tests can query consistently.
    float visibleExtentRatioForFont(const std::string& fontName) const;

    /// Returns the source font's measured top-of-tallest-glyph from baseline
    /// divided by metric em. Used to position the overlay baseline so the
    /// substitute font's caps land where the source font's caps would have.
    /// Falls back to 0.7 (typical Latin ascender ratio) for unknown fonts.
    float ascenderRatioForFont(const std::string& fontName) const;

    /// Middle tier of @ref ascenderRatioForFont: a representative
    /// hhea-ascender / unitsPerEm ratio for well-known font families, consulted
    /// when no EmbeddedFontAnalyzer profile is cached for the font (system
    /// fonts, image-replaced text). Returns a NEGATIVE sentinel when the name
    /// matches no tabled family, so the caller can fall through to the 0.75
    /// generic default. Name-fragment matched (subset prefix tolerant), so
    /// "ABCDEF+Arial-Bold" still hits the "arial" entry. Values are approximate
    /// (vary slightly by font version) but are far closer to real Latin
    /// ascenders than the blind 0.75 default. (pipeline d4e7f921 phase 3)
    static float tabledAscenderRatio(const std::string& fontName);

    /// Serif classification cached from the page's EmbeddedFontAnalyzer
    /// FontMetricProfile (profile.isSerif, PANOSE/measured): 1 = serif,
    /// 0 = sans, -1 = unknown (font not in the cache). Feeds the @p profileSerif
    /// argument of @ref buildOverlayFont so the overlay's serif/sans fallback
    /// prefers the real classification over the font-name heuristic. Subset
    /// prefix tolerant, mirroring @ref ascenderRatioForFont. (phase 3)
    int serifClassForFont(const std::string& fontName) const;

    /// Build a QFont for a specific FormattingRun within a span.
    /// @param contextWidget  The widget the overlay is being painted on. Its
    ///     window's screen supplies the device-pixel ratio the pixel size is
    ///     compensated for (WIN-33). Pass nullptr only when there is genuinely
    ///     no widget context; the primary screen is then used as a fallback.
    static QFont buildRunFont(const FormattingRun& run, const TextSpan& span,
                               float zoom, const QString& overlayOverride = {},
                               float fontSizeOverride = 0.0f,
                               const QWidget* contextWidget = nullptr);

    /// The screen @p widget is actually displayed on — resolved through the
    /// widget's own top-level window, so on a multi-monitor desktop with mixed
    /// scaling it is the monitor the window is on and NOT necessarily the
    /// primary one (WIN-33). Returns nullptr when @p widget is null or its
    /// window has no platform handle yet (never shown); callers fall back to
    /// the primary screen in that case, matching StickyNotePopup.
    static const QScreen* screenForWidget(const QWidget* widget);

    /// Device-pixel ratio of @p screen, clamped to a sane value (a degenerate
    /// or missing ratio becomes 1.0). Pure — takes the screen, so a test can
    /// feed it any screen it can reach.
    static float devicePixelRatioForScreen(const QScreen* screen);

    /// The device-pixel ratio the overlay should compensate for when painting
    /// on @p widget: @ref screenForWidget, falling back to the primary screen
    /// only when the widget has no window handle yet.
    static float devicePixelRatioForWidget(const QWidget* widget);

    /// Pure overlay pixel-size arithmetic: point size * zoom / dpr, rounded,
    /// floored at 1. Split out from @ref buildRunFont so the DPR that a given
    /// monitor implies can be asserted without a real HiDPI screen (the same
    /// seam style as PdfCanvas::setDevicePixelRatioForTest).
    static int overlayPixelSizeForDpr(float sizePt, float zoom, float dpr);

    /// The Bold/Italic/Underline/Strikethrough state the formatting toolbar
    /// should display for the cursor position. An explicit user-applied
    /// FormattingRun covering the char wins; otherwise the span's
    /// descriptor-sourced style (TextSpan.isBold/isItalic, read from
    /// /FontDescriptor at extraction) drives the indicator, so the toolbar
    /// reflects the underlying font's real Bold/Italic BEFORE any edit —
    /// instead of defaulting to unchecked when the style is encoded only in
    /// the descriptor and not the font name. (pipeline d4e7f921 phase 2)
    struct ToolbarStyle {
        bool bold = false;
        bool italic = false;
        bool underline = false;
        bool strikethrough = false;
    };
    static ToolbarStyle resolveToolbarStyle(const FormattingRun& run,
                                            const TextSpan& span);

    // ===== Writing-mode / bidi overlay geometry (R2-21b) =====================
    //
    // The on-screen caret and selection rectangles were built entirely around a
    // horizontal, left-to-right X-axis (penX accumulation, left+advance). That
    // is geometrically wrong for vertical-writing (TTB / CJK wmode=1) text — the
    // caret must step in Y with the column stacked in X — and for right-to-left
    // (Arabic / Hebrew) text, where increasing logical index must move the caret
    // LEFT (toward the left visual edge) not right. These pure functions map a
    // logical caret index / logical char range to correct VISUAL geometry, and
    // are the single source of truth the live overlay paint consumes. They are
    // deliberately window-free and side-effect-free so the geometry can be
    // asserted directly in a unit test.

    /// Writing-mode + base-direction descriptor for the overlay caret model.
    struct OverlayGeometryParams {
        bool vertical = false;  ///< TTB vertical writing mode: caret steps in Y.
        bool rtl = false;       ///< RTL base direction: caret mirrored along X.
    };

    /// Derive the base (paragraph) direction from logical text using the shaper's
    /// UAX#9-lite bidi: RTL when the first strong character is Arabic/Hebrew.
    /// Empty / neutral-only text is LTR.
    static bool overlayBaseIsRtl(const std::u32string& logical);

    /// Visual caret line endpoints {a,b} for a logical caret boundary index.
    /// @param advances  per-logical-character advance along the writing axis, in
    ///                  screen px (advances[k] = width contributed by char k, so
    ///                  the cumulative of the first N reproduces the substring
    ///                  advance including kerning). caretAdvance() selects the
    ///                  axis per char so a vertical run consumes dy, horizontal dx.
    /// @param screenBbox  the span's screen-space bounding box.
    /// @param logicalIndex  caret boundary in [0, advances.size()].
    /// Horizontal text -> a vertical caret line (a.top .. b.bottom); vertical
    /// text -> a horizontal caret line spanning the column width.
    static std::pair<QPointF, QPointF> overlayCaretLine(
        const std::vector<float>& advances,
        const QRectF& screenBbox,
        int logicalIndex,
        OverlayGeometryParams params);

    /// Visual selection rectangle(s) for a logical [startLogical, endLogical)
    /// range. For a single-direction line this is one rect: for LTR it grows
    /// rightward from the left edge, for RTL it covers the RIGHT visual span
    /// (mirrored), and for vertical it stacks downward from the top edge.
    static std::vector<QRectF> overlaySelectionRects(
        const std::vector<float>& advances,
        const QRectF& screenBbox,
        int startLogical, int endLogical,
        OverlayGeometryParams params);

    /// Override the overlay writing mode to vertical (TTB). Defaults to false
    /// (horizontal) so all existing horizontal callers are byte-for-byte
    /// unchanged. MainWindow can set this when it detects a vertical-wmode line.
    void setOverlayVerticalMode(bool vertical) { overlayVertical_ = vertical; }
    [[nodiscard]] bool overlayVerticalMode() const { return overlayVertical_; }

signals:
    void editingStateChanged(bool isEditing);
    void repaintRequested();
    void activeSpanChanged(const QString& fontName, float fontSize);
    void selectionFormattingChanged(const QString& fontFamily, float fontSize,
                                    bool bold, bool italic,
                                    bool underline, bool strikethrough,
                                    QColor textColor = QColor(Qt::black));
    /// Emitted when a span is first clicked and the overlay atlas has not been
    /// built yet. MainWindow connects to this to build the atlas using the
    /// clicked line as the reference (deferred atlas building).
    void spanActivated(QRectF pdfBbox);

    /// Emitted on right-click when text edit is active. MainWindow builds and
    /// shows a context menu with atlas mode options.
    void contextMenuRequested(QPoint screenPos);

public:
    /// Public clipboard operations (for MainWindow menu dispatch).
    /// These do nothing when no text selection is active.
    void performCopy() { copySelection(); }
    void performCut() { cutSelection(); }
    void performPaste() { pasteAtCursor(); }
    void performSelectAll() { selectAll(); }
    bool hasTextSelection() const;
    bool hasActiveSpan() const { return activeSpanIndex_ >= 0; }

private:
    void rebuildModel();
    void selectSpan(int spanIndex);
    void insertTextAtCursor(const std::u32string& text);
    void deleteCharAtCursor(bool forward);
    void copySelection();
    void cutSelection();
    // R2-10: single copy/extract permission funnel for the text-edit tool. True
    // when the document's LIVE copy permission forbids extraction OR the user
    // enabled Prevent-Copy. Consulted by EVERY clipboard path — keyboard Ctrl+C/X,
    // the global Copy/Cut menu, and the right-click context menu — so no entry
    // point can leak a copy-restricted span to the clipboard (security-audit
    // Phase 4). copyPrevented_ alone is insufficient: it is only synced from the
    // manual toggle, never from the document permission at open.
    bool copyDenied() const;
    void pasteAtCursor();
    void selectAll();
    void resetCursorBlink();

    /// Compute character index from a PDF-space X coordinate for modified spans.
    /// Uses Qt font metrics proportional advance instead of stale charQuads.
    int charIndexFromPdfX(const TextSpan& span, double pdfX) const;

    /// Per-logical-character screen-px advances for the system-font overlay of
    /// @p span, computed as cumulative-substring deltas so the running sum
    /// reproduces QFontMetricsF::horizontalAdvance(left(N)) exactly (kerning
    /// preserved). Feeds overlayCaretLine / overlaySelectionRects. (R2-21b)
    std::vector<float> systemFontAdvances(const TextSpan& span, float zoom) const;

    /// Compute the horizontal pixel advance for the first N characters of a span
    /// using atlas glyph widths (when atlas is active) or QFontMetricsF (fallback).
    /// Used for cursor and selection positioning to match atlas rendering.
    float atlasAdvanceForChars(const TextSpan& span, int charCount,
                               float scaleToScreen, float zoom) const;

    /// Compute atlas-derived scale factor: bbox screen height / avg atlas glyph height.
    float atlasScaleToScreen(const TextSpan& span, float bboxH) const;

    /// Render span text using atlas glyph images at the HOCR-aligned position.
    /// Shared by both modified and unmodified overlay paths.
    void renderAtlasOverlay(QPainter& painter, const TextSpan& span,
                            const QRectF& atlasScreenBbox, const QRectF& screenBbox,
                            float zoom, bool logChars);

    /// Record the painted caret (screen coords) for Qt::ImCursorRectangle and
    /// draw the in-flight IME composition, underlined, starting at the caret.
    /// Called from onPaintOverlay's caret branches so the composition always
    /// tracks the caret the user can actually see.
    void noteCaretAndDrawPreedit(QPainter& painter, const QRectF& caretRect);

    void emitSelectionFormatting();
    void applyFormattingField(std::function<void(FormattingRun&)> modifier);

    /// Lazily populates @ref visibleExtentCache_ for the given page by
    /// running EmbeddedFontAnalyzer on every embedded font and computing
    /// (capHeight + |descender|) / 1.0 — the cap-top-to-descender-bottom
    /// extent of standard Latin chars. Idempotent.
    void warmFontCacheForPage(int pageIndex);

    pdfe::core::Document* document_;
    std::shared_ptr<pdfe::core::MuPdfContext> context_;
    std::unique_ptr<TextEditModel> model_;
    std::unique_ptr<pdfe::fonts::FontFamilyResolver> fontResolver_;
    std::unique_ptr<pdfe::fonts::EmbeddedFontAnalyzer> fontAnalyzer_;
    /// font postScriptName → visible-glyph-extent / metric-em ratio.
    /// Computed lazily per page; never invalidated within a session
    /// (font names are stable per document).
    mutable std::unordered_map<std::string, float> visibleExtentCache_;
    /// font postScriptName → ascender ratio (top of tallest rendered glyph
    /// from baseline / metric em). Used for baseline positioning.
    mutable std::unordered_map<std::string, float> ascenderRatioCache_;
    /// font postScriptName → serif class (1 = serif, 0 = sans). Populated from
    /// FontMetricProfile.isSerif in warmFontCacheForPage; read by
    /// serifClassForFont. Absence means "not analyzed" (-1 / unknown).
    mutable std::unordered_map<std::string, int> serifCache_;
    /// Pages whose embedded fonts have been analyzed, to avoid redundant
    /// EmbeddedFontAnalyzer::analyzePageFonts calls (which extract font
    /// streams + create FT_Faces — expensive).
    std::unordered_set<int> analyzedPages_;
    QTimer* blinkTimer_ = nullptr;
    int currentPageIndex_ = -1;
    int activeSpanIndex_ = -1;
    int cursorPosition_ = 0;
    /// In-flight IME composition. Never part of the document model — see the
    /// setPreeditText comment above for why that separation is the fix.
    std::u32string preeditText_;
    int preeditCursor_ = 0;
    /// Screen-space caret rect captured by the last onPaintOverlay; feeds
    /// Qt::ImCursorRectangle so the IME candidate window follows the caret.
    QRectF lastCaretScreenRect_;
    bool lastCaretScreenRectValid_ = false;
    int selectionStart_ = -1;
    int selectionEnd_ = -1;
    bool cursorVisible_ = true;
    bool copyPrevented_ = false;
    QString overlayFontOverride_;  ///< FontMatcher-determined family for image pages
    float overlayFontSizePtOverride_ = 0.0f;  ///< FontMatcher-detected size for image pages
    std::vector<OverlayWordStyle> overlayWordStyles_;  ///< Per-word style overrides
    std::shared_ptr<pdfe::tre::EnrichedAtlas> overlayEnrichedAtlas_;  ///< Enriched atlas for image-page overlay
    std::vector<QRectF> ocrLineBboxesPdf_;  ///< HOCR line bboxes in PDF coords (for Y-offset)
    Qt::Alignment currentAlignment_ = Qt::AlignLeft;
    bool overlayVertical_ = false;  ///< TTB vertical writing mode for the overlay (R2-21b)
    bool atlasCharLogged_ = false;  ///< One-shot per-char atlas diagnostic log
    mutable int scaleLogCount_ = 0;  ///< Throttle for atlasScaleToScreen debug logging
};

} // namespace pdfe::textedit
