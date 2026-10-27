#pragma once

#include "TantivyIndex.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace pdfe::core {
class Document;
}

namespace pdfe::search {

/// F24 — outcome of a stale-index sweep. Returned rather than logged so the
/// behaviour is assertable: a sweep that reports "removed 0, kept 0" on a
/// populated root is a broken sweep, and nothing but a report can say so.
struct StaleIndexSweepReport {
    /// Index directories deleted.
    std::vector<std::filesystem::path> removed;
    /// Index directories left alone, with the reason.
    std::vector<std::filesystem::path> kept;
    /// True when the sweep refused to run at all because the root did not look
    /// like a PDFE index root (see DocumentSearchIndex::indexRoot).
    bool refusedRoot = false;
    /// Candidates skipped because the entry itself, its lock file, or something
    /// beneath it is a reparse point (Windows symlink OR NTFS junction) or a
    /// POSIX symlink — i.e. a redirection that could aim remove_all() outside
    /// the sweep root. Reported separately from `kept` so a test can assert the
    /// SPECIFIC reason, not merely that the entry survived. See the
    /// REDIRECTION gate in DocumentSearchIndex.cpp.
    std::vector<std::filesystem::path> skippedRedirection;
};

/// Owns a Tantivy index for a specific Document. The index is built
/// lazily on the first call to searchPages() and cached in a temp
/// directory keyed by the document's file path hash.
///
/// Usage:
///   DocumentSearchIndex idx(doc);
///   auto hits = idx.searchPages("query", 100);  // vector<SearchHit>
///
/// The caller is responsible for invalidating the index (calling
/// reindexPage / invalidate) when the document's text content changes.
class DocumentSearchIndex {
public:
    /// Build a search index bound to `doc`. Does NOT yet build the
    /// Tantivy index — that happens on the first searchPages() call.
    explicit DocumentSearchIndex(pdfe::core::Document* doc);
    ~DocumentSearchIndex();

    DocumentSearchIndex(const DocumentSearchIndex&) = delete;
    DocumentSearchIndex& operator=(const DocumentSearchIndex&) = delete;

    /// Search the index. If the index has not yet been built, build it
    /// first by extracting text from every page.
    std::vector<SearchHit> searchPages(const std::string& query, int maxHits);

    // ---- Pipeline G phase-8 spec'd interface (AC #1) ----
    // The phase-8 acceptance criteria name three methods explicitly:
    //   build(Document*)    — index all page text from a Document
    //   query(string)       — return candidate page indices
    //   invalidatePage(int) — update a single page's index entry
    // These are thin adapters over the lifecycle methods above so the
    // public surface matches the spec verbatim.

    /// Build / rebuild the index for the given document. Triggers an
    /// immediate full re-index. The Document* must match the one this
    /// instance was constructed with; otherwise this is a no-op.
    void build(pdfe::core::Document* doc);

    /// Run a query and return only the matching page indices, ranked
    /// by Tantivy score (highest first). Equivalent to searchPages()
    /// projected onto SearchHit::pageIndex with default maxHits=500.
    std::vector<int> query(const std::string& q);

    /// Re-extract and upsert the page text for a single page. Same
    /// behavior as reindexPage() (background-threaded upsert+commit).
    void invalidatePage(int pageIndex);
    // ---- End spec'd interface ----

    /// Reindex a single page (used after a text edit). No-op if the
    /// index has not been built yet.
    ///
    /// Page-text extraction happens synchronously on the calling
    /// thread (it must hold the document mutex), but the Tantivy
    /// upsert+commit is dispatched to a background worker so the UI
    /// thread is not blocked on disk I/O. Pipeline G phase-8 AC #3.
    void reindexPage(int pageIndex);

    /// Wait for any in-flight background reindex to finish. Tests use
    /// this to make assertions deterministic; production code may rely
    /// on the destructor's automatic join.
    void waitForPendingReindex();

    /// Drop the cached index and its on-disk files.
    void invalidate();

    /// Total documents in the Tantivy index (0 if not yet built).
    std::int64_t documentCount() const;

    /// Expose the index directory for tests.
    std::filesystem::path indexDir() const { return indexDir_; }

    // ---- F24: stale plaintext-index purge --------------------------------
    //
    // The PERMISSION GATE above stops NEW leaks. It does nothing about the
    // indexes a pre-gate build already wrote: every machine that ran one still
    // has a plaintext, searchable copy of restricted page text sitting in
    // %TEMP%\pdfe_search_indexes. Stopping the bleeding is only half the fix.
    //
    // The hazard in fixing it is the opposite of the one being fixed: an
    // over-eager sweep that deletes a CONCURRENTLY-RUNNING PDFE instance's live
    // index corrupts that instance's search mid-session. Two INDEPENDENT gates
    // must BOTH pass before anything is deleted:
    //
    //   (a) OWNERSHIP. A live index carries a sibling lock file
    //       "<key>.pdfe_index_lock" holding the owning process id, written when
    //       the index directory is created and held OPEN for the lifetime of
    //       the DocumentSearchIndex. A directory is a deletion candidate only
    //       if it has no lock file (the pre-fix leak shape — those builds never
    //       wrote one) or the recorded pid is not a live process. Anything we
    //       cannot decide is treated as ALIVE, so the sweep fails safe.
    //   (b) AGE. The directory's last-write time must be older than
    //       `staleAfter`. This is the belt to (a)'s braces: it also covers a
    //       concurrently-running PRE-FIX build, which has no lock file to find
    //       but does have a freshly-touched directory.
    //
    // Neither gate alone is sufficient and both are tested, including the
    // "another instance is running" case.

    /// Default age past which an unowned index directory is considered stale.
    static constexpr std::chrono::hours kDefaultStaleAfter{24};

    /// Name suffix of the sibling lock file for index directory "<key>".
    static constexpr const char* kLockSuffix = ".pdfe_index_lock";

    /// The directory holding every per-document index — %TEMP%/pdfe_search_indexes.
    static std::filesystem::path indexRoot();

    /// Remove stale index directories under @p root. Refuses (and reports
    /// `refusedRoot`) unless @p root is named "pdfe_search_indexes", so a
    /// mistaken caller cannot aim this at an arbitrary tree.
    static StaleIndexSweepReport sweepStaleIndexes(
        const std::filesystem::path& root,
        std::chrono::seconds staleAfter);

    /// Sweep the real index root with the default 24-hour threshold. Runs at
    /// most once per process; the first DocumentSearchIndex constructed
    /// triggers it, so the purge happens without the caller having to know.
    static StaleIndexSweepReport sweepStaleIndexes();

    /// GitHub #152 — sweep the index directories THIS PROCESS created, at
    /// process shutdown, in addition to the age-gated sweep at construction.
    ///
    /// WHY IT EXISTS. The construction sweep is OPPORTUNISTIC: it fires when a
    /// LATER index is constructed, and only against directories older than
    /// kDefaultStaleAfter (24 h). On a machine where PDFE is used once and not
    /// reopened, nothing ever sweeps and the age gate is never even evaluated —
    /// so a plaintext, searchable copy of every page of the user's document sits
    /// in %TEMP% indefinitely after they believe they closed it. This closes the
    /// cross-session window while leaving the within-session amortisation
    /// intact: the per-object destructor still keeps the files, so reopening the
    /// same document in the same session still reuses the index.
    ///
    /// "ITS OWN" IS THE WHOLE SAFETY ARGUMENT, and it is enforced three ways,
    /// each of which alone would leave a cross-instance data-loss hole of the
    /// kind AutosaveManager::isForeignLiveInstance exists to prevent:
    ///   1. Only directories THIS process registered (i.e. whose lock file this
    ///      process wrote) are candidates. Nothing is enumerated from disk.
    ///   2. The lock file is re-read at sweep time and must still record OUR
    ///      pid. Two instances opening the SAME document derive the SAME index
    ///      key, so the second claim overwrites the first — and the loser must
    ///      then leave the directory to the winner rather than delete it out
    ///      from under a live session.
    ///   3. A directory with a live DocumentSearchIndex still bound to it is
    ///      skipped, so an early or partial shutdown cannot pull the index away
    ///      from an object still using it.
    /// The redirection gates of the age-gated sweep apply unchanged, and the
    /// delete is the same handle-based removeTreeUnraceable.
    ///
    /// Registered with std::atexit by the first DocumentSearchIndex constructed,
    /// so it runs for the GUI, for a headless caller and for the test binary
    /// alike without every entry point having to remember. Idempotent: a second
    /// call finds an empty registry and reports nothing.
    static StaleIndexSweepReport sweepOwnIndexesAtShutdown();

    /// True when @p pid names a live process. Exposed because the sweep's
    /// safety rests on it and a test that cannot call it cannot prove it.
    static bool processIsAlive(unsigned long pid);

    /// True when @p p is a reparse point (Windows symlink OR NTFS junction) or
    /// a POSIX symlink — i.e. a name that resolves somewhere else.
    ///
    /// std::filesystem::is_symlink() is NOT sufficient on Windows: MSVC's
    /// implementation reports IO_REPARSE_TAG_SYMLINK but an NTFS JUNCTION uses
    /// IO_REPARSE_TAG_MOUNT_POINT, and `directory_entry::is_directory()` simply
    /// FOLLOWS it and answers true. A junction can be planted by anything
    /// running as the same user, needs no privilege (`mklink /J`), and would
    /// otherwise aim the sweep's remove_all() at whatever it points to. This
    /// predicate reads the file attributes directly so both tags are caught.
    ///
    /// Fails SAFE: an entry whose attributes cannot be read returns true
    /// (treat as a redirection, keep it), matching the sweep's existing
    /// undecidable-branch policy.
    ///
    /// Exposed because it is the whole of the redirection defence, and a
    /// safety rail a test cannot address directly is a rail nothing pins.
    static bool isRedirection(const std::filesystem::path& p);

    /// True when @p dir, or anything reachable beneath it WITHOUT following a
    /// redirection, is a redirection — or when the tree could not be fully
    /// classified (depth limit, unreadable entry). The per-candidate check is
    /// not enough on its own: remove_all() recurses, so a junction nested
    /// inside an otherwise ordinary index directory is the same hazard one
    /// level down.
    static bool treeContainsRedirection(const std::filesystem::path& dir);

public:
    /// Recursively delete `dir` in a way that CANNOT be raced by planting a
    /// reparse point mid-delete. Returns false, having deleted nothing further,
    /// on anything it cannot verify — refusing beats deleting the wrong object.
    ///
    /// This is what closes the check-then-act residual the sweep used to carry.
    /// Every entry is opened with FILE_FLAG_OPEN_REPARSE_POINT (so the open
    /// lands on a link rather than traversing it), classified from the HANDLE
    /// rather than from the path, matched by FILE ID against the id the
    /// directory enumeration reported (so a name swapped underneath us is
    /// detected), and deleted BY HANDLE (so there is no second name resolution
    /// to win). A junction is therefore unlinked, never descended.
    ///
    /// PUBLIC FOR TESTING as well as for the sweep: the sweep's own
    /// redirection scan refuses a candidate containing a junction before the
    /// delete is reached, so the delete's safety property is unreachable
    /// through the sweep and would otherwise be untestable. A guard that cannot
    /// be exercised is a guard nobody can show works.
    ///
    /// On non-Windows this falls back to std::filesystem::remove_all.
    static bool removeTreeUnraceable(const std::filesystem::path& dir);

private:
    void ensureBuilt();
    /// Builds the on-disk index. Returns false WITHOUT building if the index
    /// directory cannot be given an owner-only DACL — page plaintext must not
    /// land in a world-readable %TEMP% directory. Callers must not set built_
    /// on a false return; the in-process SearchEngine is the fallback.
    bool buildFromScratch();

    /// Extraction-permission predicate shared with the accessibility gate
    /// (src/ui/PdfCanvasAccessible.cpp): permitted when the document allows
    /// extract-for-accessibility (ISO 32000-1 bit 10) OR copy (bit 5).
    /// Governs whether page text may be materialised on disk at all; fails
    /// closed. See the block comment in DocumentSearchIndex.cpp.
    bool extractionAllowed() const;
    void joinReindexThread();

    /// Write (and hold open) the sibling lock file that marks indexDir_ as
    /// owned by this live process. Called whenever the directory is created.
    void claimIndexDir();

    /// Path of this index's sibling lock file.
    std::filesystem::path lockFilePath() const;

    pdfe::core::Document* doc_ = nullptr;
    std::filesystem::path indexDir_;
    /// Held open for the object's lifetime — see gate (a) above.
    std::ofstream lockFile_;
    /// True once claimIndexDir() has registered indexDir_ in the process-owned
    /// registry that sweepOwnIndexesAtShutdown() reads. Exists so the
    /// destructor decrements exactly once, whatever path it arrives by.
    bool registeredOwned_ = false;
    std::unique_ptr<TantivyIndex> index_;
    bool built_ = false;

    // Guards index_ and built_ across the UI thread (search, build) and
    // the background reindex worker. Tantivy itself serializes writers
    // internally, but our wrapper's owning pointer can race.
    mutable std::mutex indexMutex_;
    std::thread reindexThread_;
};

} // namespace pdfe::search
