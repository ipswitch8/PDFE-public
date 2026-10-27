#pragma once

#include "PageInfo.h"
#include <QRectF>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Forward declarations for MuPDF C types (must be at global scope, with C linkage)
extern "C" {
struct fz_document;
}

namespace pdfe::textedit {
class ContentStreamEditor;
}

namespace pdfe::fonts {
class EmbeddedFontAnalyzer;
class PdfFontEmbedder;
class Type3FontBuilder;
}

namespace pdfe::tre {
class PdfContentWriter;
class SynthesizedType3FontBundler;
}

namespace pdfe::redaction {
class RedactionManager;
}

namespace pdfe::encryption {
class EncryptionManager;
}

namespace pdfe::signatures {
class SignatureManager;
class LtvDssWriter;
class DssReader; // Pipeline F phase-10
class DocMdpEnforcer; // meta-audit follow-up F2 (PDF-31/PDF-32)
}

namespace pdfe::image {
class ImageManager;
class ImageContentEditor;
class ExifStripper;
}

namespace pdfe::metadata {
class MetadataManager;
}

namespace pdfe::layers {
class LayerManager;
}

namespace pdfe::attachments {
class AttachmentManager;
}

namespace pdfe::forms {
class FormManager;
}

namespace pdfe::optimize {
class GrayscaleConverter;
}

namespace pdfe::compliance {
class XmpMetadataWriter;
class IccProfileManager;
class FontComplianceChecker;
class PdfAValidator;
class PdfAConverter;
class ProhibitedFeatureScanner;
class PdfUAConverter;
class StructureTreeBuilder;
class AutoTagger;
class ContentStreamTagger;
class ComplianceManager;
}

namespace pdfe::core {

// Annotation filter options for display list generation
enum class AnnotationFilter {
    ShowAll,
    HideStickyNotes
};

class MuPdfContext;
class Page;
class BookmarkExtractor;
struct BookmarkNode;

/// A PDF document version — the `x.y` of a `%PDF-x.y` header (ISO 32000-1
/// §7.5.2) or of a catalog `/Version` name (ISO 32000-1 §7.5.5).
///
/// meta-audit follow-up F14 (PDF-01 / PDF-07 / PDF-08): before F14, PDFE read
/// and wrote neither, so a silent downgrade of a PDF 2.0 file was unobservable.
struct PdfVersion {
    int major = 0;
    int minor = 0;

    [[nodiscard]] constexpr bool isValid() const noexcept {
        return major > 0 && minor >= 0;
    }
    /// Total-order key. `1.7` → 17, `2.0` → 20 — the same encoding MuPDF uses
    /// in `pdf_document::version`, so the two can be compared directly.
    [[nodiscard]] constexpr int key() const noexcept { return major * 10 + minor; }
    /// "1.7". Returns "" when !isValid().
    [[nodiscard]] std::string toString() const;

    /// Parse "1.7" / "2.0" (optionally preceded by "%PDF-"). Returns nullopt
    /// when the text is not a well-formed version.
    [[nodiscard]] static std::optional<PdfVersion> parse(std::string_view text);

    friend constexpr bool operator==(PdfVersion a, PdfVersion b) noexcept {
        return a.major == b.major && a.minor == b.minor;
    }
    friend constexpr bool operator!=(PdfVersion a, PdfVersion b) noexcept {
        return !(a == b);
    }
    friend constexpr bool operator<(PdfVersion a, PdfVersion b) noexcept {
        return a.key() < b.key();
    }
    friend constexpr bool operator>(PdfVersion a, PdfVersion b) noexcept { return b < a; }
    friend constexpr bool operator<=(PdfVersion a, PdfVersion b) noexcept { return !(b < a); }
    friend constexpr bool operator>=(PdfVersion a, PdfVersion b) noexcept { return !(a < b); }
};

class Document {
public:
    /// Prefix of the error a STAGED incremental save reports when the append
    /// completed but the atomic publish (the rename) could not be performed.
    ///
    /// WHY THIS IS A DECLARED CONSTANT AND NOT A LITERAL (DCA 0015, phase-10).
    /// It is the only externally visible thing that distinguishes a staged save
    /// failing at the COMMIT from an unstaged save failing at the OPEN, and that
    /// distinction is what makes "the failure path left no staging residue" a
    /// statement about a mechanism rather than about a mechanism's absence: an
    /// unstaged save has nothing to leave behind, so it satisfies the residue
    /// check for free. A test that needs to tell those apart must not do it by
    /// substring-matching prose that a later reword silently invalidates —
    /// that is DCA 0028's species. It names this declaration instead, so a
    /// reword is a compile-time-visible edit to a contract.
    ///
    /// Both publish-failure throws in saveIncrementalStaged() begin with it.
    static constexpr const char* kAtomicPublishFailedPrefix =
        "Failed to atomically publish the incremental save";

    ~Document();

    /// GitHub #240 — who asked for this path?
    ///
    /// Network locations are refused for DocumentRequested and permitted for
    /// UserInitiated. The distinction is the whole policy: a person choosing a
    /// file on their own file server is doing something ordinary, while a PDF
    /// naming a UNC resource is asking this process to touch a host it chose —
    /// the SMB/NetBIOS egress vector GitHub #91 (WIN-60) closed.
    ///
    /// DocumentRequested is the DEFAULT deliberately, so a new call site that
    /// says nothing keeps the refusal. Every existing caller therefore keeps its
    /// current behaviour without being touched, and a future caller has to state
    /// that a human asked for the path before it is allowed. The unsafe
    /// direction requires typing.
    enum class OpenOrigin {
        DocumentRequested,  ///< From PDF content, or unattributed. Refuses network paths.
        UserInitiated,      ///< A person named this path (File > Open, argv, drop, recents).
    };

    // Factory method to open a document
    static std::unique_ptr<Document> open(
        std::shared_ptr<MuPdfContext> context,
        const std::filesystem::path& filePath,
        const std::string& password = "",
        OpenOrigin origin = OpenOrigin::DocumentRequested
    );

    // Factory method: write a blank single-page PDF to the given
    // path, then open it and return the Document. Used by File > New.
    // @param widthPt    Page width in points (default 612.0 = Letter)
    // @param heightPt   Page height in points (default 792.0 = Letter)
    static std::unique_ptr<Document> createBlank(
        std::shared_ptr<MuPdfContext> context,
        const std::filesystem::path& outputPath,
        float widthPt = 612.0f,
        float heightPt = 792.0f
    );

    // Page access
    int pageCount() const;
    PageInfo pageInfo(int index) const;
    std::unique_ptr<Page> page(int index);

    // Document properties
    bool isModified() const;

    /// F62 (phase-55). The number of garbage bytes MuPDF found BEFORE this
    /// document's `%PDF-` marker — `pdf_document::bias`. Zero for every
    /// well-formed file, and zero for any document MuPDF had to REPAIR (repair
    /// re-normalises offsets and resets bias), so a non-zero value means "this
    /// document really is being read in the biased coordinate system".
    ///
    /// It is public because it is the only observable that distinguishes the
    /// code path saveIncrementalStaged() has to compensate for from the one it
    /// does not, and a fixture that cannot assert which path it took is a
    /// fixture that can silently stop probing the branch it was written for —
    /// which is exactly the state F62 found this branch in. Returns 0 for a
    /// non-PDF or closed document.
    int preambleBias() const;

    /// F62 (phase-55). True when MuPDF is holding a signature that has been
    /// PREPARED (pdf_sign_signature) but not yet written — `pdf_has_unsaved_sigs`.
    ///
    /// Public for the same reason as preambleBias(): saveIncrementalStaged()
    /// REFUSES the combination of a header preamble and an unsaved signature,
    /// and a refusal nothing can observe the precondition of is a refusal
    /// nothing can test. Note the state is short-lived in practice —
    /// SignatureManager::signField() creates it and consumes it inside one
    /// call — so a test that does not assert this is true has probably not
    /// reached the refusal at all. Returns false for a non-PDF or closed
    /// document.
    bool hasUnsavedSignatures() const;

    /// R3-04 autosave (Phase-2 capture-coverage). A monotonically increasing
    /// counter bumped on EVERY content-mutating operation — including direct
    /// mutators that push no undo command (redaction burn-in, page-label
    /// rewrite, direct layer ops, …). AutosaveManager compares it against the
    /// value at the last autosave so a non-undo edit still triggers a recovery
    /// snapshot. Starts at 0; the save/writeRecoveryCopy paths that clear the
    /// dirty flag do NOT bump it (they clear state, they are not edits).
    [[nodiscard]] uint64_t contentVersion() const;

    /// Mark the document content as changed from an EXTERNAL mutator that does
    /// not otherwise flip Document's dirty bookkeeping. Sets modified and bumps
    /// contentVersion() so autosave sees the edit.
    void markModified();

    bool isPdf() const;
    const std::filesystem::path& filePath() const;
    void setFilePath(const std::filesystem::path& path);

    // -----------------------------------------------------------------
    // PDF version handling (meta-audit follow-up F14 — PDF-01/07/08).
    // -----------------------------------------------------------------

    /// The version in the file's `%PDF-x.y` header line, read from the raw
    /// bytes at open time (ISO 32000-1 §7.5.2).
    ///
    /// This is deliberately NOT `pdf_document::version`: MuPDF overwrites that
    /// field with the catalog `/Version` whenever one is present, so by the
    /// time the document is open the authored header value is no longer
    /// recoverable from MuPDF. Returns an invalid PdfVersion for documents
    /// with no readable header (non-PDF, or built in memory).
    [[nodiscard]] PdfVersion headerVersion() const;

    /// The catalog `/Root/Version` name, or nullopt when the key is absent or
    /// malformed (ISO 32000-1 §7.5.5).
    [[nodiscard]] std::optional<PdfVersion> catalogVersion() const;

    /// The EFFECTIVE document version.
    ///
    /// ISO 32000-1 §7.5.5 / ISO 32000-2 §7.5.5: the catalog `/Version`
    /// overrides the header **only if it is later** than the header version.
    /// The override is one-way — it may raise the version, never lower it.
    ///
    /// NOTE this differs from raw MuPDF, which applies the catalog value
    /// unconditionally and therefore reports 1.4 for a 1.7-header document
    /// carrying `/Version /1.4`. `tests/fixtures/version_catalog_older_1_4.pdf`
    /// pins that difference.
    [[nodiscard]] PdfVersion pdfVersion() const;

    /// Raise the effective document version to at least @p v.
    ///
    /// Writes the header floor used by the full-rewrite save paths and, when
    /// @p v exceeds what the header alone can express for this document, also
    /// writes the catalog `/Version`. Never lowers the version: a call with a
    /// version at or below pdfVersion() is a no-op. Throws PdfeException for a
    /// non-PDF document or an invalid version.
    void setMinimumPdfVersion(PdfVersion v);

    /// #46: adopt ownership of a scratch/bake temp file this Document was
    /// reopened onto and now holds open (e.g. a redaction "<path>.redact_tmp"
    /// produced by save() to a throwaway path). The file is deleted when the
    /// Document releases its handle — at destruction, or when a later reopen
    /// registers a different scratch. Lets a caller that cannot own the
    /// Document's lifecycle (RedactionManager) avoid orphaning the temp it
    /// created. The temp holds post-redaction (scrubbed) content — disk hygiene,
    /// not a confidentiality concern. const: mutates only pImpl bookkeeping.
    ///
    /// NO PRODUCT CALLER REMAINS AS OF WIN-87 PHASE-57, AND THAT IS RECORDED
    /// HERE RATHER THAN ACTED ON. Its only caller was
    /// RedactionManager::applyAllRedactions, which adopted the
    /// "<doc>.redact_tmp" bake temp that save()-to-a-throwaway-path left the
    /// live document reopened onto. saveThroughExternalRewrite() removed both
    /// the throwaway path and the reopen-onto-it, so there is nothing left to
    /// adopt: the bake now lives and dies behind a StagingFile whose destructor
    /// removes it on every path. Kept, not deleted, because the mechanism is
    /// the #46 fix itself (its cleanup runs in ~Impl AFTER fz_drop_document,
    /// which is the ordering that made it work) and a future caller that
    /// legitimately reopens a Document onto a scratch file would otherwise
    /// re-derive it wrongly. If nothing has claimed it by the time WIN-87's
    /// last site is migrated, delete it then — with this note as the reason it
    /// was safe to.
    void registerScratchTempForCleanup(const std::filesystem::path& path) const;
    std::shared_ptr<MuPdfContext> context() const;

    // --- Security / permissions (Phase 4: R2-09/10/11/12) ---

    /// Authentication level established when this document was opened, derived
    /// from the ACTUAL fz_authenticate_password result — NOT inferred from the
    /// permission bitmask (R2-12). Values: 0 = none/failed, 1 = user, 2 = owner,
    /// 3 = both (user==owner password). Owner authority is (authLevel() & 2).
    /// An unencrypted or non-PDF document reports 2 (owner-equivalent: it carries
    /// no access restrictions to enforce). The value is captured at open with the
    /// password actually supplied and re-captured after a save+reopen.
    int authLevel() const noexcept;

    /// True iff owner-level authority is established (owner or both). Gate for
    /// privileged security operations such as stripping encryption (R2-11).
    bool isOwnerAuthenticated() const noexcept { return (authLevel() & 2) != 0; }

    /// Live "print allowed" permission for THIS document. Reads the document's
    /// actual permission bits (pdf_document_permissions). An unencrypted / non-PDF
    /// document allows printing. Enforcement boundary for R2-09.
    bool printAllowed() const;

    /// Live "copy / content-extraction allowed" permission for THIS document.
    /// An unencrypted / non-PDF document allows extraction. Enforcement boundary
    /// for R2-10 (Copy, Select All, Export Plain Text, batch text extract).
    bool copyAllowed() const;

    // --- The remaining five of the eight standard permission bits (PDF-23) ---
    //
    // ISO 32000-1 Table 22 defines eight user-access permissions. Before the
    // meta-audit follow-up only print (bit 3) and copy (bit 5) had accessors,
    // so a document whose owner denied modification, annotation, form fill,
    // accessibility extraction, assembly, or high-resolution printing looked
    // protected and was not: no code path could even ask. Every accessor below
    // follows the same contract as printAllowed()/copyAllowed() — an
    // unencrypted or non-PDF document carries no access restrictions and
    // therefore grants the permission.

    /// Live "document modification allowed" permission (bit 4, PDF_PERM_MODIFY).
    /// Covers content edits other than annotation/form/assembly changes.
    bool modifyAllowed() const;

    /// Live "annotation and form-field modification allowed" permission
    /// (bit 6, PDF_PERM_ANNOTATE).
    bool annotateAllowed() const;

    /// Live "fill in existing form fields allowed" permission
    /// (bit 9, PDF_PERM_FORM). Granted independently of annotateAllowed().
    bool fillFormsAllowed() const;

    /// Live "extract text and graphics for accessibility allowed" permission
    /// (bit 10, PDF_PERM_ACCESSIBILITY). Deprecated in PDF 2.0 (always granted
    /// there) but still authoritative for R2..R4 documents.
    bool extractForAccessibilityAllowed() const;

    /// Live "document assembly allowed" permission (bit 11, PDF_PERM_ASSEMBLE)
    /// — insert, rotate, delete pages and create bookmarks/thumbnails.
    bool assembleAllowed() const;

    /// Live "high-resolution printing allowed" permission
    /// (bit 12, PDF_PERM_PRINT_HQ). A document may permit degraded printing
    /// (printAllowed()) while denying faithful high-resolution output.
    bool printHighResAllowed() const;

    // Bookmarks
    std::vector<std::shared_ptr<BookmarkNode>> extractBookmarks();

    // Page manipulation
    void deletePage(int index);

    /// R3-01 (destination integrity). Rebuild the page tree to contain ONLY
    /// the pages named in @p keptOriginalIndicesInNewOrder, in that order, and
    /// remap every internal destination accordingly. @p keptOriginalIndicesInNewOrder
    /// holds ORIGINAL (current) 0-based page indices — pages omitted from the
    /// list are dropped. Routed to MuPDF's native pdf_rearrange_pages, which:
    ///   * validates / remaps /Outlines (bookmark) destinations, dropping an
    ///     outline item whose target page is gone (or clearing just its dest if
    ///     it still has children),
    ///   * rebuilds the /Names -> /Dests named-destination tree keeping only
    ///     dests to retained pages,
    ///   * strips /Link annotations whose GoTo destination points nowhere, and
    ///   * (PDF_CLEAN_STRUCTURE_KEEP) preserves /StructTreeRoot and rebuilds
    ///     /ParentTree with remapped /StructParents.
    /// This is the single choke-point that keeps bookmarks, links and named
    /// destinations correct across a page delete or reorder. Catalog entries
    /// that pdf_rearrange_pages does not itself carry over (/AcroForm,
    /// /Metadata, /Lang, /MarkInfo, /ViewerPreferences, /PageLabels, ...) are
    /// preserved by this method so a page edit never silently drops the form or
    /// document metadata.
    void rearrangePages(const std::vector<int>& keptOriginalIndicesInNewOrder);

    /// R3-01 convenience: reorder ALL pages (no deletion). @p newOrder must be a
    /// permutation of 0..pageCount()-1; page at position k becomes the page
    /// whose current index is newOrder[k]. Destinations are remapped exactly as
    /// in rearrangePages().
    void reorderPages(const std::vector<int>& newOrder);

    void insertBlankPage(int atIndex, float widthPt, float heightPt, int rotation = 0);
    void insertPageObj(int atIndex, void* pageObj);
    void rotatePage(int index, int angleDelta);
    int currentRotation(int index) const;

    // Cross-document page operations
    void graftPage(int atIndex, const Document& source, int sourcePageIndex);
    void graftPages(int atIndex, const Document& source, int sourceStart, int sourceEnd);

    /// F6 / PDF-89: graftPages() carries only what `pdf_graft_mapped_page`
    /// copies -- content, resources, the boxes and /Rotate. This variant also
    /// carries the source's annotations, form fields AND their values, outline,
    /// page labels, optional-content groups, embedded files, document
    /// JavaScript and named destinations, pruning or remapping anything that
    /// referenced a page outside [sourceStart, sourceEnd]. Backs Combine Files.
    void graftPagesPreservingArtifacts(int atIndex, const Document& source,
                                       int sourceStart, int sourceEnd);

    /// Replace a page at the given index with a page from another document.
    /// Equivalent to deletePage(atIndex) + graftPage(atIndex, source, sourcePageIndex)
    /// as a single atomic operation.
    void replacePage(int atIndex, const Document& source, int sourcePageIndex);

    // Page object retention for undo (opaque pdf_obj*)
    void* retainPageObj(int index);
    void releasePageObj(void* obj);

    // Document-INDEPENDENT release of a handle from retainPageObj(). The retained
    // holder is a standalone document living in the shared MuPdfContext (see
    // retainPageObj), so it can — and MUST — be released without a live Document.
    // Undo-command destructors call this instead of releasePageObj() because the
    // owning Document may already be gone at teardown (e.g. MainWindow frees
    // document_ before ~QObject deletes the child UndoManager). Routing release
    // through a raw Document* in that window is a use-after-free (defect B2).
    static void releaseRetainedPage(void* obj);

    // Thread-safe display list creation for background rendering.
    // Returns opaque fz_display_list* that captures all rendering commands.
    // Document access is serialized internally; the returned display list
    // can be rendered from any thread without further document access.
    // Caller must free with releaseDisplayList().
    void* createDisplayList(int pageIndex);
    void releaseDisplayList(void* displayList);

    // Annotation filtering for display lists
    void setAnnotationFilter(AnnotationFilter filter);
    AnnotationFilter annotationFilter() const;

    /// Announce that a page's content stream was modified in memory.
    ///
    /// ISSUE #125. Until 2026-09-01 this only inserted into an internal set
    /// that nothing consulted — a false affordance. ~14 call sites (every
    /// ImageCommands path, GrayscaleConverter, PageOperations, TextEditCommands,
    /// MainWindow) dutifully called it believing they had signalled the UI, while
    /// the only thing that actually stops a stale canvas was a hand-written
    /// PdfCanvas::refreshPage() at each MainWindow call site. That class of bug
    /// has escaped four times (docs/audit/seed-catalog-git.md G-02/G-03/G-10/G-11,
    /// and G-03 is literally "tile cache not invalidated -> content edits not
    /// visually shown").
    ///
    /// It now NOTIFIES: every listener registered with
    /// addPageDirtyListener() is called with @p pageIndex. Callers get correct
    /// repaint behaviour by doing the obvious thing.
    void markPageDirty(int pageIndex);

    /// Called with the index of a page whose content stream just changed.
    using PageDirtyListener = std::function<void(int pageIndex)>;

    /// Opaque handle returned by addPageDirtyListener(). Never 0 for a live
    /// registration, so 0 is usable as "not registered".
    using PageDirtyToken = std::uint64_t;

    /// Register a page-dirty listener. Returns a token for removal.
    ///
    /// DELIBERATELY NOT setDocumentEventCallback(). That is a SINGLE-slot
    /// std::function<void(const std::string&)> already claimed by MainWindow for
    /// the JavaScript engine bridge, and it carries no page index. Reusing it
    /// would clobber the JS subscription and could not say which page changed.
    ///
    /// Listeners are invoked with docMutex RELEASED, so a listener may call back
    /// into this Document without deadlocking. A listener that throws is
    /// swallowed: a repaint hook must not turn an edit into a failed edit.
    [[nodiscard]] PageDirtyToken addPageDirtyListener(PageDirtyListener cb);

    /// Remove a previously registered listener. Unknown or already-removed
    /// tokens are ignored, so teardown paths can call this unconditionally.
    ///
    /// CALLING THIS ON A DESTROYED Document IS STILL A USE-AFTER-FREE — being
    /// tolerant of a bad TOKEN says nothing about a bad `this`. Observers that
    /// keep a raw Document* must check lifetimeSentinel() first; see PdfCanvas.
    void removePageDirtyListener(PageDirtyToken token);

    /// A sentinel whose lifetime is exactly this Document's.
    ///
    /// Several observers in this codebase hold a raw `Document*` (PdfCanvas,
    /// the annotation tools) and cannot tell a live document from a freed one.
    /// Destruction order is genuinely not guaranteed in either direction:
    /// MainWindow destroys the Document while the canvas still points at it,
    /// and test fixtures call `canvas->setDocument(nullptr)` in TearDown after
    /// the document local has already gone. Both were real crashes
    /// (0xC0000005 in TearDown) before this existed.
    ///
    /// Hold the returned pointer WEAKLY. A non-expired weak_ptr means the
    /// Document is still alive and may be called; an expired one means the raw
    /// pointer beside it is dangling and must not be dereferenced. Holding it
    /// strongly would keep the sentinel alive but NOT the Document, so an
    /// expired-check would then never fire — which is worse than not checking.
    [[nodiscard]] std::shared_ptr<const void> lifetimeSentinel() const;

    // ---- F24: the certification (DocMDP) save guard seam ------------------
    //
    // PDFE writes /Perms /DocMDP when a signature certifies a document, and
    // since F2 it can READ it back and say correctly whether a post-
    // certification change is disallowed at the declared /P level
    // (pdfe::signatures::DocMdpEnforcer). Until F24 nothing consulted that
    // answer: a P1 ("no changes permitted") certified document could be edited
    // and saved with no complaint, which made the enforcer detection with no
    // consumer.
    //
    // The consumer is here, at the save path, because saving is the moment the
    // violation becomes a FILE. It is a seam rather than a direct call because
    // pdfe_signatures depends on pdfe_core and not the other way round;
    // pdfe::signatures::installDocMdpSaveGuard() is the production installer
    // (called from MainWindow's constructor) and the only thing that ever
    // supplies the callback.
    struct SaveGuardVerdict {
        /// False => the save must not proceed. Document::save/saveAs/
        /// saveIncremental throw PdfeException carrying `message`.
        bool allow = true;
        /// Non-empty => something noteworthy was found, whether or not the save
        /// is allowed. Recorded in lastSaveGuardWarning().
        std::string message;
    };

    /// @param doc      the document about to be written
    /// @param target   the path being written to
    /// @param inPlace  true when @p target is the file the document was opened
    ///                 from, i.e. the certified original is being overwritten
    using SaveGuard = std::function<SaveGuardVerdict(
        const Document* doc,
        const std::filesystem::path& target,
        bool inPlace)>;

    /// Install (or, with an empty std::function, remove) the process-wide save
    /// guard. Process-wide rather than per-Document deliberately: a guard that
    /// each open site must remember to attach is a guard that one forgotten
    /// open site disables.
    static void setSaveGuard(SaveGuard guard);
    static bool hasSaveGuard();

    /// The message from the most recent guard verdict on THIS document, empty
    /// when the last save raised nothing. Non-empty after an allowed-with-
    /// warning save; also set (before the throw) on a refused save.
    [[nodiscard]] const std::string& lastSaveGuardWarning() const;

    // Save operations
    void save(const std::filesystem::path& path);

    void saveAs(const std::filesystem::path& path);

    // -----------------------------------------------------------------------
    // WIN-87 site #3, phase-57 — a full rewrite handed to an EXTERNAL writer
    // without either end ever existing under a name an attacker can guess.
    // -----------------------------------------------------------------------
    //
    // WHAT IT DOES. Full-rewrites the current in-memory state into a staging
    // file, hands those staged bytes to @p rewrite for an external transform
    // (QPDF's single-revision rewrite is the only caller today), and publishes
    // the TRANSFORM'S output over filePath().
    //
    // WHY IT IS A Document METHOD AND NOT THREE LINES IN THE CALLER. The
    // hazard this closes exists precisely BECAUSE the lifecycle leaked into a
    // caller. RedactionManager used to do `document->save("<doc>.redact_tmp")`
    // and then reopen that name with QPDF: two by-path hops through a name
    // derived from the user's own document path, with nothing holding it in
    // between. Stamping attacker bytes into that intermediate got them parsed
    // by QPDF and republished as the document the user believed had content
    // removed from it — window A of WIN-87, and the making-the-final-rename-
    // atomic fix did not touch it, because that rename faithfully publishes
    // whatever the external writer produced.
    //
    // WHY IT CANNOT RECUR HERE. Neither artifact is ever published under a
    // predictable name, and neither is ever re-resolved by name at all:
    //
    //   * the bake goes into StagingFile::createBeside(filePath()) — a 128-bit
    //     CSPRNG name, CREATE_NEW, exclusive handle (FILE_SHARE_READ) held from
    //     creation to destruction — and is NEVER published. Its destructor
    //     removes it on every path, throwing ones included.
    //   * @p rewrite reads it through a DUPLICATE of that still-held handle,
    //     so there is no name lookup and no instant at which the handle is
    //     down.
    //   * the transform's output goes into a SECOND createBeside staging file
    //     and is published by renaming THROUGH its exclusive handle.
    //
    //   createBeside, NOT createFullRewriteTempFor: the latter deliberately
    //   keeps the predictable "<target>.pdfe_tmp" name because that name is the
    //   R2-04 crash-recovery contract. Neither artifact here is recoverable
    //   work — the bake is an intermediate the user never asked for and the
    //   output is superseded the moment it publishes — so offering either as
    //   "your unsaved work" would be a lie, and paying the predictable name for
    //   it would be paying for nothing.
    //
    // CONTRACT FOR @p rewrite.
    //   * It is called with `in` positioned at 0 over the staged bake and `out`
    //     positioned at 0 over the staged output. IT TAKES OWNERSHIP OF BOTH,
    //     on every path INCLUDING a throwing one, and must fclose both before
    //     returning or propagating. This is not tidiness: a handle THIS process
    //     still holds blocks the replace-existing rename that publishes the
    //     result, so a leaked stream turns a working publish into a diagnosable
    //     failure. (It is a failure, never a security hole — but it is a bug.)
    //   * It runs OUTSIDE any fz_try region, so ordinary C++ objects and
    //     exceptions are safe in it. Do not move it inside one.
    //   * An exception from it aborts the publish. filePath() is then still
    //     byte-intact, the Document is reopened onto it rather than left as a
    //     dead null handle, and the exception surfaces to the caller.
    //
    // PRECONDITION FOR THE PUBLISH — NO LIVE Page MAY BE HELD ACROSS THIS
    // CALL. A Page holds an fz_keep_document reference (MuPDF
    // source/fitz/document.c, fz_load_page), so the fz_drop_document inside
    // does not actually close the file, and the rename-over then fails with
    // "Access is denied". Document cannot drain Pages it does not own, so this
    // is a caller obligation:
    //
    //   * `core/SaveWithRebind.h`'s `drainRebindAroundWrite` is the general
    //     mechanism — snapshot each command's page index, release every Page
    //     holder, run the write, re-resolve and rebind. Use it when an undo
    //     stack holds raw Page* refs.
    //   * `MainWindow::onApplyRedactions` is the worked example (P11-BUG-1):
    //     clear the active tool, drain the tile renderer, invalidate the tile
    //     cache, drop the held pages, then clear undo.
    //
    // THIS PRECONDITION IS INHERITED, NOT INVENTED — `Document::save()`'s
    // in-place full rewrite has always had it, which is why that sequence
    // exists at all.
    //
    // AND VIOLATING IT IS SAFE, WHICH IS THE POINT OF THE FAILURE BRANCH.
    // The publish fails with a diagnosable error that names this cause, the
    // original is byte-intact — and the Document is reopened onto the STAGED
    // BAKE rather than onto the original, so the transform it just performed
    // is NOT silently discarded. That distinction is load-bearing when the
    // transform is a redaction: reopening the original would hand the caller
    // back a document whose confidential content had returned, and a
    // subsequent saveAs() would write it out. (Measured: that is exactly how
    // `Jbig2RedactionTest.OriginalJbig2BytesAreNotRecoverable` failed against
    // a first draft of this method.) The staging object is retained with its
    // exclusive handle open, and the reopen goes through a DUPLICATE OF THAT
    // HANDLE (`openDuplicateStream` -> `fz_open_file_ptr_no_close` ->
    // `fz_open_document_with_stream`), never through the staging file's name.
    // Two reasons, and the second is not a preference: a by-name reopen would
    // re-create the substitution window this method exists to remove, and it
    // also CANNOT SUCCEED — the StagingFile holds its handle with
    // `FILE_SHARE_READ` only, while MuPDF's by-name open goes through the CRT,
    // which requests share read AND write. Measured: the by-name form failed
    // every time and silently dropped through to the original, i.e. straight
    // back into the confidentiality defect above.
    //
    // If the duplicate cannot be taken, or the reopen from it also fails, the
    // Document is left CLOSED rather than reopened onto the original. Both call
    // sites throw immediately after, so the failure is loud; what the caller
    // cannot do is silently save unredacted bytes.
    //
    // THE GUARD IS CONSULTED FOR THE REAL TARGET. runSaveGuard(filePath()),
    // not for a scratch name. The old shape asked the F24 certification guard
    // about "<doc>.redact_tmp", so a guard that would refuse to rewrite a
    // certified document was answering a question about a file nobody cared
    // about. Fires "willSave"/"didSave" exactly as save() does.
    //
    // Throws PdfeException on any failure; the original is untouched in every
    // such case.
    void saveThroughExternalRewrite(
        const std::function<void(std::FILE* in, std::FILE* out)>& rewrite);

    /// The same closed publish lifecycle, but writing to a DIFFERENT path and
    /// leaving this Document alone — WIN-87 site #8/#9, phase-59.
    ///
    /// `saveThroughExternalRewrite` above republishes over `filePath()` and
    /// therefore has to drop and reopen the live handle. A converter writes to
    /// a path the user chose, with the source document still open, so none of
    /// that applies: nothing here drops `pImpl->doc`, nothing reopens, and
    /// `filePath()`/`incrementalSafe_` are untouched. Mixing the two would
    /// mean a saveAs silently repointing the Document at its own output.
    ///
    /// WHAT IT CLOSES. The compliance converters ran the pre-phase-57 three-step
    /// by-path relay: MuPDF wrote "<out>.pdfe_pdfa_tmp" BY NAME, nothing held
    /// it, QPDF REOPENED IT BY NAME, and then — worse than any WIN-87 site —
    /// `QPDFWriter` wrote DIRECTLY over the user's output path, so there was no
    /// atomic publish at all and a failure mid-write left the output truncated.
    /// That is the R2-04 data-loss shape, not just the substitution one.
    ///
    /// The document is baked into a StagingFile through a duplicate of its own
    /// exclusive handle, the callback gets duplicate streams, and only the
    /// OUTPUT staging file is published — by renaming through its handle. There
    /// is no instant at which either artifact has a guessable name, and none at
    /// which either is unheld.
    ///
    /// @param target the file to publish over. It is never removed first: on
    ///        any failure it is byte-intact, and both staging files are removed
    ///        by their destructors.
    void saveAsThroughExternalRewrite(
        const std::filesystem::path& target,
        const std::function<void(std::FILE* in, std::FILE* out)>& rewrite);

    /// R3-04 autosave / crash-recovery. Write the CURRENT in-memory document
    /// (including unsaved edits) as a standalone, self-contained PDF to @p path
    /// — a "recovery sidecar" copy — WITHOUT disturbing the live document's
    /// save state. Unlike saveAs(), this does NOT change filePath(), does NOT
    /// touch the incremental-save / full-rewrite / needs-full-GC session flags,
    /// and does NOT reopen the handle: the live document is byte-for-byte the
    /// same editing session afterwards, so a subsequent ordinary File>Save is
    /// unaffected (an autosave in the background must never silently downgrade
    /// the next real save's routing).
    ///
    /// The target is always a DISTINCT file from the source, so — unlike the
    /// in-place save() path — there is no original to remove and thus no
    /// Windows file-lock hazard; the current fz_document handle stays open.
    ///
    /// Throws PdfeException if the document is not a PDF or MuPDF rejects the
    /// write.
    void writeRecoveryCopy(const std::filesystem::path& path) const;

    /// Incremental save — append new xref / updated objects to the file
    /// the document was opened from (filePath()). The original bytes
    /// are preserved as a prefix, so a digital signature's signed
    /// ByteRange remains verifiable after annotation edits that fit in
    /// the incremental-save model.
    ///
    /// Throws if the document wasn't opened from a file path (e.g.
    /// built in memory), or if MuPDF rejects the save.
    ///
    /// UNSTAGED — writes onto the user's only copy (F58-c, phase-53). This hands
    /// filePath() straight to pdf_save_document(do_incremental=1), which opens it
    /// in APPEND mode, so a mid-write failure leaves the user's document carrying
    /// a partial revision: a broken trailing xref, and for a signed document a
    /// /ByteRange that no longer resolves. Nothing in PDFE routes here any more —
    /// Document::save()'s incremental branch and LtvDssWriter both go through
    /// saveIncrementalStaged(), on measurements recorded at that branch — and new
    /// callers should not. It is kept because it is the exact behaviour
    /// tests/test_dss_mid_append_kill.cpp's revert-and-observe control needs to
    /// compare against; a staged save is only demonstrably better than an
    /// unstaged one while the unstaged one still exists to be measured.
    void saveIncremental();

    /// STAGED incremental save — the crash-safe form of saveIncremental()
    /// (meta-audit follow-up F58, phase-52).
    ///
    /// WHY THIS EXISTS. saveIncremental() hands filePath() straight to
    /// pdf_save_document(do_incremental=1), which opens it in APPEND mode and
    /// writes onto the user's only copy. A genuine mid-write failure — disk
    /// full, media pulled, power loss, an I/O error — therefore leaves the
    /// user's document with a partial appended revision: a broken trailing
    /// xref, and for a signed document a /ByteRange that no longer resolves.
    /// DssRollback cannot help, because it undoes in-memory OBJECT mutations,
    /// not bytes already committed to disk.
    ///
    /// WHICH OTHER WRITES STAGE, STATED ACCURATELY (F58-e, phase-53). The
    /// phase-52 text here claimed "Every other durable write in PDFE
    /// (Document::save, saveAs-over-existing, redaction, batch, image export)
    /// already stages". That was FALSE AS WRITTEN and named the wrong function:
    /// Document::save() stages only on its FULL-REWRITE branch. Its incremental
    /// branch — the ordinary File>Save route for additive edits, i.e. form fills
    /// and annotations on signed documents — routes to the UNSTAGED
    /// saveIncremental(). Accurately:
    ///   * stage-and-atomically-replace: Document::save (full-rewrite branch),
    ///     saveAs over an existing file, redaction apply, batch, image export,
    ///     and this method;
    ///   * direct in-place append, no staging: Document::saveIncremental(), and
    ///     Document::save()'s incremental branch which calls it.
    /// See the note on saveIncremental() for the measurement behind that split.
    ///
    /// AND "STAGES" IS NOT ONE PROPERTY — WIN-87, phase-56. The list above says
    /// which writes use a staging file; it does not say which ones HOLD IT. Of
    /// the six, three now do: this method, Document::save()'s full-rewrite
    /// branch, and Document::saveAs() (which also stages a destination that
    /// does not exist yet, not only an overwrite). Redaction apply, batch and
    /// image export still resolve their staging artifact BY NAME between the
    /// write and the rename, which is the WIN-87 window — a staging file that
    /// nothing holds is a predictable, writable sibling, and phase-52
    /// demonstrated bytes stamped into one being atomically published over the
    /// user's signed document. Those three are phases 4 and 5. Reading this
    /// list as "already safe" is exactly the mistake the F58-e correction above
    /// records for the phase-52 version of it.
    ///
    /// WHAT IT DOES. Byte-copies filePath() into a freshly created staging file
    /// beside it (same directory, therefore same volume, therefore the rename
    /// really is atomic), appends the incremental revision to THAT copy, flushes
    /// it, releases the MuPDF read handle, and publishes it over filePath(). The
    /// copy-then-append shape is what preserves the property incremental saves
    /// exist for: the original bytes remain a byte-identical PREFIX of the
    /// result, so existing signatures keep verifying. A full rewrite would not.
    ///
    /// THE STAGING FILE IS NOT A PREDICTABLE, UNGUARDED SIBLING (F58-a,
    /// phase-53). It is core::StagingFile: "<filePath>.pdfe_part.<128-bit
    /// CSPRNG hex>", created CREATE_NEW with an exclusive handle held from
    /// creation to publication, written through that handle, and published
    /// through it. Phase-52's "<filePath>.pdfe_tmp" — predictable, CopyFileW'd,
    /// with no handle held across the write→rename gap — was demonstrably
    /// attackable by any process with write access to the containing directory,
    /// which is a LOWER bar than writing the document itself. See
    /// core/AtomicFileReplace.h for the mechanism and the exploit it closes.
    ///
    /// GUARANTEES.
    ///  * On ANY failure the file at filePath() is byte-identical to its
    ///    pre-call state — the staged bytes are the only casualty — and the
    ///    staging file is removed.
    ///  * On success filePath() is the original bytes plus the appended
    ///    revision, published in one atomic directory operation, with the DACL
    ///    it had before the call (F58-b: publication never widens permissions).
    ///
    /// @param reloadedFromDisk optional out-parameter, set to true once the
    ///        in-memory fz_document has been dropped and re-opened from disk.
    ///        Windows cannot rename over a file MuPDF holds open (see
    ///        Document::save step 2 and saveAs's third exemption), so the
    ///        commit necessarily drops the handle. Once that has happened ALL
    ///        un-saved in-memory mutation is gone, which callers holding an
    ///        undo/rollback snapshot must know: rolling back against the
    ///        re-opened document would delete objects that legitimately belong
    ///        to it. It is set BEFORE the rename is attempted, so it is true on
    ///        the rename-failure path as well — on that path the document was
    ///        re-opened from the UNCHANGED original, so the mutation is
    ///        discarded rather than rolled back, which is the same outcome.
    ///
    /// @param publishedButReopenFailed optional out-parameter (F58-f,
    ///        phase-53). Set to true in exactly one situation: the revision WAS
    ///        published — the document on disk is correct and complete — but the
    ///        re-open afterwards failed, which on Windows routinely means an A/V
    ///        scanner or the search indexer is momentarily holding the
    ///        just-renamed file. This method still throws in that case (the
    ///        Document is no longer usable and the caller must know), but a
    ///        caller that reports outcomes to a user MUST distinguish it: before
    ///        this flag existed, LtvDssWriter::write() reported a plain failure
    ///        for a /DSS upgrade that had in fact succeeded.
    ///
    /// Any fz_page/Page objects obtained from this Document before the call are
    /// invalidated by that re-open, exactly as they are by Document::save().
    /// If the re-open fails, pageCount() reports 0 and page()/pageInfo() throw —
    /// they do NOT dereference the dropped handle (F58-f).
    ///
    /// Throws PdfeException on any failure.
    void saveIncrementalStaged(bool* reloadedFromDisk = nullptr,
                               bool* publishedButReopenFailed = nullptr);

    /// Tier 1 incremental-save routing (pipeline b2e47d91).
    ///
    /// Returns true once the current editing session has performed any
    /// destructive or structural operation whose result must NOT be
    /// persisted via the incremental-append path. Incremental save never
    /// garbage-collects, so removed/replaced object bytes remain physically
    /// in the file — a data-confidentiality leak for redaction apply, EXIF
    /// strip, and image/page removal, and a structural hazard for page
    /// reorder/insert. Such sessions must go through the full-rewrite path
    /// (do_garbage=3), which scrubs the orphaned bytes.
    ///
    /// Pure additive edits (form-field fills, annotation add/modify) leave
    /// this false, so save() can route them through the fast saveIncremental
    /// path — the large-XFA-form performance win this method exists for.
    ///
    /// The flag is sticky for the session and is cleared only by a
    /// successful full-rewrite save (which bakes a clean, GC'd file).
    [[nodiscard]] bool sessionRequiresFullRewrite() const;

    /// Mark the session as requiring a full-rewrite save. Called by every
    /// destructive/structural operation (page delete/insert/reorder,
    /// redaction apply, form flatten, font embedding, encryption change,
    /// content-stream text rewrite, image/EXIF removal, grayscale recolor,
    /// layer/attachment/metadata/compliance edits, …). const because it only
    /// flips a bookkeeping bit; the underlying PDF mutation is performed by
    /// the caller via its own (friend) access to the document.
    void markRequiresFullRewrite() const;

    /// Tier 2 conditional-GC (pipeline b2e47d91, phase-3).
    ///
    /// Returns true once the session has performed a *dedup-worthy* operation
    /// — one whose full-rewrite genuinely benefits from MuPDF's most expensive
    /// garbage level (do_garbage=3: de-duplicate identical objects). The narrow
    /// trigger set is: redaction apply, page delete/reorder/import, form
    /// flatten, font embedding (PdfFontEmbedder), and encryption change.
    ///
    /// When false, full-rewrite saves use the cheaper default level
    /// (effectiveGarbageLevel(), gc=1), which still removes unreferenced
    /// objects — so confidentiality is preserved for the broader
    /// requires-full-rewrite set (text edits, image/EXIF removal, …) — but
    /// skips the super-linear dedup pass that dominated large-XFA save time.
    [[nodiscard]] bool needsFullGc() const;

    /// Mark the session as needing full (do_garbage=3) garbage collection.
    /// Implies markRequiresFullRewrite() — every dedup-worthy op is also
    /// destructive/structural. const for the same reason as above.
    void markNeedsFullGc() const;

    /// GitHub #19 / gap-register PDF-50 — what a full-rewrite save may do to a
    /// document that already carries APPLIED signatures.
    ///
    /// THE PROBLEM. A full rewrite (do_garbage + do_compress) rebuilds the file
    /// from the object graph and moves every byte offset in it. Every existing
    /// signature's `/ByteRange` then describes a file that no longer exists, so
    /// the signature stops verifying — and it stops verifying SILENTLY, because
    /// the signature dictionary itself is copied across intact. Form flatten
    /// (`FormManager::flattenForm`) is worse still: `pdf_bake_document` with
    /// `bake_widgets=1` bakes the signature widget away entirely, so the output
    /// has no signature dictionary at all and a dangling `/AcroForm` reference.
    /// Both were measured on `tests/fixtures/test_signed.pdf`; see
    /// `FlattenSignedFormTest.FlattenPreservesExistingSignatureFieldIntegrity`.
    ///
    /// THE POLICY. ISO 32000-1 §12.8 leaves an application two honest options:
    /// refuse, or produce output in which the signature is still verifiable. A
    /// full rewrite cannot do the second, so the default is REFUSE. Publishing
    /// a file whose `/ByteRange` no longer resolves, with no error and no
    /// warning, is the third outcome and it is the defect.
    ///
    /// WHY THE GUARD LIVES HERE AND NOT IN THE OPERATIONS. Flatten is only two
    /// of the destructive operations that set needsFullGc() — page delete,
    /// page import/graft, font embedding and redaction apply are the others,
    /// and every one of them breaks a signature in exactly the same way.
    /// Guarding the flatten call sites would have fixed two of six; guarding
    /// PUBLICATION fixes the class. It is also the only point at which the
    /// question "is a damaged file about to reach the user" can be answered.
    ///
    /// WHY IT IS NOT A BLANKET BLOCK ON SIGNED DOCUMENTS. It fires only when
    /// needsFullGc() is set, i.e. only when a destructive structural change has
    /// ALREADY been made in this session. The ordinary signed-document routes —
    /// opening, reading, form fills and annotations (which save incrementally
    /// and preserve the byte prefix) — never reach it.
    enum class SignedRewritePolicy {
        /// Default. A full-rewrite save of a document with applied signatures
        /// throws PdfeException and publishes nothing.
        Refuse,
        /// The caller has told the user the signatures will be destroyed and
        /// the user accepted. Restores the pre-#19 behaviour for that one
        /// document. Deliberately per-Document and not global: an opt-out that
        /// outlives the document it was granted for is the shape of every
        /// "control with a documented bypass" this codebase has had to fix.
        AcceptLoss,
    };

    /// Set this document's signed-rewrite policy. @see SignedRewritePolicy.
    void setSignedRewritePolicy(SignedRewritePolicy policy);
    [[nodiscard]] SignedRewritePolicy signedRewritePolicy() const;

    /// Number of APPLIED signatures (a `/V` with a non-empty `/Contents`, and
    /// therefore a `/ByteRange`) currently reachable in the in-memory
    /// document. 0 for a non-PDF. Never throws. Delegates to
    /// pdfe::core::countAppliedSignatures — the shared union-of-two-
    /// reachability-paths probe, not a third private traversal.
    [[nodiscard]] int appliedSignatureCount() const;

    /// The same count taken ONCE, when the document was opened.
    ///
    /// The two differ exactly when the session destroyed a signature, which is
    /// the case the guard exists for: `pdf_bake_document(bake_widgets=1)`
    /// orphans a signature widget and leaves `/Root /AcroForm` dangling, so by
    /// save time `appliedSignatureCount()` reports 0 on the very path that did
    /// the most damage.
    [[nodiscard]] int appliedSignatureCountAtOpen() const;

    /// The do_garbage level a full-rewrite save should use right now: 3 when
    /// needsFullGc(), else the Phase-1-validated safe default (gc=1, which is
    /// 150–980× faster than gc=3 on the large-XFA fixtures while round-tripping
    /// cleanly — see tests/docs/xfa_save_benchmark.md).
    [[nodiscard]] int effectiveGarbageLevel() const;

    /// PDF-02 (meta-audit gap register, issue #1). True when the document, AS
    /// OPENED, stored at least one object inside an object stream (/ObjStm) —
    /// the compact PDF 1.5+ on-disk shape in which object headers and their
    /// xref entries disappear into a single compressed stream.
    ///
    /// Captured once at open time, not recomputed: editing an object rewrites
    /// it into the incremental xref as a plain in-use entry, so a save-time
    /// scan would report the shape draining away over a long session.
    [[nodiscard]] bool sourceUsesObjectStreams() const;

    /// The MuPDF `pdf_write_options::do_use_objstms` value a full-rewrite save
    /// should use right now: 1 when the source used object streams, else 0.
    ///
    /// PRESERVE, never IMPOSE. Turning object streams on unconditionally would
    /// silently convert a classic-xref PDF 1.4 document into a 1.5+ one that
    /// older readers cannot open — a format upgrade nobody asked for, and the
    /// exact species of surprise applyVersionFloorForSave() exists to prevent
    /// in the other direction.
    [[nodiscard]] int effectiveObjectStreamMode() const;

    /// R2-04 (data-loss) orphaned-temp recovery. Set at open() time when a
    /// sibling "<filePath>.pdfe_tmp" is found — the residue of a prior
    /// full-rewrite save that died before its atomic rename. Returns the orphan
    /// temp path when one was detected, else std::nullopt.
    ///
    /// The recovery policy is deliberately conservative: open() SURFACES the
    /// orphan (via this accessor) but never auto-applies it, so neither the
    /// user's original nor the temp is modified until the caller decides. A UI
    /// can offer an explicit "recover unsaved changes" action.
    ///
    /// CORRECTION (F58-d, phase-53) — THIS IS NOT NECESSARILY A COMPLETE FILE.
    /// The phase-52 text here, and the matching comment in the constructor,
    /// asserted that the orphan "is a COMPLETE, pdf_save_document-finished file
    /// that never replaced the original". Nothing checked that, and it was
    /// false in two directions:
    ///   * a real disk-full between the temp write and the rename leaves a
    ///     TRUNCATED temp (SaveFailureInjectionTest.TruncatedOrphanTempIsSurfaced
    ///     NotApplied stages exactly that, and requires it to be surfaced);
    ///   * F58 briefly pointed the STAGED INCREMENTAL append at the same suffix,
    ///     so a real TerminateProcess mid-append left a 46 MB residue whose only
    ///     `startxref` is the ORIGINAL's — it opens as the PRE-save document
    ///     with 46 MB of unsigned junk past the last `%%EOF`, which for a signed
    ///     document is the classic incremental-update tampering shape.
    /// The second is now structurally impossible: staged appends use
    /// core::StagingFile's distinguishable "<filePath>.pdfe_part.<hex>" name and
    /// are swept at open, never surfaced. The first is real and is here to stay,
    /// so the contract is corrected rather than pretended away — use
    /// pendingRecoveredTempIsComplete() before offering the orphan to a user as
    /// a recoverable document.
    [[nodiscard]] std::optional<std::filesystem::path> pendingRecoveredTemp() const;

    /// F58-d (phase-53). True when the orphan reported by pendingRecoveredTemp()
    /// is structurally FINISHED — it begins with `%PDF-` and a `%%EOF` follows
    /// its last `startxref`, which is what a completed pdf_save_document always
    /// emits and what a save killed part-way through never does. False when
    /// there is no orphan, or when the orphan is a truncated carcass.
    ///
    /// A recovery UI must surface an incomplete orphan differently — as residue
    /// that may be salvageable, never as "your unsaved changes". Deliberately
    /// NOT size-based: an unedited full rewrite legitimately produces a temp the
    /// same size as the original, so size says nothing about completeness.
    [[nodiscard]] bool pendingRecoveredTempIsComplete() const;

    // Refresh cached state after structural changes
    void refreshPageCount();

    // Pipeline E phase-9: document-level JavaScript event hook. Caller
    // (typically MainWindow) registers a callback that is invoked from
    // save()/saveAs() (with "willSave"/"didSave") and from the
    // destructor (with "willClose"). The callback usually forwards to
    // JavaScriptEngine::fireDocumentEvent. Document itself stays
    // independent of the JS engine class.
    using DocumentEventCallback = std::function<void(const std::string&)>;
    void setDocumentEventCallback(DocumentEventCallback cb);

    // Non-copyable
    Document(const Document&) = delete;
    Document& operator=(const Document&) = delete;

    // Movable
    Document(Document&&) noexcept;
    Document& operator=(Document&&) noexcept;

private:
    /// Shared probe behind every *Allowed() accessor. @p muPdfPermBit is one of
    /// the PDF_PERM_* constants from <mupdf/pdf/crypt.h>. Returns true when the
    /// document carries no /Encrypt dictionary (nothing to enforce) or when the
    /// live permission bitmask has that bit set.
    bool permissionAllowed(int muPdfPermBit) const;

    /// F14 (PDF-01). Raise MuPDF's writer header version to pdfVersion()
    /// immediately before a full-rewrite save, so no save can emit a header
    /// EARLIER than the version the document was authored at. Touches only the
    /// writer's header field — never a PDF object — so it cannot perturb an
    /// incremental append's byte prefix. See the definition in Document.cpp for
    /// the MuPDF behaviour that makes this necessary.
    void applyVersionFloorForSave() const;


    /// F24 — run the installed save guard for @p target and act on its verdict:
    /// record the message, and throw PdfeException when the guard refuses.
    /// No-op when no guard is installed.
    void runSaveGuard(const std::filesystem::path& target) const;

    /// GitHub #19 / PDF-50 — refuse to PUBLISH a full rewrite of a document
    /// that carries applied signatures. @see SignedRewritePolicy. No-op unless
    /// needsFullGc() is set, the document has at least one applied signature,
    /// and the policy is Refuse. Called from save() and saveAs() immediately
    /// after runSaveGuard(), i.e. before anything is written and before
    /// "willSave" fires, so a refusal leaves every file exactly as it was.
    void runSignedRewriteGuard(const std::filesystem::path& target) const;

    friend class BookmarkExtractor;
    friend class BookmarkManager;
    friend class pdfe::textedit::ContentStreamEditor;
    friend class pdfe::fonts::EmbeddedFontAnalyzer;
    friend class pdfe::fonts::PdfFontEmbedder;
    friend class pdfe::fonts::Type3FontBuilder;
    friend class pdfe::tre::PdfContentWriter;
    friend class pdfe::tre::SynthesizedType3FontBundler;
    friend class pdfe::redaction::RedactionManager;
    friend class pdfe::encryption::EncryptionManager;
    friend class pdfe::signatures::SignatureManager;
    friend class pdfe::signatures::LtvDssWriter;
    // Pipeline F phase-10: read-back side of /DSS for offline LTV
    // verification (counterpart to LtvDssWriter on the write side).
    friend class pdfe::signatures::DssReader;
    friend class pdfe::signatures::DocMdpEnforcer;
    friend class pdfe::image::ImageManager;
    friend class pdfe::image::ImageContentEditor;
    friend class pdfe::image::ExifStripper;
    friend class pdfe::metadata::MetadataManager;
    friend class pdfe::compliance::XmpMetadataWriter;
    friend class pdfe::compliance::IccProfileManager;
    friend class pdfe::compliance::FontComplianceChecker;
    friend class pdfe::compliance::PdfAValidator;
    friend class pdfe::compliance::PdfAConverter;
    friend class pdfe::compliance::ProhibitedFeatureScanner;
    friend class pdfe::compliance::PdfUAConverter;
    friend class pdfe::compliance::StructureTreeBuilder;
    friend class pdfe::compliance::AutoTagger;
    friend class pdfe::compliance::ContentStreamTagger;
    friend class pdfe::compliance::ComplianceManager;
    friend class pdfe::layers::LayerManager;
    friend class pdfe::attachments::AttachmentManager;
    friend class pdfe::forms::FormManager;
    // Pipeline E phase-6: page label editor needs raw fz_document access
    // to walk /Catalog and the /PageLabels number tree.
    friend class PageLabelManager;
    // Pipeline E phase-7: GrayscaleConverter needs raw pdf_document
    // access to call pdf_recolor_page.
    friend class pdfe::optimize::GrayscaleConverter;
    friend void extractPagesToFile(
        std::shared_ptr<MuPdfContext>, Document*,
        const std::vector<int>&, const std::filesystem::path&);
    friend QRectF getPageCropBox(Document*, int);
    friend void setPageCropBox(Document*, int, const QRectF&);

    Document(
        std::shared_ptr<MuPdfContext> context,
        const std::filesystem::path& filePath,
        const std::string& password
    );

    /// Access the underlying fz_document handle (for internal use by friends).
    ::fz_document* fzDocument() const;

    class Impl;
    std::unique_ptr<Impl> pImpl;
};

} // namespace pdfe::core
