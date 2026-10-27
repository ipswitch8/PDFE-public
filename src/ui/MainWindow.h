#pragma once

#include <optional>
#include <memory>
#include <vector>

class QTemporaryFile;

#include <QIcon>
#include <QMainWindow>
#include <QStringList>
#include <QRectF>
#include <QSet>
#include <QVector>
#include <QColor>
#include <QImage>
#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <cstdint>

class QAction;
class QNetworkAccessManager;   // GitHub #127 -- opt-in version check
class QLineEdit;
class QMenu;
class QToolBar;
class QComboBox;
class QLabel;
class QDockWidget;
class QActionGroup;
class QPushButton;
class QFrame;        // GitHub #60 -- active-content advisory banner
class QToolButton;   // GitHub #60 -- its dismiss control
class QDragEnterEvent;
class QDropEvent;
class QUrl;   // GitHub #18 -- MainWindow::submitCurrentFormTo

namespace pdfe::core {
    class Document;
    class MuPdfContext;
    class Page;
    class AutosaveManager;
}

namespace pdfe::commands {
    class UndoManager;
}

namespace pdfe::rendering {
    class TileCache;
    class TileRenderer;
}

namespace pdfe::annotations {
    class AnnotationManager;
    class AnnotationTool;
    class ImageStampLibrary;
    struct AnnotationData;
    enum class AnnotationType;  // opaque; complete type only needed in the .cpp
}

namespace pdfe::ui {
    class TestPipeFocusGuard;   // GitHub #303; complete type only in the .cpp
}

namespace pdfe::forms {
    class FormManager;
    class JavaScriptEngine;
    class FormDataIO;
    struct FormFieldData;
    enum class FormFieldType;  // opaque; complete type only needed in the .cpp
}

namespace pdfe::textedit {
    class TextEditTool;
}

namespace pdfe::reflow {
    class AlignmentPipeline;
}

namespace pdfe::theme {
    class ThemeManager;
}

#include "NavigationHistory.h"
// The member below is a std::vector<std::unique_ptr<PinnedTempFile>>, and
// unique_ptr's DELETER needs a complete type at the point the vector is
// destroyed — so the definition, not a forward declaration, is required here.
// (PinnedTempFile could not be held by value in a vector in any case: it has
// deleted copy operations and a user-declared destructor, so it is neither
// copyable nor movable.)
#include "../core/TempFileSafety.h"

#include "../textedit/TextEditModel.h"  // FormattingRun (needed by-value in std::vector)

namespace pdfe::fonts {
    class EmbeddedFontAnalyzer;
    class FontAnalysisCache;
    class SubstituteFontSelector;
    class PdfFontEmbedder;
}

#include "tre/AtlasMode.h"
#include "tre/TreTypes.h"         // HocrLine, RgbColor (by-value in PendingAtlasState)
#include "tre/VisualFontRanker.h" // RankedCandidate (by-value in PendingAtlasState)
#include "fonts/SubstituteFontSelector.h"  // SubstituteResult (by-value in PendingAtlasState)

namespace pdfe::tre {
    class OcrPipeline;
    class CoordMapper;
    class BgColorDetector;
    class FontMatcher;
    class QualityAssessor;
    class ImageMasker;
    class BitmapCompositor;
    class PdfContentWriter;
    class RegionReconstructor;
    class ImageReplacer;
    class EnrichedAtlas;
    class CNNFontClassifier;
}

namespace pdfe::redaction {
class RedactionManager;
}

namespace pdfe::encryption {
class EncryptionManager;
}

namespace pdfe::signatures {
class SignatureManager;
}

namespace pdfe::image {
class ImageManager;
class ExifStripper;
class ImageContentEditor;
class ImageTool;
class ImagePropertiesPanel;
}

namespace pdfe::metadata {
class MetadataManager;
}

namespace pdfe::app {
class AppSettings;
}

#include "ToolbarLayoutManager.h"

namespace pdfe::ui {

class PdfCanvas;
class PagePanel;
class PropertiesPanel;
class StatusBar;
class SearchPanel;
class BookmarkPanel;
class CommentPanel;
class LayerPanel;
class AnnotationPropertiesWidget;
class FormFieldPropertiesPanel;
class TextEditToolBar;
class RedactionPanel;
class SignaturePanel;
class MetadataDialog;
class RecentFilesMenu;
class AboutDialog;
class StickyNotePopup;
class DefaultsLogPanel;
class AttachmentPanel;

/// Build the application QIcon for the current OS color scheme.
/// Returns the dark SVG variant if Qt::ColorScheme::Dark is reported
/// by QGuiApplication::styleHints(), otherwise the light SVG. Falls
/// back to the multi-size .ico if the SVG resources are unavailable
/// at runtime (e.g., QtSvg plugin missing). Available to dialogs
/// and tests so the title-bar icon is consistent across the app.
QIcon pdfeAppIcon();

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    // Non-copyable, non-movable
    MainWindow(const MainWindow&) = delete;
    MainWindow& operator=(const MainWindow&) = delete;

    /// Open a PDF file programmatically (used by CLI and RecentFilesMenu).
    void openFile(const QString& filePath);

    // ── Session end (Windows logoff / shutdown / restart) ──────────────
    // F10 / WIN-68, WIN-69, WIN-70.
    //
    // Before this, PDFE handled ONLY QCloseEvent. A Windows logoff or
    // shutdown does not send one: the session manager broadcasts
    // WM_QUERYENDSESSION, Qt turns it into
    // QGuiApplication::commitDataRequest, and an application that ignores
    // that signal is simply terminated — the unsaved-changes prompt in
    // closeEvent() never runs and the user's edits are gone with no
    // warning at all.
    //
    // Qt's commitDataRequest is chosen over a raw nativeEvent
    // WM_QUERYENDSESSION handler deliberately: it is the cross-platform
    // route, Qt already owns the window-procedure plumbing, and — the
    // decisive reason — it is testable, because the decision logic can be
    // driven directly (see handleSessionEnd) without a live session
    // manager or a real logoff.

    /// What the application wants the session manager to do.
    enum class SessionEndDecision {
        Proceed,  ///< nothing unsaved, or the work has been made recoverable
        Cancel    ///< the user explicitly asked to abort the logoff/shutdown
    };

    /// Decide how to respond to a session-end (logoff / shutdown / restart)
    /// request, and — critically — make any unsaved work RECOVERABLE first.
    ///
    /// Contract, in this order:
    ///   1. If there are no unsaved changes, return Proceed. Nothing to do.
    ///   2. Otherwise ALWAYS take an emergency autosave snapshot FIRST, before
    ///      any prompt. The session manager may kill us at any moment and is
    ///      not obliged to wait for a dialog, so the snapshot is what actually
    ///      guarantees "not lost silently" — the next launch's recovery scan
    ///      finds the work. This step runs even when interaction is forbidden
    ///      (the unattended `shutdown /f` case), which is exactly when the old
    ///      closeEvent-only design lost everything.
    ///   3. If the session manager allows interaction, ask the user
    ///      (promptSaveOnClose): Save performs a real save, Cancel returns
    ///      Cancel so the shutdown is aborted, Discard proceeds.
    ///
    /// `allowsInteraction` mirrors QSessionManager::allowsInteraction().
    SessionEndDecision handleSessionEnd(bool allowsInteraction);

    /// True when an autosave recovery snapshot is currently published for
    /// this session (i.e. a crash or a forced logoff right now would be
    /// recoverable). Test accessor for the session-end oracle.
    bool hasRecoverySnapshotForTesting() const;

    /// Discard this window's recovery sidecar. Test teardown only: a
    /// session-end test deliberately PUBLISHES a snapshot, and without this the
    /// sidecar would survive into the next test and make the launch-time
    /// recovery prompt fire in an unrelated one.
    void clearRecoveryForTesting();

    /// Queue an alignment change for the active text span into
    /// AlignmentPipeline. No-op when no text edit session is active.
    /// Called from the TextEditToolBar::alignmentChanged signal; also
    /// exposed publicly so tests can exercise the full save→flush chain
    /// without having to prime a full text-edit session.
    void queueAlignmentForActiveSpan(Qt::Alignment qtAlign);

    /// Hit-test link annotations at `pdfPoint` on `pageIndex`. If a
    /// link is found, dispatch based on its /A action type:
    ///   Uri    → refused unless Security/externalActionsAllowed is ON
    ///             (F13 / SEC-49); otherwise QDesktopServices::openUrl
    ///   GoTo   → canvas_->goToPage
    ///   Named  → invoke matching MainWindow slot (NextPage, Print, ...)
    ///   Launch → confirmation dialog + QDesktopServices if accepted
    /// Returns true if a link was found and handled.
    bool followLinkAt(int pageIndex, QPointF pdfPoint);

    /// Pipeline E phase-13: test seam for /Launch link follow. When set,
    /// followLinkAt() routes the launch decision through this callback
    /// instead of showing a QMessageBox + QDesktopServices::openUrl.
    /// The callback receives the requested local path and returns
    /// `true` to simulate "user accepted (file would be launched)" or
    /// `false` to simulate "user declined". Both branches are observed
    /// by the test fixture's capture lists so we can assert that the
    /// prompt was offered AND that decline genuinely blocks the launch
    /// without real shell dispatch.
    ///
    /// Pass an empty std::function to clear the seam and restore the
    /// normal QMessageBox + openUrl path.
    using LaunchHandler =
        std::function<bool(const QString& localPath)>;
    static void setLaunchHandlerForTesting(LaunchHandler handler);

    /// Pipeline E phase-13: count of /Launch attempts since process
    /// start. Increments BOTH on accepted and declined launches —
    /// callers can compare deltas to verify the prompt was reached at
    /// all. This is independent of LaunchHandler.
    static int launchPromptCountForTesting();

    /// Pipeline E phase-13: count of /Launch invocations that actually
    /// proceeded past the prompt (user accepted, or test handler
    /// returned true). Stays at 0 when every prompt was declined.
    static int launchExecutedCountForTesting();

    /// Pipeline E phase-13: reset both counters. Tests call this in
    /// SetUp().
    static void resetLaunchCountersForTesting();

    // ------------------------------------------------------------------
    // Meta-audit F13 (SEC-49): the /URI external-action gate.
    //
    // `Security/externalActionsAllowed` (AppSettings, default OFF) used to be
    // DEAD CODE behind a LIVE CHECKBOX — declared, persisted, shown in
    // Preferences → Security, and consulted nowhere in src/. A security
    // control that silently gates nothing is worse than no control, because
    // the user believes they are protected. followLinkAt() now consults it
    // before dispatching a /URI action:
    //
    //   preference OFF (default) → the URI is NOT dispatched. The refusal is
    //       reported non-modally on the status bar (same idiom as the
    //       copy-permission refusal in onExportPlainText) and
    //       uriRefusedCountForTesting() increments.
    //   preference ON            → the URI is dispatched exactly as before.
    //
    // /Launch is deliberately NOT gated on the preference: it is already
    // governed by an unconditional per-click prompt, which is strictly
    // stronger than a persisted allow-flag, and the Preferences tooltip
    // already states "/Launch will still prompt regardless".
    // ------------------------------------------------------------------

    /// Test seam for the /URI dispatcher. When set, an ALLOWED URI is handed
    /// to this callback instead of QDesktopServices::openUrl, so tests can
    /// observe the dispatched URL without launching the OS browser. Pass an
    /// empty std::function to restore the normal shell path.
    using UriHandler = std::function<void(const QString& uri)>;
    static void setUriHandlerForTesting(UriHandler handler);

    /// Count of /URI actions dispatched (preference ON) since process start.
    static int uriDispatchedCountForTesting();

    /// Count of /URI actions REFUSED by the externalActionsAllowed gate
    /// (preference OFF) since process start. Does NOT count the separate
    /// UNC / file-share-scheme refusal, which is a different control.
    static int uriRefusedCountForTesting();

    /// Reset both /URI counters. Tests call this in SetUp().
    static void resetUriCountersForTesting();

    /// Test-visible accessor that queues an alignment directly for the
    /// given page + bounding rect. Real UI code queues via
    /// queueAlignmentForActiveSpan which derives these from the active
    /// text span; this overload lets tests construct deterministic
    /// BlockParams without a live text-edit session.
    void queueAlignmentForTest(int pageIndex, Qt::Alignment qtAlign,
                               const QRectF& pdfBbox,
                               const std::string& utf8Text,
                               float fontSizePt = 12.0f);

    /// Pipeline b3f19d72 phase-1 test-automation hook. Return the
    /// /Alt (alternate text) value of the annotation currently
    /// selected via SelectTool (mirrors @ref openAltTextDialogAction_
    /// selection state: ``selectedAnnotationPageIndex_`` and
    /// ``selectedAnnotationIndex_``). Returns an empty QString when:
    ///   - no document is loaded;
    ///   - no annotation is selected;
    ///   - the annotation has no /Alt entry.
    /// Lets pywinauto / WinAppDriver tests verify alt-text round-trip
    /// after AltTextDialog accept without driving the dialog read-back
    /// path manually.
    Q_INVOKABLE QString getCurrentAnnotationAltText();

    /// Pipeline b3f19d72 phase-8 test-only seam: returns true (and resets
    /// the internal flag to false) if onPrint() was entered while
    /// PDFE_TEST_DRYRUN_PRINT=1 was active.  Lets pywinauto tests confirm
    /// Ctrl+P reached the slot without driving native QPrinter machinery.
    Q_INVOKABLE bool consumeLastPrintDryRunFired();

    /// Pipeline b3f19d72 phase-11 test-only seam: returns the currently
    /// selected annotation index, or -1 when no annotation is selected.
    /// Lets pywinauto tests verify model-level selection (not just UIA
    /// widget visibility) before driving property-change operations.
    Q_INVOKABLE int currentSelectedAnnotationIndex() const;

    /// Pipeline b3f19d72 phase-11 test-only seam: activate an annotation/edit
    /// tool by its action objectName. Bypasses UIA toolbar discovery which is
    /// flaky in Qt6 for some tool buttons that get pushed into the overflow
    /// popup. Production code reaches the same state via QAction::trigger()
    /// from menus/toolbars. Returns true on success, false if the named
    /// action doesn't exist or is disabled.
    ///
    /// Python test harness drives this seam via the hidden QLineEdit
    /// "testToolActivatePipe" (objectName): writing the action name to the
    /// pipe via UIA set_edit_text() triggers textChanged → activateToolByObjectName.
    Q_INVOKABLE bool activateToolByObjectName(const QString& actionObjectName);

    /// GitHub #271 / #294. Publish an acknowledgement for a tool activation to
    /// testToolActivateResultPipe_, after the deferred trigger has run. No-op
    /// when the test pipes are disabled. See testToolActivateResultPipe_.
    // stateOverride empty -> the state is derived (armed when the canvas holds
    // a tool object, triggered otherwise). Pass a literal for an outcome that
    // cannot be derived, e.g. "disabled": the caller must be able to tell a
    // refusal from silence, and silence is all a missing ack conveys.
    void publishToolActivateAck(const QString& actionObjectName,
                                const QString& stateOverride = QString());

    // GitHub #271. Performs up to <count> undos ("<count>") or redos
    // ("redo:<count>") in one call and reports how many ACTUALLY happened via
    // testUndoRepeatResultPipe. One pipe call rather than N because a UIA round
    // trip costs ~270 ms at 1000 annotations. Test-only: the pipes that reach
    // it exist only when testPipesEnabled().
    void runTestUndoRedoRepeat(const QString& spec);

    // Writes "<seq>:<payload>" to testUndoRepeatResultPipe. The sequence number
    // is what lets the caller tell a fresh reply from the previous one: two
    // consecutive batches legitimately produce the same payload (undo 1000 ->
    // "1000:exhausted", redo 1000 -> "1000:exhausted"), so value comparison
    // alone cannot see the second one arrive.
    void publishUndoRepeatResult(const QString& payload);

    Q_INVOKABLE bool isPrintEnabled() const;

    Q_INVOKABLE bool deleteSelectedAnnotation();

    /// Pipeline 7-loop test-only seam: count comment-panel annotation rows.
    /// Lets pywinauto tests assert comment-panel sync (row count, per-type
    /// counts) WITHOUT enumerating the QTreeWidget via UIA descendants(),
    /// which Qt6's UIA bridge on Windows Server 2019 does not reliably
    /// surface (only viewport rows enumerate, and often none at all).
    ///
    /// A plain integer is returned (as a string) because the only carrier
    /// is a single-line QLineEdit result pipe — packing a row LIST into one
    /// UIA ValuePattern value is lossy (UIA collapses separators), so the
    /// seam answers a specific count query instead.
    ///
    /// query semantics:
    ///   - empty  → total annotation-row count.
    ///   - any other token → count of rows whose column-0 label CONTAINS
    ///     the token (e.g. "[Highlight]" counts Highlight rows).
    /// Page-header rows are always excluded.  Returns "0" when the panel
    /// is absent.  Product-side refresh wiring is covered by the GoogleTest
    /// suite (test_comment_panel*.cpp); this seam only substitutes the
    /// flaky UIA read path.  Driven via the hidden
    /// "testCommentInventoryTriggerPipe" → "testCommentInventoryResultPipe".
    Q_INVOKABLE QString commentPanelRowCount(const QString& query) const;

    /// Pipeline 7-loop test-only seam: report the live text-edit tool's
    /// selection state as "spanCount,activeSpanIndex,selStart,selEnd".
    /// Lets UI tests confirm a double-click actually placed a cursor on a
    /// real vector-text span (activeSpanIndex >= 0) before a typed edit,
    /// distinguishing a genuine no-op-edit product bug from a click that
    /// missed the text band.  Returns "0,-1,-1,-1" when no text-edit tool
    /// is active.  Driven via the hidden "testTextEditDiagTriggerPipe" →
    /// "testTextEditDiagResultPipe".
    Q_INVOKABLE QString textEditSelectionDiag() const;

    /// Pipeline e7d3c2a1 phase-2 render-oracle seam: report which editing
    /// tool is currently active on the canvas — "treToolAction" (the
    /// scanned-region / TRE drag tool), "textEditToolAction" (the per-span
    /// vector text tool), the checked annotation tool's objectName, or
    /// "none".  Lets render-verification tests assert the per-region tool
    /// routing decision (defect #4) without enumerating the UIA toolbar.
    /// Driven via "testActiveToolQueryPipe" → "testActiveToolResultPipe".
    Q_INVOKABLE QString activeToolDiag() const;

    /// Pipeline e7d3c2a1 phase-3 test-only seam: run OCR on the current page
    /// via ocrPipeline_->ocrPage() and return the recognized text as a plain
    /// UTF-8 string (hOCR tags stripped, whitespace collapsed).  Lets the
    /// multilingual render-verification test assert that the OCR output on an
    /// Arabic block actually contains Arabic-script codepoints rather than
    /// garbled Latin — the render-level signature of defect #3 (doc-wide
    /// OcrConfig::language baked at open instead of per-region language).
    /// Returns an empty string if no document/pipeline is loaded.  Driven via
    /// "testOcrTextQueryPipe" → "testOcrTextResultPipe".  Non-const: it lazily
    /// constructs the TRE/OCR pipeline (initTreComponents) on first use, the
    /// same lazy init the text-edit path performs.
    Q_INVOKABLE QString currentPageOcrText();

    /// GitHub #18 (PDF-48) — pipeline p83-outstanding phase-2.
    ///
    /// Run the SHIPPED form-submit decision for `urlText` against the document
    /// that is actually open, and report what the product decided as a token
    /// (see submitCurrentFormTo for the vocabulary). This calls the SAME
    /// production function the Forms ▸ Submit Form… dialog calls; it is not a
    /// parallel implementation, which is the whole reason it can stand as
    /// evidence that the product refuses an incomplete required-field form.
    ///
    /// `hostConfirmed` is deliberately NOT exposed: the seam can only ever run
    /// the unconfirmed pass, which stops at "confirm-host:…" and can therefore
    /// never cause an outbound connection from a test.
    ///
    /// GATED on pdfe::ui::testseam::testPipesEnabled(). Returns
    /// "seam-disabled" otherwise, and in a release-configured build the gate
    /// is a compile-time constant false, so the body is statically dead. An
    /// ungated Q_INVOKABLE here would hand any local process that can talk to
    /// the meta-object a way to drive network egress — exactly the hole
    /// phase-12 of the previous pipeline closed.
    Q_INVOKABLE QString submitCurrentFormDiag(const QString& urlText);

    /// GitHub #179 — pipeline p83-outstanding phase-2.
    ///
    /// Surface the open document's encryption dictionary facts, including
    /// `/EncryptMetadata`, as "encrypted=<0|1>;metadataEncrypted=<0|1>;
    /// method=<name>". Reads through the live EncryptionManager — the same
    /// EncryptionInfo the Document Permissions dialog displays — so a test can
    /// assert the surfaced value without driving a modal whose text no test
    /// can see. Returns "no-document" when nothing is open.
    ///
    /// Gated on testPipesEnabled() for the same reason as above; returns
    /// "seam-disabled" when the seam is off.
    Q_INVOKABLE QString documentEncryptionDiag() const;


    /// Pipeline b3f19d72 phase-1 test-only seam: set the annotation
    /// selection state directly. Production code reaches the same
    /// state through SelectTool::annotationSelected. Tests use this
    /// to exercise getCurrentAnnotationAltText's positive path
    /// without driving a full SelectTool click pipeline (which
    /// depends on QWidget visibility + a real cursor under offscreen
    /// Qt, both flaky in CI). Pass (-1, -1) to clear.
    void setSelectedAnnotationForTesting(int pageIndex, int annotIndex);

    /// GitHub #253 / p97 phase-9 read-back seams for the CHOICE-ROUTING test.
    ///
    /// Why these exist. `onFormFieldValueChanged` decides whether a choice edit
    /// is committed through `SetChoiceSelectionCommand` (which models a
    /// `vector<int>`) or `SetFieldValueCommand` (which takes one value string
    /// and therefore keeps only the FIRST selection of a multi-select list
    /// box). That routing decision was the actual fix for a live data-loss bug,
    /// and the `claim-check` gate failed phase-9 because NOTHING exercised it:
    /// `test_form_commands.cpp` constructs `SetChoiceSelectionCommand` directly
    /// against a `FormManager`, with no `MainWindow` in the fixture at all, so
    /// reverting the routing to always-`SetFieldValueCommand` -- i.e. restoring
    /// the bug -- left the whole suite green.
    ///
    /// Asserting the routing requires reading back what the LIVE `FormManager`
    /// actually holds after the signal has been dispatched, and `formManager_`
    /// is private. These two accessors are the narrowest thing that makes the
    /// real dispatch observable; deliberately NOT a general `formManager()`
    /// getter, which would let a test reach around the routing it is meant to
    /// be testing.

    /// Widget index of the first ListBox on the current page carrying at least
    /// `minOptions` options, or -1 if there is none. Lets the test locate a
    /// field by PROPERTY rather than hardcoding an index that a fixture change
    /// would silently invalidate.
    int firstListBoxForTesting(int minOptions) const;

    /// The selected indices the live FormManager holds for `widgetIndex` on the
    /// current page. Empty when there is no document, manager, page, or field.
    /// This is the oracle: after a two-index edit it must contain BOTH indices,
    /// which `SetFieldValueCommand` structurally cannot achieve.
    std::vector<int> committedSelectionForTesting(int widgetIndex) const;

    /// Pipeline b3f19d72 phase-1 test-only seam: set /Alt text on
    /// the given annotation via the live AnnotationManager. Lets
    /// tests prepare a known alt-text value without re-implementing
    /// the MuPDF dict-write logic. Returns true on success, false
    /// when there's no document / annotationManager / page or the
    /// annotation index is out of range.
    bool setAnnotationAltTextForTesting(int pageIndex, int annotIndex,
                                        const QString& altText);

    /// c82a1f37 hardening: return the zoom combo directly for tests. The
    /// ZoomComboExists integration test previously resolved it by walking the
    /// main toolbar's QWidgetActions and via findChild<QComboBox> — BOTH
    /// intermittently miss the combo in a never-shown headless QMainWindow,
    /// where the QWidgetAction's default widget stays UNREALIZED and is not
    /// parented into the live child tree (documented racy; CI run 27082833605).
    /// The member pointer is populated unconditionally in createToolBar() (the
    /// addWidget() to the main toolbar is the adjacent line), so returning it is
    /// a race-free assertion that the zoom combo exists and is wired in.
    QComboBox* zoomComboForTesting() const { return zoomCombo_; }

    /// bugfix-3e91a7c4 #4/#5 test seam: the match count reported by the most
    /// recent onReplaceAll() (the "Replaced N match(es)" value). -1 until first use.
    int lastReplaceAllCountForTesting() const { return lastReplaceAllCountForTesting_; }

    /// bugfix-3e91a7c4 #4/#5 test seam: the live in-memory document. onReplaceAll
    /// applies edits IN MEMORY (unsaved), so a test must read the result back
    /// through this accessor, not by reopening the file on disk.
    pdfe::core::Document* documentForTesting() const { return document_.get(); }

    /// GitHub #240 — the alignment pipeline, so a test can select which Save As
    /// branch runs.
    ///
    /// onSaveAsFile() forks on `alignmentPipeline_->pendingPageCount() > 0`: one
    /// branch flushes and reopens, the other preserves undo by rebinding command
    /// pages. The network policy is checked ONCE before that fork, and the
    /// acceptance criterion for #240 asks for both branches to be exercised
    /// against a network target — which is impossible from a widget test without
    /// being able to put a page in the queue. Read-only accessor; the queueing
    /// method it exposes is already public on AlignmentPipeline.
    pdfe::reflow::AlignmentPipeline* alignmentPipelineForTesting() const {
        return alignmentPipeline_.get();
    }

    /// Drive the WIN-87 site #4 rewrite (the QPDF single-revision pass that
    /// publishes the redacted document) without the modal confirmation that
    /// guards it in onApplyRedactions(). Returns false and fills @p errorOut
    /// on failure — the production caller turns the same exception into a
    /// reopen of the untouched original.
    bool rewriteRedactedDocumentThroughQpdfForTesting(
        const std::filesystem::path& filePath, QString* errorOut = nullptr);

    /// WIN-87 site #4 test seam. Fired INSIDE the publish window — after
    /// closeDocument() has released the document's locks and before the source
    /// bytes are read — so a hostile test can act at exactly the instant that
    /// distinguishes the hardened path from the pre-phase-57 one.
    ///
    /// WHY THIS EXISTS, because a bare "test hook" in production code needs a
    /// justification stronger than convenience. The window here is microseconds
    /// wide, and unlike sites #3 and #5 it cannot be closed structurally: the
    /// artifact under attack is the USER'S OWN DOCUMENT, whose name is the thing
    /// being protected and therefore cannot be made unpredictable. A
    /// free-running attacker thread hits that window only by luck, and MEASURED
    /// it did not: `phase57_revert_and_observe.py` found the site-#4 hostile
    /// case passing against the REVERTED, vulnerable build — a green test over
    /// a live defect. This seam replaces that coin toss with a deterministic
    /// one, so the case is red whenever the fix is absent.
    ///
    /// Never set in production; a null hook costs one predictable branch.
    static void setRedactionRewriteWindowHookForTesting(std::function<void()> hook);

    /// WIN-87 site #5 save-window test seam. Fires in onSaveFile/onSaveAsFile
    /// at the exact instant AFTER closeDocument() and BEFORE the alignment
    /// flush reads the just-saved document.
    ///
    /// WHY THIS ONE EXISTS SEPARATELY FROM THE SITE-#4 SEAM. The site-#5
    /// hostile case attacks the flush's STAGING artifact, whose name phase-57
    /// made unpredictable — a structural fix a racing thread can witness. The
    /// window this seam marks is a different one, on the INPUT side: the
    /// caller deliberately releases every handle on the user's just-saved
    /// document, and the document's name is the one thing that cannot be made
    /// unpredictable. The pin is therefore taken by the CALLER before
    /// closeDocument(); this seam fires inside the gap that pin covers, so a
    /// substitution attempted here fails against the hardened build and lands
    /// against a build where the pin is taken inside the flush instead.
    ///
    /// Never set in production; a null hook costs one predictable branch.
    static void setSaveCloseWindowHookForTesting(std::function<void()> hook);

    /// WIN-87 site #4 OUTPUT-STAGING test seam. Fires inside the QPDF
    /// redaction rewrite at the instant the staged output is fully written and
    /// has NOT yet been published over the user's document, with its path as
    /// the argument.
    ///
    /// WHY IT EXISTS: acceptance criterion 3 asks for a hostile-filesystem test
    /// on ".redact_out / the QPDF rewrite's equivalent intermediate" — the
    /// OUTPUT. The phase's first pass gave site #4 only a SOURCE-side seam and
    /// tested the input window instead, which is a real defect this phase found
    /// but is NOT the artifact the criterion names. Caught by the phase-4
    /// validator gate. Site #5 had both seams from the start; site #4 now does
    /// too, and the asymmetry was the whole tell.
    ///
    /// Never set in production; a null hook costs one predictable branch.
    static void setRedactionOutputStagingWindowHookForTesting(
        std::function<void(const std::filesystem::path&)> hook);

    /// WIN-87 site #5 STAGING-publish test seam. Fires inside the alignment
    /// flush at the instant the staging artifact has been fully written and has
    /// NOT yet been published over the user's document, with its path as the
    /// argument.
    ///
    /// WHY A SEAM WAS NEEDED HERE AT ALL, given the free-running attacker the
    /// staging case already uses: MEASURED, that case does not TERMINATE
    /// against the reverted (pre-phase-57) code — 600 s and killed, where the
    /// fixed build passes in about a second. A hang is an absence of
    /// measurement, not a red, so acceptance criterion 8 could not be
    /// satisfied by it. The attacker case is kept, because its subject IS the
    /// counters (a name that could be observed, opened for write, renamed
    /// over); this seam backs it with a second case that terminates and can
    /// therefore discriminate.
    ///
    /// Never set in production; a null hook costs one predictable branch.
    static void setAlignStagingWindowHookForTesting(
        std::function<void(const std::filesystem::path&)> hook);

    /// bugfix-3e91a7c4 #4/#5 test seam: drive the private onReplaceAll slot from a
    /// test without a live SearchPanel signal round-trip.
    void replaceAllForTesting(const QString& find, const QString& replace) {
        onReplaceAll(find, replace);
    }

    /// bugfix-3e91a7c4 #1/#2 test seams: the annotation and form toolbars, so
    /// UI-structure tests can assert action icons (#2) and field-group
    /// separators (#1) without walking the whole widget tree.
    QToolBar* annotationToolBarForTesting() const { return annotationToolBar_; }
    QToolBar* formToolBarForTesting() const { return formToolBar_; }

    /// bugfix-3e91a7c4 #3 test seams: the three tabified left-dock panels plus a
    /// direct entry to the default-layout enforcement, so a test can simulate a
    /// stale restoreState() (Layers raised / Pages hidden) and verify Pages is
    /// forced back as the default left-dock tab.
    QDockWidget* pagePanelDockForTesting() const { return pagePanelDock_; }
    QDockWidget* layerPanelDockForTesting() const { return layerPanelDock_; }
    QDockWidget* bookmarkPanelDockForTesting() const { return bookmarkPanelDock_; }
    void ensurePagesDockDefaultForTesting() { ensurePagesDockDefault(); }

    // ---- R3-12 clipboard image (in/out) ----------------------------------
    /// Set the copy-selection region in PDF user-space (bottom-left origin,
    /// points) for `pageIndex`. When a selection is set, onCopy() copies a
    /// rendered image of just this region to the clipboard as CF_DIB/CF_BITMAP
    /// (via QClipboard::setImage, which Qt exposes to the Windows clipboard in
    /// those formats) instead of copying the whole-page text. Pass an empty
    /// rect (or a negative page index) to clear the selection.
    void setCopySelectionRect(int pageIndex, const QRectF& pdfRect);

    /// True when a non-empty copy-selection region is currently set.
    bool hasCopySelection() const { return copySelectionValid_; }

    /// R3-12: render the current copy-selection region to a QImage and place it
    /// on the clipboard as an image. Honours the document copy/extract
    /// permission (documentDeniesCopy()). Returns true when an image was
    /// copied; false when there is no selection, no document, or copy is denied.
    /// The produced image is sized to the SELECTION (scale-multiplied), never
    /// the whole page.
    bool copySelectionAsImage();

    /// R3-12: paste an image from the clipboard into the current page via the
    /// existing undoable InsertImageCommand pipeline. Returns true when an
    /// image was inserted; false when the clipboard holds no image. The
    /// clipboard image is materialised to a temporary PNG that the insert path
    /// reads; the temp file is intentionally NOT deleted because
    /// InsertImageCommand::redo() re-reads the source file.
    bool pasteImageFromClipboard();

    /// R3-12 test seam: number of images currently on `pageIndex` (via the live
    /// ImageManager). Returns -1 when no document/manager is available.
    int pageImageCountForTesting(int pageIndex);

    /// R3-12 test seams: drive the copy / paste Edit-menu slots directly.
    void copyForTesting() { onCopy(); }
    void pasteForTesting() { onPaste(); }

    /// R3-12 end-to-end test seam: exercise the PRODUCTION selection wiring.
    /// Activates the Select tool (which wires SelectTool::regionSelected →
    /// setCopySelectionRect, exactly as onAnnotationToolSelected does), then
    /// drives the live SelectTool to select `annotIndex` on `pageIndex` via the
    /// canvas → SelectTool::selectByIndex → regionSelected → setCopySelectionRect.
    /// After this, onCopy() copies the selected annotation's region as an image.
    /// Returns true when the selection was dispatched. This does NOT set the
    /// copy-selection directly — it proves the SelectTool→MainWindow→onCopy path.
    bool selectAnnotationRegionForTesting(int pageIndex, int annotIndex);

    /// GitHub #241 — hand multi-file drop routing to whoever owns the windows.
    ///
    /// Off by default, which is the long-standing behaviour: a dropped
    /// multi-selection opens the first .pdf in THIS window and the rest are not
    /// opened. `WindowManager` turns it on for the windows it creates, and then
    /// every dropped file is offered through openPathRequested() so each gets a
    /// window of its own — with the same-file rule still applied, so a drop
    /// containing a duplicate cannot produce two editors on one document.
    void setDropRoutingDelegated(bool delegated) { dropRoutingDelegated_ = delegated; }
    bool dropRoutingDelegated() const { return dropRoutingDelegated_; }

signals:
    /// GitHub #241 — this window was asked to open a file it should not open
    /// itself, because the file belongs in a window of its own.
    ///
    /// Emitted by dropEvent() for the second and subsequent PDFs in a
    /// multi-file drop. The first still opens in THIS window, which keeps the
    /// long-standing drop behaviour (and every test written against it) intact;
    /// the rest are offered to whoever is managing windows.
    ///
    /// Nothing in the widget layer connects this. When no WindowManager is
    /// listening — every unit test, and any embedding that wants the old
    /// single-window behaviour — the extra paths go nowhere, which is exactly
    /// what happened before this signal existed.
    void openPathRequested(const QString& filePath);

protected:
    void closeEvent(QCloseEvent* event) override;

    /// Drag-drop file open (UI-DD-01). Accept .pdf drops on the main
    /// window and route them through openFile().
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;

    /// Action chosen by the user when prompted to save unsaved
    /// changes on close. Tests override `promptSaveOnClose()` to
    /// return a fixed value and exercise the closeEvent branching
    /// without spawning a real QMessageBox (UI-KB-07).
    enum class CloseAction { Save, Discard, Cancel };

    /// True when the current document has unsaved changes the
    /// closeEvent must prompt for. Virtual so test subclasses can
    /// inject a deterministic state without loading a real document.
    virtual bool hasUnsavedChanges() const;

    /// Show a save / discard / cancel prompt for unsaved changes.
    /// Production implementation displays a QMessageBox. Override in
    /// test subclasses to inject a deterministic answer.
    virtual CloseAction promptSaveOnClose();

    /// GitHub #240 — the outcome of asking whether a network open may proceed.
    enum class NetworkOpenDecision {
        Allowed,      ///< The share answered and the setting permits it.
        Unreachable,  ///< The location did not answer within the budget.
        Refused,      ///< The setting forbids user network opens.
    };

    /// Decide whether a USER-initiated open of @p fileName on a network location
    /// may proceed, probing reachability OFF the GUI thread within a bounded
    /// budget. Virtual so a test can decide without a real share.
    virtual NetworkOpenDecision decideNetworkOpen(const QString& fileName);

    /// Copy @p networkPath to a local temporary file and return its path, or an
    /// empty string on failure. The copy lives as long as this window.
    ///
    /// Exists because the preflight child is low-integrity and cannot
    /// authenticate to SMB — see the call site. Virtual so a test can substitute
    /// a staging step without a share.
    virtual QString stageLocalPreflightCopy(const QString& networkPath);

    /// GitHub #242 — what the user chose in the Insert Image dialog.
    struct InsertImageChoice {
        QRectF targetRect;
        int jpegQuality = 85;
        bool stripExif = true;
        bool maintainAspectRatio = true;
    };

    /// Ask for placement and encoding options; `std::nullopt` means cancelled.
    ///
    /// Production shows InsertImageDialog. Virtual so a widget test can answer
    /// it without a real modal — `exec()` blocks the event loop, so a headless
    /// test that triggered insertImageAction would hang rather than fail, which
    /// is exactly why this path had no coverage while `targetRect` was a
    /// hardcoded literal.
    virtual std::optional<InsertImageChoice> promptInsertImageOptions(
        const QString& imagePath, QSize sourceSize, QSizeF pageSize);

    /// Confirm the irreversible form-flatten. Production shows a QMessageBox
    /// warning; override in a test subclass to answer it deterministically.
    ///
    /// GitHub #243 — this exists because the modal made the UI flatten path
    /// untestable: QMessageBox::exec() blocks the event loop, so a headless
    /// widget test that triggered flattenFormAction simply hung. That is why
    /// the defect in onFlattenForm() (destroying the FormTool while leaving
    /// formToolAction_ checked) was never caught by a unit test.
    virtual bool confirmFlattenForm();

public:
    /// Populate `menu` with the text-edit-mode context-menu actions
    /// (UI-CTX-02 Cut/Copy/Paste/Select All + UI-CTX-11
    /// Bold/Italic/Find + Atlas Mode submenu when ready). Tool may
    /// be null when the menu is built without a live text-edit tool;
    /// in that case all actions are disabled. Tests inspect the
    /// populated menu without calling QMenu::exec.
    void buildTextEditContextMenu(QMenu& menu,
                                   pdfe::textedit::TextEditTool* tool);

private slots:
    // Pipeline E phase-10
    void onCustomizeToolbars();

    // File menu
    void onNewFile();
    void onOpenFile();
    void onSaveFile();
    void onSaveAsFile();
    void onExportPlainText();
    void onCreatePdfFromImages();
    void onCombineFiles(); // Pipeline E phase-11
    void onPreviousView();
    void onNextView();
    void onCloseFile();
    void onExit();

    // Edit menu
    void onUndo();
    void onRedo();
    void onCopy();
    void onCut();
    void onPaste();
    void onSelectAll();
    void onFind();
    void onReplace(const QString& find, const QString& replace);
    void onReplaceAll(const QString& find, const QString& replace);
    void onPrint();
    void onPrintPreview();

    // View menu
    void onZoomIn();
    void onZoomOut();
    void onFitWidth();
    void onFitPage();
    void onActualSize();
    void onToggleFullScreen();
    void onTogglePagePanel();
    void onTogglePropertiesPanel();
    void onToggleSearchPanel();
    void onToggleBookmarkPanel();

    // Help menu
    void onAbout();
    void onDocumentation();
    void onReportIssue();
    void onCheckUpdates();

    /// GitHub #127. Opt-in, notify-only startup version check. Returns
    /// immediately doing nothing unless AppSettings::updateCheckEnabled() is
    /// true, which it is not by default. Never downloads or executes anything
    /// -- see UC-9 in docs/code-signing.md.
    void maybeCheckForUpdateOnStartup();

    // Canvas interactions
    void onPageChanged(int pageIndex);
    void onZoomChanged(float zoom);
    void onPageSelected(int pageIndex);

    // Search and bookmark interactions
    void onSearchResultSelected(int pageIndex, QVector<QRectF> quads);
    void onBookmarkActivated(int pageIndex, float x, float y, float zoom);

    // Zoom combo
    void onZoomComboChanged(int index);
    void onZoomComboEdited(const QString& text);

    // Annotation toolbar
    void onAnnotationToolSelected(QAction* action);
    void onAnnotationColorPicker();
    void onCommentPanelNavigate(int pageIndex, int annotIndex);
    void onCommentPanelDelete(int pageIndex, int annotIndex);
    void onAnnotationPropertiesChanged(pdfe::annotations::AnnotationData data);
    void onAnnotationCreatedByTool(int pageIndex, pdfe::annotations::AnnotationData data);

    /// c82a1f37 P5: create a form field via the command system (undo + refresh
    /// + title). Extracted from the FormTool::fieldCreated callback so both a
    /// real drag and the testFieldCreatePipe seam share one deterministic path.
    void createFormField(int pageIndex, pdfe::forms::FormFieldType type,
                         QRectF rect);

    /// c82a1f37 test-hardening: deterministically create an annotation of the
    /// given type at the given rect via the command system (undo + comment-panel
    /// refresh + canvas refresh + title), bypassing the flaky synthetic-click
    /// create. Driven by the testAnnotCreatePipe seam.
    void createAnnotationForTest(int pageIndex,
                                 pdfe::annotations::AnnotationType type,
                                 QRectF rect);

    // Pipeline E phase-4: image stamp library
    void onImportImageStamp();

    // Text editing
    void onTextEditToolSelected();

    // Scanned text editing (TRE)
    void onTreRegionSelected(int pageIndex, QRectF pdfRect);

    // Redaction
    void onRedactToolSelected();
    void onMarkRedaction();
    void onApplyRedactions();
    void onDeleteRedactionMark(int pageIndex, int annotIndex);
    void onRedactionMarkSelected(int pageIndex, int annotIndex);

    // Encryption
    void onSaveWithEncryption();
    void onDocumentPermissions();
    void onRemovePassword();

    // Signatures
    void onSignDocument();
    void onVerifySignatures();
    /// Pipeline F phase-9: user-confirmed network fetch for OCSP+CRL
    /// followed by /DSS embedding. NEVER auto-runs; only triggered by
    /// the requestValidationAction.
    void onRequestValidation();

    // Image manipulation
    void onImageToolSelected();
    void onInsertImage();
    void onStripExif();
    void onImageRemove();
    void onImageRotate90();
    void onImageRotate180();
    void onImageRotate270();
    void onImageCrop();
    void onImageResize();

    // Metadata
    void onDocumentMetadata();
    void onCompliance();

    // Forms
    void onFormToolSelected();
    void onCreateFormField();
    void onFormFieldProperties();
    void onExportFormData();
    void onImportFormData();
    void onResetForm();
    void onFlattenForm();
    void onFormFieldSelected(int widgetIndex);
    void onFormFieldDeselected();
    void onFormFieldValueChanged(pdfe::forms::FormFieldData data);

    // Page manipulation
    void onRotatePageCW();
    void onRotatePageCCW();
    void onRotatePage180();
    void onDeletePages();
    void onMovePageUp();
    void onMovePageDown();
    void onInsertBlankPage();
    void onInsertPagesFromFile();
    void onExtractPages();
    void onPageMoved(int fromIndex, int toIndex);
    // preserveCommandPages: when true, refresh caches/panels after a structural
    // change WITHOUT clearing commandPages_. The ordinary File>Save path uses
    // this so the pages that saveWithCommandPageRebind() re-resolved (and the
    // undo commands rebound to) survive the post-save refresh. Default false
    // keeps every other caller's drop-stale-pointers behavior.
    void onDocumentStructureChanged(bool preserveCommandPages = false);

    // Toolbar toggles
    void onToggleStickyNotes(bool checked);
    void onToggleCopyPrevention(bool checked);
    void onTogglePrintPrevention(bool checked);
    void onToggleOcrBoxes(bool checked);
    void loadOcrBoxesForPage(int pageIndex);

    // Sticky note popup
    void onStickyNoteDoubleClicked(int pageIndex, int annotIndex, pdfe::annotations::AnnotationData data);
    void onStickyNotePopupContentsChanged(pdfe::annotations::AnnotationData data);
    void onStickyNotePopupClosed(int pageIndex, int annotIndex);

private:
    /// GitHub #18 (PDF-48) — THE shipped form-submit entry point.
    ///
    /// Collects the open document's fields through the live FormManager,
    /// converts them with pdfe::formsubmit::toSubmitFields (which is what
    /// carries the parsed Required flag across the boundary), asks
    /// FormSubmitter::shouldSubmit, and only on Allow calls
    /// FormSubmitter::submitForm. Returns a decision token:
    ///
    ///   "reject-external-actions-disabled"  Security/externalActionsAllowed
    ///                                       is off (the default)
    ///   "reject-no-document"                nothing open
    ///   "reject-invalid-url"                empty/malformed/unsupported scheme
    ///   "reject-blocked-destination"        loopback / private / link-local
    ///   "reject-missing-required:<a,b,c>"   a Required field carries no value
    ///   "confirm-host:<host>"               host not yet confirmed this session
    ///   "submitted:<httpStatus>"            POSTed
    ///   "submit-failed:<message>"           POST attempted and failed
    ///
    /// `hostConfirmed` is passed true only by the action handler AFTER the
    /// user has answered the host prompt. The prompt is raised by the caller,
    /// not here, so this function performs no modal I/O on the path a test
    /// drives and cannot open a network connection on the unconfirmed pass.
    ///
    /// The destination rules deliberately outrank completeness — see
    /// FormSubmitter::shouldSubmit; a submit aimed at 169.254.169.254 is
    /// reported as an SSRF refusal whether or not the form is complete.
    QString submitCurrentFormTo(const QUrl& url, bool hostConfirmed);

    /// Hosts the user has confirmed for /SubmitForm during THIS session.
    /// Deliberately not persisted: an allowlist that survives a restart is a
    /// standing egress permission granted by one click on one document, and
    /// nothing in AppSettings stores one today. Stored as plain strings so
    /// MainWindow.h stays free of formsubmit types.
    QSet<QString> confirmedSubmitHosts_;

    // Pipeline E phase-13: process-wide /Launch test seam state.
    // Pipeline F phase-1: counters atomicized + handler mutex-guarded
    // so concurrent test runs (TSan stress, future parallel GTest)
    // don't race. Defined in MainWindow.cpp.
    static LaunchHandler sLaunchHandler_;
    static std::mutex sLaunchHandlerMutex_;
    static std::atomic<int> sLaunchPromptCount_;
    static std::atomic<int> sLaunchExecutedCount_;

    // Meta-audit F13 (SEC-49): process-wide /URI gate test seam state.
    static UriHandler sUriHandler_;
    static std::mutex sUriHandlerMutex_;

    // WIN-87 site #4 publish-window test seam. See the public setter for why
    // a production-code hook is justified here.
    static std::function<void()> sRedactionRewriteWindowHook_;
    // Guarded like sUriHandler_ one line above. Latent rather than live
    // today (callers are single-threaded), but the sibling precedent is a
    // mutex and a future test arming this from a worker would be a data
    // race in the middle of a security-critical window.
    static std::mutex sRedactionRewriteWindowHookMutex_;

    // WIN-87 site #5 save-window test seam. Guarded for the same reason as the
    // sibling above. Fired via the helper so both call sites are identical and
    // neither can forget the lock.
    static std::function<void()> sSaveCloseWindowHook_;
    static std::mutex sSaveCloseWindowHookMutex_;
    static void fireSaveCloseWindowHookForTesting();

    // WIN-87 site #5 staging-publish test seam. Same guarding rationale.
    static std::function<void(const std::filesystem::path&)> sAlignStagingWindowHook_;
    static std::mutex sAlignStagingWindowHookMutex_;
    // WIN-87 site #4 output-staging test seam. Same guarding rationale.
    static std::function<void(const std::filesystem::path&)> sRedactionOutputStagingHook_;
    static std::mutex sRedactionOutputStagingHookMutex_;
    static void fireRedactionOutputStagingHookForTesting(
        const std::filesystem::path& stagingPath);

    static void fireAlignStagingWindowHookForTesting(
        const std::filesystem::path& stagingPath);

    static std::atomic<int> sUriDispatchedCount_;
    static std::atomic<int> sUriRefusedCount_;

    /// True when Security/externalActionsAllowed is set. Reads the live
    /// AppSettings on every call so a Preferences change takes effect without
    /// a restart (and so tests can flip it between clicks).
    bool externalActionsAllowed() const;

    void createActions();
    void createMenus();
    void createToolBar();
    void createAnnotationToolBar();
    void createStatusBar();

    // Pipeline E phase-10: customizable toolbars.
    std::vector<QAction*> buildToolbarActionRegistry() const;
    void applyToolbarLayoutsList(const std::vector<ToolbarLayout>& layouts);
    void loadSavedToolbarLayout();
    QAction* customizeToolbarsAction_ = nullptr;
    void createDockPanels();
    /// bugfix-3e91a7c4 #3: force Pages to be the visible/raised left-dock tab,
    /// overriding any stale saved windowState (Layers raised / Pages hidden).
    /// Called after restoreState() in the constructor.
    void ensurePagesDockDefault();
    void setupAccessibleNames();
    void setupPanelShortcuts();
    void updateWindowTitle();
    void updateZoomCombo(float zoom);
    void closeDocument();

    /// GitHub #60 (SEC-62). Build the active-content advisory banner. Called
    /// once from the constructor; the widget is created hidden.
    void createProhibitedFeatureBanner();
    /// Run ProhibitedFeatureScanner over the freshly opened document_ and
    /// show/hide the banner accordingly. MUST be able to HIDE as well as show:
    /// a banner that survives into the next document is indistinguishable from
    /// one that fires for every document, and both train the user to ignore it.
    void updateProhibitedFeatureBanner();

    void initTreComponents();
    void destroyTreComponents();
    void refreshImageForScannedPage(int pageIndex, const std::u32string& text,
                                     const QRectF& spanBbox,
                                     const std::vector<pdfe::textedit::FormattingRun>& fmtRuns = {},
                                     const std::string& spanFontName = {},
                                     float spanFontSize = 0.0f);
    void activateTextEditForPage(int pageIndex);
    QString resolveTessdataPath() const;
    QString resolveModelPath() const;
    void repositionStickyNotePopup();
    void applyAtlasMode(pdfe::tre::AtlasMode mode);
    void showAtlasComparison();

    /// If the alignment pipeline has pending work, run it through the
    /// saved file. Call after document_->save() / saveAs() has written
    /// `savedPath`; the pipeline rewrites the file in place (temp +
    /// atomic replace). Returns true when applied (or nothing to do).
    /// @param preTakenSource A deny-write read handle on @p savedPath that the
    ///        CALLER took BEFORE it released the document. Ownership passes
    ///        here; this closes it. May be null only when no flush is pending.
    ///
    /// WHY THE CALLER TAKES THE PIN AND NOT THIS FUNCTION. Both callers
    /// deliberately `closeDocument()` first so the in-place publish can land,
    /// which releases every handle on the user's just-saved document. A pin
    /// taken INSIDE this function is therefore taken AFTER that release, and the
    /// document sits unheld under its own predictable name in between — the
    /// exact substitution window this site was named the worst of the seven
    /// for. Found by the phase-4 karen gate, which also caught the comment that
    /// claimed the name was "resolved exactly twice, both times up front"; that
    /// was true of this function and false of the sequence it belongs to.
    bool flushAlignmentPipelineIfPending(const std::filesystem::path& savedPath,
                                         std::FILE* preTakenSource);

    /// WIN-87 site #4 (phase-57). The QPDF single-revision rewrite that
    /// publishes the REDACTED document over @p filePath. Closes the document
    /// itself (the rename cannot land while this process holds the file) and
    /// throws on any failure, leaving @p filePath byte-intact. Extracted from
    /// onApplyRedactions() so a hostile-filesystem test can drive it: the
    /// caller sits behind a modal confirmation, and a window that only opens
    /// inside a QMessageBox cannot be attacked by a test.
    void rewriteRedactedDocumentThroughQpdf(
        const std::filesystem::path& filePath);

    // MuPDF context (shared across app lifetime)
    std::shared_ptr<pdfe::core::MuPdfContext> muPdfContext_;

    // Document and rendering
    std::unique_ptr<pdfe::core::Document> document_;
    /// bugfix-3e91a7c4 #4/#5: match count from the most recent onReplaceAll()
    /// (exposed for tests via lastReplaceAllCountForTesting()).
    int lastReplaceAllCountForTesting_ = -1;
    std::unique_ptr<pdfe::rendering::TileCache> tileCache_;
    std::unique_ptr<pdfe::rendering::TileRenderer> tileRenderer_;

    // Command system
    pdfe::commands::UndoManager* undoManager_ = nullptr;

    // R3-04 autosave / crash-recovery. Owns a headless AutosaveManager for the
    // currently-open document; started on open, stopped/cleared on clean save
    // or close. checkForRecoverableSessions() runs once at startup to offer
    // recovery of any session that did not exit cleanly.
    std::unique_ptr<pdfe::core::AutosaveManager> autosaveManager_;
    void checkForRecoverableSessions();

    /// Append @p what to the file named by PDFE_RECOVERY_TRACE, if set.
    /// Inert otherwise. GitHub #285 cluster G: the app's stderr stops reaching
    /// a parent harness once QApplication is up and the file log sink is off by
    /// default, so the startup recovery path had no observable channel at all.
    void recoveryTrace(const char* what);

    // Annotation support
    std::unique_ptr<pdfe::annotations::AnnotationManager> annotationManager_;

    // Pipeline E phase-4: image stamp library (custom user stamps)
    std::unique_ptr<pdfe::annotations::ImageStampLibrary> imageStampLibrary_;

    // Form support
    std::unique_ptr<pdfe::forms::FormManager> formManager_;
    std::unique_ptr<pdfe::forms::JavaScriptEngine> jsEngine_;
    std::unique_ptr<pdfe::forms::FormDataIO> formDataIO_;

    // UI components
    PdfCanvas* canvas_ = nullptr;
    // GitHub #303 -- restores keyboard focus when a UIA write to a
    // hidden test pipe steals it. Owned by this window.
    pdfe::ui::TestPipeFocusGuard* testPipeFocusGuard_ = nullptr;
    // GitHub #60 (SEC-62) — active-content advisory banner. The canvas is no
    // longer the central widget directly; it shares a vertical central area
    // with this banner, which sits above it and is hidden unless the document
    // just opened actually carries active content.
    //
    // Deliberately a QFrame in the widget tree and NOT a QDialog: a Qt
    // app-modal dialog is invisible to UIA (measured: four minutes open, a
    // 240 s poll saw nothing), so a modal advisory cannot be asserted by the
    // UI-automation layer at all. objectName/accessibleName are part of the
    // contract, not decoration — see tests/test_prohibited_feature_banner.cpp.
    QWidget* centralArea_ = nullptr;
    QFrame* prohibitedFeatureBanner_ = nullptr;
    QLabel* prohibitedFeatureBannerText_ = nullptr;
    QToolButton* prohibitedFeatureBannerDismiss_ = nullptr;
    QDockWidget* pagePanelDock_ = nullptr;
    PagePanel* pagePanel_ = nullptr;
    QDockWidget* propertiesPanelDock_ = nullptr;
    PropertiesPanel* propertiesPanel_ = nullptr;
    QDockWidget* searchPanelDock_ = nullptr;
    SearchPanel* searchPanel_ = nullptr;
    QDockWidget* bookmarkPanelDock_ = nullptr;
    BookmarkPanel* bookmarkPanel_ = nullptr;
    QDockWidget* layerPanelDock_ = nullptr;
    LayerPanel* layerPanel_ = nullptr;
    QAction* toggleLayerPanelAction_ = nullptr;
    QDockWidget* commentPanelDock_ = nullptr;
    CommentPanel* commentPanel_ = nullptr;
    QDockWidget* annotationPropertiesDock_ = nullptr;
    AnnotationPropertiesWidget* annotationProperties_ = nullptr;
    QDockWidget* formPropertiesDock_ = nullptr;
    FormFieldPropertiesPanel* formPropertiesPanel_ = nullptr;
    QDockWidget* redactionPanelDock_ = nullptr;
    RedactionPanel* redactionPanel_ = nullptr;
    StatusBar* statusBar_ = nullptr;
    StickyNotePopup* stickyNotePopup_ = nullptr;
    QDockWidget* defaultsLogDock_ = nullptr;
    DefaultsLogPanel* defaultsLogPanel_ = nullptr;
    QDockWidget* attachmentPanelDock_ = nullptr;
    AttachmentPanel* attachmentPanel_ = nullptr;
    QAction* toggleAttachmentPanelAction_ = nullptr;

    // Actions - File
    QAction* newFileAction_ = nullptr;
    QAction* openAction_ = nullptr;
    QAction* saveAction_ = nullptr;
    QAction* saveAsAction_ = nullptr;
    QAction* closeAction_ = nullptr;
    QAction* printAction_ = nullptr;
    QAction* printPreviewAction_ = nullptr;
    QAction* exitAction_ = nullptr;
    QAction* exportPlainTextAction_ = nullptr;
    QAction* createPdfFromImagesAction_ = nullptr;
    QAction* combineFilesAction_ = nullptr;
    QAction* previousViewAction_ = nullptr;
    QAction* nextViewAction_ = nullptr;

    NavigationHistory navHistory_;

    // Actions - Edit
    QAction* undoAction_ = nullptr;
    QAction* redoAction_ = nullptr;
    QAction* copyAction_ = nullptr;
    QAction* cutAction_ = nullptr;
    QAction* pasteAction_ = nullptr;
    QAction* selectAllAction_ = nullptr;
    QAction* findAction_ = nullptr;
    QAction* replaceAction_ = nullptr;
    QAction* preferencesAction_ = nullptr;
    /// Pipeline F phase-10: opens TrustAnchorDialog (Edit > Trust Anchors...).
    QAction* trustAnchorAction_ = nullptr;

    // Actions - View
    QAction* rotateViewAction_ = nullptr;         // objectName: actionRotateViewCW
    QAction* rotateViewCCWAction_ = nullptr;      // objectName: actionRotateViewCCW
    QAction* rotateView180Action_ = nullptr;      // objectName: actionRotateView180
    QAction* resetViewRotationAction_ = nullptr;  // objectName: actionResetViewRotation
    QAction* themeToggleAction_ = nullptr;  // legacy: kept for back-compat
    // Pipeline F phase-5: View > Theme submenu (Light / Dark / System)
    QAction* themeLightAction_ = nullptr;
    QAction* themeDarkAction_ = nullptr;
    QAction* themeSystemAction_ = nullptr;
    QAction* zoomInAction_ = nullptr;
    QAction* zoomOutAction_ = nullptr;
    QAction* fitWidthAction_ = nullptr;
    QAction* fitPageAction_ = nullptr;
    QAction* actualSizeAction_ = nullptr;
    QAction* fullScreenAction_ = nullptr;
    QAction* togglePagePanelAction_ = nullptr;
    QAction* togglePropertiesPanelAction_ = nullptr;
    QAction* toggleSearchPanelAction_ = nullptr;
    QAction* toggleBookmarkPanelAction_ = nullptr;
    QAction* toggleOcrBoxesAction_ = nullptr;
    bool ocrBoxesEnabled_ = false;

    // Actions - Pages
    QAction* rotatePageCWAction_ = nullptr;
    QAction* rotatePageCCWAction_ = nullptr;
    QAction* rotatePage180Action_ = nullptr;
    QAction* deletePageAction_ = nullptr;
    /// Cached enabled state of @ref deletePageAction_ while a text edit
    /// session has the action temporarily disabled (so the Delete key
    /// forward-deletes characters instead of triggering page deletion).
    bool deletePageActionWasEnabled_ = false;
    QAction* insertBlankPageAction_ = nullptr;
    QAction* insertPagesFromFileAction_ = nullptr;
    QAction* extractPagesAction_ = nullptr;
    QAction* movePageUpAction_ = nullptr;
    QAction* movePageDownAction_ = nullptr;

    // Actions - Text Edit
    QAction* textEditToolAction_ = nullptr;
    TextEditToolBar* textEditToolBar_ = nullptr;

    // Actions - TRE (Scanned Text Edit)
    QAction* treToolAction_ = nullptr;

    // Actions - Forms
    QAction* formToolAction_ = nullptr;
    QAction* createFormFieldAction_ = nullptr;
    QAction* formFieldPropertiesAction_ = nullptr;
    QAction* exportFormDataAction_ = nullptr;
    QAction* importFormDataAction_ = nullptr;
    QAction* resetFormAction_ = nullptr;
    QAction* flattenFormAction_ = nullptr;
    QAction* tabOrderAction_ = nullptr;
    // Pipeline F phase-5: form-wide calculation order (/AcroForm /CO)
    QAction* calculationOrderAction_ = nullptr;
    // Pipeline F phase-5: alt text editor for /Alt + /ActualText
    QAction* altTextAction_ = nullptr;
    // Pipeline H phase-4: selection-aware AutomationId path. Enabled
    // iff exactly one annotation is currently selected; the
    // triggered slot opens AltTextDialog for that annotation.
    // Coexists with altTextAction_ (which operates on annotation 0
    // of the current page unconditionally) — the two serve
    // different UX and test scenarios.
    QAction* openAltTextDialogAction_ = nullptr;
    int selectedAnnotationPageIndex_ = -1;
    int selectedAnnotationIndex_ = -1;
    QAction* submitFormAction_ = nullptr;

    // Form field creation toolbar
    QToolBar* formToolBar_ = nullptr;
    QAction* formTextFieldAction_ = nullptr;
    QAction* formCheckboxAction_ = nullptr;
    QAction* formRadioButtonAction_ = nullptr;
    QAction* formComboBoxAction_ = nullptr;
    QAction* formListBoxAction_ = nullptr;
    QAction* formPushButtonAction_ = nullptr;
    QAction* formSignatureAction_ = nullptr;

    // Actions - Security (Redaction)
    QAction* redactToolAction_ = nullptr;
    QAction* markRedactionAction_ = nullptr;
    QAction* applyRedactionsAction_ = nullptr;

    // Actions - Security (Encryption)
    QAction* saveWithEncryptionAction_ = nullptr;
    QAction* documentPermissionsAction_ = nullptr;
    QAction* removePasswordAction_ = nullptr;

    // Actions - Security (Signatures)
    QAction* signDocumentAction_ = nullptr;
    QAction* verifySignaturesAction_ = nullptr;
    /// Pipeline F phase-9. Visible always under Security menu but
    /// disabled until the document has at least one verified signature.
    QAction* requestValidationAction_ = nullptr;

    // Actions - Help
    QAction* aboutAction_ = nullptr;
    QAction* documentationAction_ = nullptr;
    QAction* reportIssueAction_ = nullptr;
    QAction* checkUpdatesAction_ = nullptr;

    // Toolbar widgets
    QComboBox* zoomCombo_ = nullptr;

    // Annotation toolbar
    QToolBar* annotationToolBar_ = nullptr;
    QActionGroup* annotToolGroup_ = nullptr;
    QAction* selectToolAction_ = nullptr;
    QAction* stickyNoteToolAction_ = nullptr;
    QAction* highlightToolAction_ = nullptr;
    QAction* underlineToolAction_ = nullptr;
    QAction* strikeOutToolAction_ = nullptr;
    QAction* inkToolAction_ = nullptr;
    QAction* freeTextToolAction_ = nullptr;
    QAction* rectangleToolAction_ = nullptr;
    QAction* circleToolAction_ = nullptr;
    QAction* lineToolAction_ = nullptr;

    // Current tool's page (kept alive while tool is active)
    std::unique_ptr<pdfe::core::Page> toolPage_;

    // Pages kept alive for commands in the undo stack
    std::vector<std::unique_ptr<pdfe::core::Page>> commandPages_;

    // Clipboard-pasted images, materialised to temp PNGs that
    // InsertImageCommand::redo() re-reads BY PATH after every undo/redo.
    // Holding the PIN for the window's lifetime is the point (WIN-87 wider
    // class, phase-60): the file must stay unsubstitutable for exactly as long
    // as the undo stack can still reach back to it, which is this long.
    // ~PinnedTempFile deletes them on teardown, so nothing leaks to %TEMP%
    // either — the old code deliberately left them behind forever.
    std::vector<std::unique_ptr<pdfe::core::PinnedTempFile>> pinnedPastedImages_;

    // Current annotation color
    QColor currentAnnotColor_{255, 255, 0}; // Default yellow
    QPushButton* colorPickerButton_ = nullptr;

    QString currentFileName_;

    /// GitHub #241 — see setDropRoutingDelegated().
    bool dropRoutingDelegated_ = false;

    /// GitHub #240 — local copies staged for the preflight child, kept alive for
    /// the window's lifetime because the preflight runs after the staging call
    /// returns. QTemporaryFile removes each one on destruction.
    std::vector<std::unique_ptr<QTemporaryFile>> preflightStagingFiles_;

    /// Test seam: shortens the network reachability budget so a test does not
    /// wait seconds for a deliberately unreachable host. Zero means production.
    int networkProbeTimeoutMsForTesting_ = 0;

    // Name shown to the USER for the open document (title bar, status bar,
    // dialog bodies) when it must differ from the basename of
    // currentFileName_. File > New routes through openFile() on a temp file
    // named pdfe_untitled_<epoch_ms>.pdf; the user sees tr("Untitled")
    // instead. Empty means "just use the basename of currentFileName_".
    QString documentDisplayName_;
    // Set immediately before an openFile() call that needs a display name;
    // openFile() consumes and clears it on entry, so it can never leak into
    // a later, unrelated open.
    QString pendingDisplayName_;

    // R3-12: current copy-selection region for image copy-out. When
    // copySelectionValid_ is true, onCopy() renders copySelectionRect_ on
    // copySelectionPage_ to an image and puts it on the clipboard instead of
    // copying whole-page text. In PDF user-space (bottom-left origin, points).
    QRectF copySelectionRect_;
    int copySelectionPage_ = -1;
    bool copySelectionValid_ = false;

    bool textEditAutoRouting_ = false;
    bool skipGroupTextEditCall_ = false;  // Prevents QActionGroup double-delivery

    // TRE (Text Reconstruction Engine) infrastructure -- lazy-initialized
    std::unique_ptr<pdfe::fonts::EmbeddedFontAnalyzer> embeddedFontAnalyzer_;
    std::unique_ptr<pdfe::fonts::FontAnalysisCache> fontAnalysisCache_;
    std::unique_ptr<pdfe::fonts::SubstituteFontSelector> substituteFontSelector_;
    std::unique_ptr<pdfe::fonts::PdfFontEmbedder> pdfFontEmbedder_;
    std::unique_ptr<pdfe::tre::OcrPipeline> ocrPipeline_;
    std::unique_ptr<pdfe::tre::CoordMapper> coordMapper_;
    std::unique_ptr<pdfe::tre::BgColorDetector> bgColorDetector_;
    std::unique_ptr<pdfe::tre::FontMatcher> fontMatcher_;
    std::unique_ptr<pdfe::tre::CNNFontClassifier> cnnFontClassifier_;
    std::unique_ptr<pdfe::tre::QualityAssessor> qualityAssessor_;
    std::unique_ptr<pdfe::tre::ImageMasker> imageMasker_;
    std::unique_ptr<pdfe::tre::BitmapCompositor> bitmapCompositor_;
    std::unique_ptr<pdfe::tre::PdfContentWriter> pdfContentWriter_;
    std::unique_ptr<pdfe::tre::RegionReconstructor> regionReconstructor_;
    std::unique_ptr<pdfe::tre::ImageReplacer> imageReplacer_;

    // Cached enriched atlas for WYSIWYG save path (same atlas used by overlay)
    std::shared_ptr<pdfe::tre::EnrichedAtlas> currentEnrichedAtlas_;

    // Atlas mode selection
    pdfe::tre::AtlasMode currentAtlasMode_ = pdfe::tre::AtlasMode::ScanCropped;
    std::shared_ptr<pdfe::tre::EnrichedAtlas> scanCroppedAtlas_;
    std::shared_ptr<pdfe::tre::EnrichedAtlas> freeTypeStyledAtlas_;
    bool atlasChoiceMade_ = false;
    /// Persisted state of the AtlasComparisonDialog "Fill missing
    /// from font" checkbox. Survives dialog close+reopen within the
    /// session; reset on document close.
    bool fillMissingPersisted_ = false;

    /// Pipeline b3f19d72 phase-8: set to true whenever onPrint() runs
    /// with PDFE_TEST_DRYRUN_PRINT=1 active.  Reset by
    /// consumeLastPrintDryRunFired().
    bool lastPrintDryRunFired_ = false;

    /// Pipeline b3f19d72 phase-11: hidden QLineEdit invocation pipe.
    /// Python pywinauto tests write a QAction objectName to this widget via
    /// UIA set_edit_text(); textChanged fires activateToolByObjectName().
    /// Always invisible to the user (size 0×0, no tab stop).
    class QLineEdit* testToolActivatePipe_ = nullptr;

    /// GitHub #271 / #294. Acknowledgement for testToolActivatePipe_.
    ///
    /// Written AFTER the deferred trigger has actually run, as
    /// "<seq>:<actionObjectName>:<armed|triggered>".
    ///
    /// Why an acknowledgement is needed at all: activateToolByObjectName()
    /// defers the trigger with QTimer::singleShot(0, ...) — deliberately,
    /// because many actions open a modal QDialog::exec() that would block the
    /// pipe's textChanged handler and time out the UIA set_edit_text() RPC. So
    /// its `true` means "found and queued", NOT "armed", and a caller that
    /// treats the write as completion is racing the trigger.
    ///
    /// #294 measured what that costs: the 1000-annotation stress loop re-arms a
    /// single-shot tool by clicking the toolbar at fixed coordinates, and the
    /// click does not re-arm. Every later canvas click went to SelectTool and
    /// created nothing, so the loop persisted ONE annotation out of 1000 while
    /// its own counters reported success. Nothing in the harness could see it.
    ///
    /// The SEQUENCE NUMBER is load-bearing. Without it a caller polling this
    /// pipe can read the PREVIOUS iteration's acknowledgement and proceed — a
    /// false completion, which is the same class of defect as the counters that
    /// hid #294 in the first place. The caller must observe the number CHANGE.
    ///
    /// "armed" means a tool object is installed on the canvas after the
    /// trigger; "triggered" means the action fired but left no tool (a non-tool
    /// action, or one whose tool construction failed). The distinction is
    /// reported rather than collapsed, because a caller re-arming a tool needs
    /// the first and a caller invoking a menu action only needs the second.
    class QLineEdit* testToolActivateResultPipe_ = nullptr;

    // GitHub #271. Batched undo/redo for the 1000-annotation stress loop,
    // which drove Ctrl+Z 1010 times through real SendInput and undid NOTHING
    // while reporting success -- it counted keystrokes sent. The result pipe
    // carries the number of operations performed, so the caller asserts on an
    // outcome rather than on its own loop index.
    class QLineEdit* testUndoRepeatPipe_ = nullptr;
    class QLineEdit* testUndoRepeatResultPipe_ = nullptr;
    // Set while a batch is running. runTestUndoRedoRepeat calls processEvents
    // between operations, which lets another write to the pipe -- or a deferred
    // trigger -- run inside it, so without this two batches can walk the same
    // undo stack at once.
    bool testUndoRepeatBusy_ = false;
    unsigned long long testUndoRepeatSeq_ = 0;

    // GitHub #295. Ctrl+Z after creating an annotation undoes nothing, while
    // triggering undoAction through the pipe works and the batched seam above
    // performs exactly as many undos as the stack holds. So the stack is
    // populated and undo works; what fails is the KEYSTROKE reaching the
    // QAction. The issue names two surviving hypotheses -- a focused child
    // widget consuming Ctrl+Z for its own undo, or undoAction_ transiently
    // disabled at the instant the key lands -- and they differ in whether a
    // real user is affected.
    //
    // Separating them needs the focused widget's identity and the action's
    // enabled state AT THE MOMENT OF DELIVERY, which is why this RECORDS rather
    // than answers a query: by the time a test can ask, focus and the enabled
    // state have both moved on. Measured in text-edit mode for contrast,
    // Ctrl+Z works there (tests/test_ui_text_edit_undo.py passes), so whatever
    // this catches is specific to the annotation path.
    //
    // Gated on testPipesEnabled(); absent in production.
    class QLineEdit* testKeyTraceQueryPipe_ = nullptr;
    class QLineEdit* testKeyTraceResultPipe_ = nullptr;
    /// One ';'-separated record per observed Ctrl+Z, oldest first. Bounded so a
    /// long run cannot grow it without limit.
    QStringList testKeyTrace_;
    static constexpr int kTestKeyTraceMax = 64;
    /// When true the trace records EVERY key event, not only Ctrl+Z. Set by
    /// writing "all" to the query pipe. Exists because the first run of the
    /// #295 probe recorded nothing at all, and "no Ctrl+Z arrived" cannot be
    /// distinguished from "no key arrived" by a Ctrl+Z-only filter — both are
    /// silence, with different owners.
    bool testKeyTraceAll_ = false;
    /// Installed on qApp so it sees the event before any focused widget can
    /// consume it -- a filter on MainWindow alone would miss exactly the case
    /// hypothesis 1 describes.
    class ShortcutTraceFilter* testKeyTraceFilter_ = nullptr;
    /// Appends one record for a Ctrl+Z event. Called only by the filter, which
    /// is a friend rather than this being public: nothing outside the trace
    /// seam has any business writing into the trace.
    void recordCtrlZTrace(const char* eventKind, class QWidget* focus);
    friend class ShortcutTraceFilter;

    /// Monotonic counter for testToolActivateResultPipe_. Never reset.
    unsigned long long testToolActivateSeq_ = 0;

    /// Pipeline b3f19d72 phase-12: hidden QLineEdit invocation pipe for
    /// PdfCanvas::clickByPdfCoord.  Python pywinauto tests write a string
    /// of the form "pageIndex,pdfX,pdfY" (e.g. "0,280.0,700.0") via
    /// UIA set_edit_text(); textChanged parses the tuple and dispatches
    /// the synthesized mouse-press/release through the FormTool hit-test
    /// pipeline, exactly as a real user click would.
    class QLineEdit* testClickByPdfCoordPipe_ = nullptr;

    /// Pipeline 7-loop: like testClickByPdfCoordPipe_ but dispatches a
    /// double-click (PdfCanvas::doubleClickByPdfCoord) so a UI test can
    /// trigger TextEditTool word-selection deterministically at a PDF
    /// coordinate, immune to canvas letterbox margins.  Payload format
    /// "pageIndex,pdfX,pdfY".
    class QLineEdit* testDoubleClickByPdfCoordPipe_ = nullptr;

    /// Pipeline 7-loop: deliver text/keys to the active text-edit tool.
    /// Payload "@KEY:<int>" sends a single Qt::Key (e.g. Backspace);
    /// any other payload is typed as text via PdfCanvas::typeIntoTextEdit.
    /// Substitutes OS keyboard delivery to the focusless custom canvas.
    class QLineEdit* testTextEditTypePipe_ = nullptr;

    /// Pipeline b3f19d72 phase-15: hidden two-pipe readback seam for
    /// getCurrentAnnotationAltText().  Python tests write any non-empty
    /// string to testAltTextReadbackTriggerPipe_; the textChanged handler
    /// calls getCurrentAnnotationAltText() and writes the result to
    /// testAltTextReadbackResultPipe_.  Python then reads the result via
    /// UIA ValuePattern on testAltTextReadbackResultPipe_.
    class QLineEdit* testAltTextReadbackTriggerPipe_ = nullptr;
    class QLineEdit* testAltTextReadbackResultPipe_ = nullptr;

    /// Pipeline 7-loop: hidden two-pipe readback seam for
    /// commentPanelRowInventory().  Python tests write any non-empty
    /// string to testCommentInventoryTriggerPipe_; the textChanged
    /// handler calls commentPanelRowInventory() and writes the
    /// newline-joined row-text result to testCommentInventoryResultPipe_.
    /// Substitutes the Server-2019 Qt6 UIA QTreeWidget enumeration path,
    /// which does not reliably surface comment-tree rows.
    class QLineEdit* testCommentInventoryTriggerPipe_ = nullptr;
    class QLineEdit* testCommentInventoryResultPipe_ = nullptr;

    /// Pipeline 7-loop: hidden two-pipe readback seam for
    /// textEditSelectionDiag().  Python writes any non-empty string to
    /// the trigger; the handler writes "spanCount,activeSpanIndex,
    /// selStart,selEnd" to the result pipe.
    class QLineEdit* testTextEditDiagTriggerPipe_ = nullptr;
    class QLineEdit* testTextEditDiagResultPipe_ = nullptr;

    /// Pipeline 9a4e2f71 phase-2: hidden write-only seam to drive a font-size
    /// change through the toolbar deterministically. Python writes a float
    /// (points); the handler calls TextEditToolBar::setFontSizeForTesting(),
    /// firing the real fontSizeChanged → applyFontSize path. Replaces a UIA
    /// spinbox set-value gesture that no-ops in the non-interactive CI runner
    /// session (CI-LIMITATION-WINDOWS-VALIDATION-UI-E2E-FLAKES item #2). The
    /// applied size is read back via textEditSelectionDiag()'s ";runsz=" field.
    class QLineEdit* testToolbarFontSizePipe_ = nullptr;

    /// Pipeline 9a4e2f71 phase-2: hidden two-pipe readback for the TextEditToolBar
    /// visibility. Python writes any non-empty string to the query; the handler
    /// returns "1"/"0" = textEditToolBar_->isVisible(). Deterministic ground
    /// truth for "is the toolbar shown", replacing flaky UIA enumeration of the
    /// toolbar's QFontComboBox child, which the Qt6 UIA bridge does not reliably
    /// expose for a QToolBar on Server 2019
    /// (CI-LIMITATION-WINDOWS-VALIDATION-UI-E2E-FLAKES item #1).
    class QLineEdit* testToolbarVisibleQueryPipe_ = nullptr;
    class QLineEdit* testToolbarVisibleResultPipe_ = nullptr;

    /// Pipeline e7d3c2a1 phase-2: hidden two-pipe readback seam for
    /// activeToolDiag() — confirms the per-region tool routing decision.
    class QLineEdit* testActiveToolQueryPipe_ = nullptr;
    class QLineEdit* testActiveToolResultPipe_ = nullptr;

    /// Pipeline e7d3c2a1 phase-3: hidden two-pipe readback seam for
    /// currentPageOcrText() — drives OCR on the current page and returns the
    /// recognized plain text so the multilingual (#3) render test can assert
    /// Arabic-script output without a pixel oracle.
    class QLineEdit* testOcrTextQueryPipe_ = nullptr;
    /// Page-rect readback seam (see MainWindow.cpp). Test-only.
    /// Zoom-set seam (see MainWindow.cpp). Test-only.
    class QLineEdit* testSetZoomPipe_ = nullptr;
    /// View-state readback seam (see MainWindow.cpp). Test-only.
    class QLineEdit* testViewStateQueryPipe_ = nullptr;
    class QLineEdit* testViewStateResultPipe_ = nullptr;
    class QLineEdit* testPageRectQueryPipe_ = nullptr;
    class QLineEdit* testPageRectResultPipe_ = nullptr;
    class QLineEdit* testOcrTextResultPipe_ = nullptr;

    /// Pipeline c91e7f3a phase-5: hidden pipe seam to set a form field's
    /// text value via FormFieldPropertiesPanel::setTextValueForTesting().
    /// Python tests write the desired value string; the textChanged handler
    /// calls setTextValueForTesting(value) which directly fires the
    /// fieldValueChanged signal → onFormFieldValueChanged() creates a
    /// SetFieldValueCommand, bypassing the UIA ValuePattern → editingFinished
    /// race that leaves the field uncommitted.
    class QLineEdit* testFormFieldValueSetPipe_ = nullptr;

    /// Pipeline c91e7f3a phase-7: form field value read-back pipe pair.
    /// Python tests write any non-empty token to testFormFieldValueQueryPipe_
    /// to trigger a read of the currently-selected form field's text value.
    /// The result is written to testFormFieldValueResultPipe_ (accessed via
    /// get_form_field_panel_value() in qinvoke.py).  Bypasses the UIA
    /// window.descendants() / FindAll traversal that can deadlock when PDFE's
    /// main thread is busy painting the FormFieldPropertiesPanel after a
    /// click_by_pdf_coord dispatch.
    class QLineEdit* testFormFieldValueQueryPipe_  = nullptr;
    class QLineEdit* testFormFieldValueResultPipe_ = nullptr;

    /// Pipeline f2a8c3e1 phase-2: annotation property pipe seams.
    /// Writing "pageIdx,annotIdx" to testAnnotSelectPipe_ selects an annotation
    /// by index, bypassing the flaky SelectTool canvas-click hit-test.
    /// Writing "R,G,B" to testAnnotColorPipe_ applies the color directly to
    /// the selected annotation via onAnnotationPropertiesChanged(), bypassing
    /// the QColorDialog UIA dance which is non-deterministic.
    /// Writing text to testAnnotAuthorPipe_ / testAnnotContentsPipe_ applies
    /// the author / contents field directly, bypassing UIA ValuePattern on
    /// QLineEdit/QTextEdit (which races against the Qt event loop).
    /// All pipes are gated by PDFE_TEST_ANNOT_PIPES=1; inert in production.
    class QLineEdit* testAnnotSelectPipe_   = nullptr;  ///< objectName = "testAnnotSelectPipe"
    class QLineEdit* testAnnotColorPipe_    = nullptr;  ///< objectName = "testAnnotColorPipe"
    class QLineEdit* testAnnotAuthorPipe_   = nullptr;  ///< objectName = "testAnnotAuthorPipe"
    class QLineEdit* testAnnotContentsPipe_ = nullptr;  ///< objectName = "testAnnotContentsPipe"
    class QLineEdit* testAnnotDeletePipe_   = nullptr;  ///< objectName = "testAnnotDeletePipe"
    /// c82a1f37 test-hardening: deterministic annotation CREATE seam. Python
    /// writes "type,page,x,y,w,h"; invokes createAnnotationForTest() directly
    /// (bypasses the non-deterministic synthetic-click create that flaked in CI).
    class QLineEdit* testAnnotCreatePipe_   = nullptr;  ///< objectName = "testAnnotCreatePipe"

    /// Pipeline c91e7f3a phase-3: TextEditToolBar fontComboBox readback + set
    /// pipes.  Qt6's UIA bridge does not reliably expose QFontComboBox contents
    /// when the toolbar is mounted via addToolBar() on Windows Server 2019.
    ///
    /// testFontFamiliesQueryPipe_ (objectName = "testFontFamiliesQueryPipe"):
    ///   Write any non-empty string → handler writes newline-separated font
    ///   families to testFontFamiliesResultPipe_.  First line is the current
    ///   selection; remaining lines are all entries in order.
    ///
    /// testFontFamiliesResultPipe_ (objectName = "testFontFamiliesResultPipe"):
    ///   Read-only result sink; Python reads via UIA ValuePattern.
    ///
    /// testSetFontFamilyPipe_ (objectName = "testSetFontFamilyPipe"):
    ///   Write a font family name → handler calls
    ///   textEditToolBar_->setCurrentText(family) and emits fontFamilyChanged.
    ///   Bypasses the unreachable QFontComboBox UIA ValuePattern.
    class QLineEdit* testFontFamiliesQueryPipe_  = nullptr;
    class QLineEdit* testFontFamiliesResultPipe_ = nullptr;
    class QLineEdit* testSetFontFamilyPipe_      = nullptr;

    /// Pipeline c94e2f17 phase-4.5 (GAP-61): hidden query/result pipes
    /// for context-menu QAction discovery. Bypasses Qt6's UIA bridge
    /// which does not expose synchronous QMenu::exec popups. Test
    /// writes a panel objectName ("pagePanel", "bookmarkPanel",
    /// "commentPanel") to testContextActionsQueryPipe_; the handler
    /// walks the panel's QAction children via findChildren<QAction*>
    /// and writes a '|'-separated list of their objectNames to
    /// testContextActionsResultPipe_. Gated by PDFE_TEST_PIPES.
    class QLineEdit* testContextActionsQueryPipe_  = nullptr;
    class QLineEdit* testContextActionsResultPipe_ = nullptr;

    /// Pipeline b3d7f942 phase-3 (GAP-5): hidden pipe for
    /// PagePanel::movePageProgrammatic.  Python tests write
    /// "fromIndex,toIndex"; handler calls
    /// pagePanel_->movePageProgrammatic(from, to) which emits
    /// pageMoved(from, to) → MovePageCommand pipeline.
    /// Gated by PDFE_TEST_PIPES; nullptr in production.
    class QLineEdit* testMovePagePipe_ = nullptr;

    /// GitHub #100: hidden pipe that installs a SaveSpacePrecheck free-space
    /// probe reporting a fixed byte count, so a live-UI test can make the
    /// volume look full and assert the "Low disk space" warning dialog
    /// actually appears. Payload is a decimal byte count, or "off" to restore
    /// the real `std::filesystem::space` reading.
    /// Gated by PDFE_TEST_PIPES; nullptr in production.
    class QLineEdit* testSimulateFreeSpacePipe_ = nullptr;

    /// Pipeline b5e3a9f2 phase-6 (P-GAP-73): hidden pipe for
    /// PagePanel::selectPagesProgrammatic. Python tests write a
    /// comma-separated list of page indices (e.g. "0,2,3"); handler
    /// calls pagePanel_->selectPagesProgrammatic({0,2,3}) which sets
    /// the QListWidget's selection model directly. Bypasses the
    /// pyautogui Ctrl/Shift+click route which is flaky against
    /// Qt6 ExtendedSelection mode. Gated by PDFE_TEST_PIPES.
    class QLineEdit* testSelectPagesPipe_ = nullptr;

    /// Pipeline f7c2a4e8 phase-3 (B1): hidden pipe for PagePanel::
    /// dragItemProgrammatic("fromIdx,toIdx"). Gated by PDFE_TEST_PIPES.
    class QLineEdit* testDragPipe_ = nullptr;

    /// Hidden pipe to navigate the canvas to a specific page
    /// deterministically. Python tests write a single 0-based page index
    /// (e.g. "1"); the handler calls canvas_->goToPage(n), which sets
    /// PdfCanvas::currentPage() without depending on OS keyboard focus
    /// (the {PGDN} type_keys route is foreground-focus-dependent and flaky
    /// on the Server-2019 CI session). The page-move menu actions
    /// (onMovePageUp/onMovePageDown) read canvas_->currentPage() as the
    /// move source, so a test must be able to place the canvas on a
    /// known page deterministically. Gated by PDFE_TEST_PIPES; nullptr
    /// in production.
    class QLineEdit* testGoToPagePipe_ = nullptr;

    /// Pipeline f7c2a4e8 phase-3 (B2): hidden pipe for PdfCanvas::
    /// dragByPdfCoord("pageIdx,fromX,fromY,toX,toY"). Gated by
    /// PDFE_TEST_PIPES.
    class QLineEdit* testCanvasDragPipe_ = nullptr;

    /// c82a1f37 P4: query/result pair exposing the selected annotation's
    /// resize/rotate handle centroids in fitz coords ("Name:x,y;...").
    /// Gated by PDFE_TEST_PIPES; nullptr in production.
    class QLineEdit* testAnnotHandlesTriggerPipe_ = nullptr;
    class QLineEdit* testAnnotHandlesResultPipe_ = nullptr;

    /// c82a1f37 P5: deterministic form-field create seam. Python writes
    /// "type,page,x,y,w,h"; invokes createFormField() directly (bypasses the
    /// non-deterministic real-mouse drag). Gated by PDFE_TEST_PIPES.
    class QLineEdit* testFieldCreatePipe_ = nullptr;

    /// Pipeline f7c2a4e8 phase-3 (B2): hidden pipe for PdfCanvas::
    /// tripleClickByPdfCoord("pageIdx,x,y"). Gated by PDFE_TEST_PIPES.
    class QLineEdit* testTripleClickPipe_ = nullptr;

    /// Pipeline f7c2a4e8 phase-3 (B3): hidden pipe for PdfCanvas::
    /// scrollByPixels("delta"). Gated by PDFE_TEST_PIPES.
    class QLineEdit* testScrollPipe_ = nullptr;

    /// Pipeline b3d7f942 phase-3 (GAP-6/GAP-7): hidden pipe for
    /// PdfCanvas::addRedactionMarkAtPdfRect.  Python tests write
    /// "pageIndex,x,y,w,h" (fitz-space coords: origin top-left, Y-down,
    /// points); handler calls canvas_->addRedactionMarkAtPdfRect() which
    /// emits redactionMarkRequested → CreateRedactionCommand pipeline.
    /// Gated by PDFE_TEST_PIPES; nullptr in production.
    class QLineEdit* testAddRedactionMarkPipe_ = nullptr;

    /// Pipeline b3d7f942 phase-3 (GAP-7): hidden query+result pipe pair
    /// for reading RedactionPanel's pending mark count.
    /// Python tests write any non-empty token to the query pipe; handler
    /// reads redactionPanel_->topLevelItemCount() and writes the integer
    /// (as a string) to the result pipe.  Bypasses the UIA QTreeWidget
    /// cell-inflation issue (4 DataItem children per row × N rows).
    /// Gated by PDFE_TEST_PIPES; nullptr in production.
    class QLineEdit* testRedactionMarkCountQueryPipe_ = nullptr;
    class QLineEdit* testRedactionMarkCountResultPipe_ = nullptr;

    /// Pipeline a7c4f891 phase-2 (D3 / P-GAP-84): hidden query+result
    /// pipe pair for reading PdfCanvas::viewRotation(). Python tests
    /// write any non-empty token to the query pipe; handler reads
    /// canvas_->viewRotation() (returning 0/90/180/270) and writes
    /// the integer (as a string) to the result pipe. Bypasses the
    /// PdfCanvasAccessible UIA tree which does not expose the
    /// rotation state as a Description. Gated by PDFE_TEST_PIPES;
    /// nullptr in production.
    class QLineEdit* testCanvasRotationQueryPipe_ = nullptr;
    class QLineEdit* testCanvasRotationResultPipe_ = nullptr;

    /// Pipeline a7c4f891 phase-2 (D4 / P-GAP-82): hidden pipe for
    /// BookmarkPanel::insertBookmark. Python tests write
    /// "pageIndex,title". Handler parses and calls
    /// bookmarkPanel_->insertBookmark(pageIndex, title). Provides
    /// a PASS path for P-GAP-82 independent of QMenu UIA
    /// reachability. Gated by PDFE_TEST_PIPES; nullptr in production.
    class QLineEdit* testInsertBookmarkPipe_ = nullptr;

    /// Pipeline a7c4f891 phase-3 (E2 / P12-INFRA-1 sub-issue 5):
    /// hidden query+result pipe pair for reading printAction_->
    /// isEnabled(). printAction is only added to fileMenu, so its
    /// UIA exposure depends on the menu being open. The pipe gives
    /// tests a foreground-independent path to verify permission
    /// gating reflects in the action's enabled state. Result pipe
    /// returns "1" for enabled, "0" for disabled, "-1" if printAction_
    /// is null. Gated by PDFE_TEST_PIPES; nullptr in production.
    class QLineEdit* testPrintActionEnabledQueryPipe_ = nullptr;
    class QLineEdit* testPrintActionEnabledResultPipe_ = nullptr;

    /// Pipeline b5e3a9f2 phase-4 (GAP-51): hidden pipe for invoking
    /// Document::saveAs() programmatically, bypassing the QFileDialog
    /// + native Windows overwrite-confirmation modal that the
    /// integration test cannot reliably drive. Python tests write the
    /// absolute target path; handler calls document_->saveAs(path)
    /// directly. Gated by PDFE_TEST_PIPES; nullptr in production.
    /// GAP-51 was NOT a product bug: Document::saveAs() correctly
    /// overwrites (proven by SaveTest.SaveAs_OverwritesExistingFile).
    /// The integration-test failure was that the test's
    /// `fx.main_window.type_keys("{ENTER}")` for the overwrite-confirm
    /// modal does not reach the modal because it is system-modal, not
    /// a child of main_window.
    class QLineEdit* testSaveAsPipe_ = nullptr;

    /// GitHub #220: hidden pipe for invoking page extraction programmatically.
    ///
    /// Mirrors testSaveAsPipe_ deliberately. onExtractPages() ALREADY honours
    /// testseam::takeFileDialogPath(), so the save dialog was never the whole
    /// blocker -- the residue was that reaching `Pages -> Extract Pages...`
    /// needs a UIA menu walk that does not reach the leaf reliably on this
    /// host, and the test then had no truthful oracle either. This pipe
    /// removes both: it dispatches core::extractPagesToFile() directly, so the
    /// oracle becomes "the written file has the requested page count", read
    /// back by the test with pymupdf.
    ///
    /// Payload: `"<idx>[,<idx>...];<absolute-output-path>"`, page indices
    /// 0-based, e.g. `"0,2;C:/tmp/out.pdf"`. An unparseable or out-of-range
    /// payload writes nothing and reports on testExtractResultPipe_, so a
    /// test can distinguish "extraction failed" from "the pipe never ran" --
    /// a distinction a fire-and-forget pipe cannot make, and the reason the
    /// f7c2a4e8 review rejected a liveness-check oracle for this row.
    /// Result payload: `"ok|<pageCount>"` or `"err|<reason>"`.
    /// Gated by PDFE_TEST_PIPES; nullptr in production.
    class QLineEdit* testExtractPipe_ = nullptr;
    class QLineEdit* testExtractResultPipe_ = nullptr;

    /// GitHub #219: hidden pipe that drives TRE span activation directly.
    ///
    /// The measured break was never OCR -- `TEXT_EDIT_READY|9` reports
    /// correctly -- it is the click -> spanActivated hop, which does not
    /// complete under a synthetic click. TextEditTool already exposes
    /// selectSpanForTesting(), which calls the PRODUCTION selectSpan() and so
    /// emits the real spanActivated signal MainWindow listens for; nothing
    /// connected it to the Python side. This pipe does.
    ///
    /// Payload is either a bare span index (`"3"`) or a PDF-space point
    /// (`"<pdfX>,<pdfY>"`), in which case the handler resolves the span whose
    /// bounding box contains the point. Result on
    /// testSpanActivateResultPipe_: `"ok|<spanIndex>"` or `"err|<reason>"`.
    /// Gated by PDFE_TEST_PIPES; nullptr in production.
    class QLineEdit* testSpanActivatePipe_ = nullptr;
    class QLineEdit* testSpanActivateResultPipe_ = nullptr;

    /// GitHub #222: readback of the PagePanel's CURRENT page selection.
    ///
    /// The two SendInput gesture tests (Ctrl+click multi-select, Shift+click
    /// range-select) ended in `assert session.proc.poll() is None` -- a
    /// liveness check, which passes whether the gesture selected two pages,
    /// one page, or nothing at all. There was a pipe to SET a selection
    /// (testSelectPagesPipe) and none to READ one, so the tests had no oracle
    /// available even in principle; the matrix row's stated cause ("needs
    /// multi-page fixture") was doubly wrong, since tests/fixtures/
    /// test_multipage.pdf exists and has 5 pages.
    ///
    /// Write any non-empty token; the result pipe answers with the selected
    /// page indices as a comma-separated list ("0,2"), or "none" when the
    /// selection is empty -- distinguishable from "0", which is a real
    /// one-page selection. Gated by PDFE_TEST_PIPES; nullptr in production.
    class QLineEdit* testSelectedPagesQueryPipe_ = nullptr;
    class QLineEdit* testSelectedPagesResultPipe_ = nullptr;

    /// Pipeline a7c4f891 phase-5 (D1 / P-GAP-74): hidden query/result
    /// pipe pair for atlas readiness. Python tests write any non-empty
    /// token to the query pipe; the handler returns "1" if both
    /// scanCroppedAtlas_ and freeTypeStyledAtlas_ are populated (i.e.,
    /// the TRE atlas pipeline has completed and the AtlasComparisonDialog
    /// is ready to open or has already opened), "0" otherwise. Gated by
    /// PDFE_TEST_PIPES; nullptr in production.
    class QLineEdit* testAtlasReadyQueryPipe_ = nullptr;
    class QLineEdit* testAtlasReadyResultPipe_ = nullptr;

    /// Pipeline phase-7 (loop-until-clean): hidden query/result pipe pair
    /// reporting the zoom toolbar combo's existence + current text. The
    /// QComboBox lives in a QToolBar; on the Windows-Server-2019 CI session
    /// the Qt6 UIA bridge does not reliably expose a combo-in-toolbar by its
    /// AutomationId, so test_zoom_combo_uia_exposed cannot find it via UIA.
    /// This pipe is a genuine readback: any non-empty query token returns
    /// "<currentText>" if zoomCombo_ exists, or "__null__" if it is null.
    /// Gated by PDFE_TEST_PIPES; nullptr in production.
    class QLineEdit* testZoomComboQueryPipe_ = nullptr;
    class QLineEdit* testZoomComboResultPipe_ = nullptr;

    /// Pipeline phase-7 (loop-until-clean): hidden query/result pipe pair
    /// reporting the open document's page count. The page-operations tests
    /// read the count from the status-bar pageLabel via UIA, but Qt does not
    /// expose QLabel::text() through UIA and the accessibleName mirror is
    /// flaky to discover by suffix immediately after launch, so the read can
    /// return 0 while the document is in fact loaded. This pipe returns the
    /// authoritative document_->pageCount() ("0" if no document). Gated by
    /// PDFE_TEST_PIPES; nullptr in production.
    class QLineEdit* testPageCountQueryPipe_ = nullptr;
    class QLineEdit* testPageCountResultPipe_ = nullptr;

    /// GitHub #136 (CI-LIMITATION-PAGEPANEL-POPULATE-RACE): PagePanel
    /// readiness readback. Result is "<panelItems>,<docPages>". The panel
    /// has finished populating exactly when the two are equal and non-zero
    /// (PagePanel::generateThumbnails adds all rows synchronously). The
    /// page-operations tests previously polled the UIA tree for a
    /// hand-picked FLOOR of ListItems, which a half-built tree satisfies —
    /// this pipe replaces the floor with the real number. Gated by
    /// PDFE_TEST_PIPES; nullptr in production.
    class QLineEdit* testPagePanelReadyQueryPipe_ = nullptr;
    class QLineEdit* testPagePanelReadyResultPipe_ = nullptr;

    /// GitHub #136 (CI-LIMITATION-VISUAL-REGRESSION-SESSION-DRIFT): canvas
    /// rectangle readback for screenshot capture. Result is "x,y,w,h" giving
    /// the PdfCanvas widget's position and size **in MainWindow CLIENT-AREA
    /// DEVICE PIXELS** — i.e. the exact coordinate space `GetClientRect` /
    /// `PrintWindow` use, so a test can pass it straight to
    /// `capture_hwnd(hwnd, region=...)`.
    ///
    /// The visual tests used to crop a FRACTION of the window (x = 15% of
    /// width, w = 70%), which is the "never guess canvas coordinates"
    /// anti-pattern applied to a capture region: when the dock widths differ
    /// the fractional box slides off the canvas and the comparison measures
    /// dock furniture. Gated by PDFE_TEST_PIPES; nullptr in production.
    class QLineEdit* testCanvasRectQueryPipe_ = nullptr;
    class QLineEdit* testCanvasRectResultPipe_ = nullptr;

    /// GitHub #136: replace the per-user persisted dock layout with a fixed
    /// one so the canvas rectangle is identical on every run and profile.
    /// Called from the constructor only when
    /// `testseam::testDeterministicLayoutEnabled()`.
    void applyDeterministicTestLayout();

    /// Pipeline f3c91a2e phase-7 (GAP-A14): hidden query/result pipe pairs
    /// that give the LTV pywinauto tests a genuine PASS path on the
    /// Windows-Server-2019 CI session, where UIA cannot enumerate the
    /// SignaturePanel label text nor modal Qt dialogs. Python writes any
    /// non-empty token to the query pipe; the handler writes the asserted
    /// state into the result pipe (read back via UIA ValuePattern).
    ///  * LtvStatus      → SignaturePanel::ltvStatusText() ("__null__" if
    ///                     no panel/empty).
    ///  * ValidationDlg  → ValidationRequestDialog::lastTestIntrospection()
    ///                     (recorded when the modal dialog last constructed).
    ///  * TrustAnchorDlg → TrustAnchorDialog::lastTestIntrospection().
    /// All gated by PDFE_TEST_PIPES; nullptr in production.
    class QLineEdit* testLtvStatusQueryPipe_ = nullptr;
    class QLineEdit* testLtvStatusResultPipe_ = nullptr;
    class QLineEdit* testValidationDialogQueryPipe_ = nullptr;
    class QLineEdit* testValidationDialogResultPipe_ = nullptr;
    class QLineEdit* testTrustAnchorDialogQueryPipe_ = nullptr;
    class QLineEdit* testTrustAnchorDialogResultPipe_ = nullptr;

    /// Pipeline f3c91a2e phase-7 (GAP-A14): ONE generic query/result pipe
    /// pair for the reusable dialog-introspection seam (TestDialogSeam.h).
    /// Python writes a dialog objectName (e.g. "ComplianceDialog") to the
    /// query pipe; the handler writes that dialog's recorded child-widget
    /// inventory from pdfe::ui::testseam::dialogIntrospection() into the
    /// result pipe ("__null__" if the dialog has not been opened). Lets a
    /// pywinauto test assert a modal dialog opened with the expected
    /// widgets even though UIA cannot enumerate it on Server 2019. Gated by
    /// PDFE_TEST_PIPES; nullptr in production.
    class QLineEdit* testDialogIntrospectionQueryPipe_ = nullptr;
    class QLineEdit* testDialogIntrospectionResultPipe_ = nullptr;

    // Deferred atlas build state (populated at tool activation, rebuilt per line click)
    struct PendingAtlasState {
        std::vector<pdfe::tre::HocrLine> allLines;
        QImage pageImage;
        pdfe::tre::RgbColor bgColor{255, 255, 255};
        std::u32string pageText;
        std::string fontFilePath;
        std::string fontFamily;
        int fontFaceIndex = 0;
        float fontSizePt = 0.0f;
        pdfe::tre::RgbColor textColor{0, 0, 0};
        int pageIdx = -1;
        int scanDpi = 300;
        bool ready = false;      ///< True when data is populated
        int currentRefLine = -1; ///< HOCR line index of current atlas reference (-1 = none)

        // CNN deferred ranking: candidates collected at activation, ranked on span click
        std::vector<pdfe::fonts::SubstituteResult> cnnCandidates;
        pdfe::tre::ScannedTextProperties detectedProperties;
        /// Per-line font size (PDF pt) aligned to allLines — the lines a span click
        /// resolves against. 0.0 for a line with no valid measurement (the clicked
        /// line then falls back to fontSizePt). Computed via FontMatcher::lineFontSizePt
        /// so a heading line and a body line get their OWN size in the overlay/atlas
        /// instead of the single region average (pipeline c4a9e271 Phase 1). Recomputed
        /// over allLines rather than reusing detectedProperties.perLineFontSizePt because
        /// allLines comes from a separate char-box OCR pass and is not index-compatible.
        std::vector<float> perLineFontSizePt;
        bool cnnRanked = false;  ///< True after first visual ranking has been done

        // Full ranked candidate list from visualRankForLineWithCandidates (for dialog)
        std::vector<pdfe::tre::VisualFontRanker::RankedCandidate> visualRankedCandidates;

        // Clicked-line data for the comparison dialog
        QImage clickedLineImage;   ///< Cropped scan image of the clicked line
        QString clickedLineText;   ///< HOCR text of the clicked line
    };
    PendingAtlasState pendingAtlas_;

    // Scanned page image edit tracking (stack-based, synchronized to undo manager depth)
    // Saves/restores XObject refs (pdf_obj*) for lossless undo on image-dominated pages.
    // EditTextCommand only handles the content stream; the XObject modification from
    // refreshImageForScannedPage() is tracked here for undo/redo.
    struct ImageEditEntry {
        int undoDepth = 0;     // UndoManager::undoStackSize() when this edit occurred
        int pageIndex = -1;
        QRectF spanBbox;
        void* savedXObjectRef = nullptr;  // pdf_obj* of XObject before this edit
        std::string xobjectName;          // XObject key name (e.g. "Im0")
        // Redo parameters: stored so refreshImageForScannedPage() can be re-invoked
        std::u32string editedText;
        std::vector<pdfe::textedit::FormattingRun> formattingRuns;
        std::string fontName;
        float fontSize = 0.0f;
    };
    std::vector<ImageEditEntry> imageEditStack_;
    int imageEditPos_ = -1;           // Current position (-1 = no image edits tracked)
    uint32_t nextRegionId_ = 1;       // Incrementing RegionId for ImageReplacer
    static constexpr int kMaxImageEdits = 20;  // Memory bound for cached entries
    void releaseImageEditStack();      // Release all saved XObject refs

    // Redaction support
    std::unique_ptr<pdfe::redaction::RedactionManager> redactionManager_;

    // Encryption support
    std::unique_ptr<pdfe::encryption::EncryptionManager> encryptionManager_;

    // Signature support
    std::unique_ptr<pdfe::signatures::SignatureManager> signatureManager_;
    QDockWidget* signaturePanelDock_ = nullptr;
    SignaturePanel* signaturePanel_ = nullptr;

    // Image manipulation
    std::unique_ptr<pdfe::image::ImageManager> imageManager_;
    std::unique_ptr<pdfe::image::ExifStripper> exifStripper_;
    std::unique_ptr<pdfe::image::ImageContentEditor> imageContentEditor_;
    std::unique_ptr<pdfe::image::ImageTool> imageTool_;
    QDockWidget* imagePropertiesDock_ = nullptr;
    pdfe::image::ImagePropertiesPanel* imagePropertiesPanel_ = nullptr;

    // Image actions
    QAction* imageToolAction_ = nullptr;
    QAction* insertImageAction_ = nullptr;
    QAction* stripExifAction_ = nullptr;
    QAction* imageRemoveAction_ = nullptr;
    QAction* imageRotateAction_ = nullptr;
    QAction* imageCropAction_ = nullptr;
    QAction* imageResizeAction_ = nullptr;

    // Metadata
    std::unique_ptr<pdfe::metadata::MetadataManager> metadataManager_;
    QAction* documentMetadataAction_ = nullptr;

    // Compliance
    QAction* complianceAction_ = nullptr;
    QAction* ocrAction_ = nullptr;
    QAction* ocrAllPagesAction_ = nullptr;  // Phase 7d: OCR All Pages
    QAction* treAction_ = nullptr;
    QAction* accessibilityAction_ = nullptr;
    QAction* batchAction_ = nullptr;
    QAction* compareAction_ = nullptr;
    QAction* preflightAction_ = nullptr;

    // Phase-9 alignment persistence: coordinator that queues alignment
    // changes made via TextEditToolBar and flushes them through
    // AlignmentInjector at save time.
    std::unique_ptr<pdfe::reflow::AlignmentPipeline> alignmentPipeline_;

    // App settings and recent files
    std::unique_ptr<pdfe::app::AppSettings> appSettings_;

    // GitHub #127. Created lazily and ONLY when the opt-in check is enabled,
    // so a stock install never even constructs a network stack for this.
    std::unique_ptr<QNetworkAccessManager> updateCheckNam_;

    // Pipeline F phase-5: theme manager (Light / Dark / System)
    std::unique_ptr<pdfe::theme::ThemeManager> themeManager_;
    RecentFilesMenu* recentFilesMenu_ = nullptr;

    // Toolbar toggles
    QAction* toggleStickyNotesAction_ = nullptr;
    QAction* toggleCopyPreventionAction_ = nullptr;
    QAction* togglePrintPreventionAction_ = nullptr;
    bool copyPreventionActive_ = false;
    bool printPreventionActive_ = false;

    // Phase 4 (R2-09/R2-10) unified permission gates. Each combines the LIVE
    // document permission (Document::printAllowed / copyAllowed — the mandatory
    // floor the document's owner set) with the user-facing Prevent-Print /
    // Prevent-Copy toggle (additive). Every content-print / content-extract
    // entry point (menu, toolbar, Named /Print link, Copy, Select All, Export
    // Plain Text) consults these so no path bypasses a denied permission.
    bool documentDeniesPrint() const;
    bool documentDeniesCopy() const;
};

} // namespace pdfe::ui
