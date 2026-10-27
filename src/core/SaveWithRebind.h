// -*- coding: utf-8 -*-
#pragma once

#include <exception>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <utility>
#include <vector>

#include "core/Command.h"
#include "core/Document.h"
#include "core/Page.h"
#include "core/UndoManager.h"

namespace pdfe::core {

/**
 * @brief Save @p doc on the full-rewrite path while command/undo state holds
 *        raw Page* refs into @p commandPages, keeping undo valid afterwards.
 *
 * Pipeline f4a82c1e, Phase 3 (Option B). The ordinary File>Save path
 * (MainWindow::onSaveFile) calls Document::save() with the session's
 * accumulated @c commandPages_ (and the undo stack's commands that point into
 * them) still alive. A live Page keeps the source @c fz_document handle open
 * via @c fz_keep_document, so on Windows the full-rewrite remove()+rename()
 * of the original fails, the retries exhaust, and save() throws after
 * reopening the pre-edit file — a silent lost-changes hazard. Dropping the
 * pages first (as the irreversible redaction path does) would dangle the raw
 * Page* pointers the undo commands hold and crash a later undo.
 *
 * These entry points own the correct sequence: snapshot each command's page
 * index, release ALL Page holders, run the write (now unlocked), then
 * re-resolve the pages and rebind the surviving commands so undo stays valid.
 * saveWithCommandPageRebind wraps Document::save (in-place; the file-lock /
 * lost-changes path). saveAsWithCommandPageRebind wraps Document::saveAs
 * (write to a new path; no in-place lock, but saveAs still drops+reopens the
 * page tree, so the rebind is what keeps a post-Save-As undo valid).
 * See tests/docs/journey-soak-phase3-findings.md §4.
 */
namespace detail {

/// Shared drain -> write -> rebind sequence. @p performWrite does the actual
/// Document::save / Document::saveAs; everything around it releases and
/// re-resolves the held pages so the write is not locked by a live Page and
/// the undo stack stays valid afterward.
template <typename WriteAction>
inline bool drainRebindAroundWrite(
    Document& doc,
    pdfe::commands::UndoManager& undo,
    std::vector<std::unique_ptr<Page>>& commandPages,
    WriteAction&& performWrite)
{
    // 1. Snapshot the page index each undo/redo command references, WHILE the
    //    pages are still alive. After the drop (step 2) a command's cached
    //    Page* dangles, so pageIndex() must be read here, not later.
    std::vector<std::pair<pdfe::commands::Command*, int>> commandSnapshot;
    undo.forEachCommand([&commandSnapshot](pdfe::commands::Command* cmd) {
        commandSnapshot.emplace_back(cmd, cmd->pageIndex());
    });

    // Every page index that must be live again after the write: the holders
    // themselves plus every command-referenced page.
    std::set<int> neededIndices;
    for (const auto& page : commandPages) {
        if (page) {
            neededIndices.insert(page->index());
        }
    }
    for (const auto& [cmd, idx] : commandSnapshot) {
        if (idx >= 0) {
            neededIndices.insert(idx);
        }
    }

    // 2. Release ALL Page holders. A live Page keeps the source fz_document
    //    handle open (fz_keep_document); with none held, an in-place
    //    remove()+rename() of the original can proceed.
    commandPages.clear();

    // 3. Perform the actual write — now unlocked.
    //
    //    R2-03 (data-loss): performWrite() CAN throw — Document::save() throws
    //    when the write still cannot complete (e.g. a residual file-lock holder,
    //    or the atomic rename-over ultimately failing). If we let that exception
    //    propagate HERE, steps 4-5 never run, so every undo/redo command is left
    //    holding its pre-drain raw Page* into the pages just dropped in step 2 →
    //    dangling → heap-use-after-free on the next undo(). So we ALWAYS run the
    //    re-resolve (step 4) + rebind (step 5) whether the write succeeds OR
    //    throws, then rethrow the original exception afterwards so the caller
    //    still surfaces the save failure.
    //
    //    This is safe against the thrown-save state: Document::save()'s failure
    //    path reopens the ORIGINAL (R2-04), and Document::saveAs() leaves its
    //    handles untouched on throw — so doc.pageCount()/doc.page(idx) below
    //    resolve against a live document. Any index that still cannot be resolved
    //    falls through to a nullptr rebind (the defined "page gone" safe-no-op).
    std::exception_ptr writeError;
    try {
        performWrite();
    } catch (...) {
        writeError = std::current_exception();
    }

    // 4. Re-resolve the needed pages into fresh holders (owned by commandPages,
    //    preserving the original invariant that command page_ points into it).
    //
    //    A snapshotted index can be STALE: Page::index() returns a cached
    //    snapshot, so a page-delete performed after a command's page was cached
    //    shrinks the document and leaves that command reporting an out-of-range
    //    index. Document::page(idx) THROWS on out-of-range, and an unhandled
    //    throw here would exit BEFORE step 5's rebind loop — leaving the entire
    //    undo stack pointing at the pages just dropped in step 2 (dangling
    //    Page* → UAF on the next undo). Guard the resolve robustly so the
    //    drain → write → rebind sequence ALWAYS completes:
    //      (a) read the live page count ONCE;
    //      (b) skip any index outside [0, pageCount);
    //      (c) wrap each resolve in its own try/catch so no single bad index can
    //          abort the loop.
    //    Any index left unresolved here falls through to step 5's nullptr
    //    rebind — a defined "page gone" state the command families treat as a
    //    safe no-op in undo()/redo().
    std::map<int, Page*> resolvedByIndex;
    // Read the live page count defensively: after a thrown save the document is
    // reopened+usable, but if pageCount() itself were to throw we must STILL
    // reach step 5 and rebind every command to nullptr rather than leave them
    // dangling. A count of 0 skips every index below → all commands rebind to
    // the safe "page gone" nullptr state.
    int pageCount = 0;
    try {
        pageCount = doc.pageCount();
    } catch (...) {
        pageCount = 0;
    }
    for (int idx : neededIndices) {
        if (idx < 0 || idx >= pageCount) {
            continue;
        }
        try {
            auto page = doc.page(idx);
            if (page) {
                resolvedByIndex[idx] = page.get();
                commandPages.push_back(std::move(page));
            }
        } catch (...) {
            // Defensive: leave idx unresolved → step 5 rebinds it to nullptr.
            // Never let one index abort the drain/rebind sequence.
        }
    }

    // 5. Rebind every surviving command to its freshly-resolved page so undo
    //    and redo dereference a live object rather than the dropped one.
    for (const auto& [cmd, idx] : commandSnapshot) {
        if (idx < 0) {
            continue;
        }
        auto it = resolvedByIndex.find(idx);
        cmd->rebindPage(it != resolvedByIndex.end() ? it->second : nullptr);
    }

    // R2-03: if the write threw, every command has now been rebound to a live
    // page or nullptr — safe to surface the original save failure to the caller.
    if (writeError) {
        std::rethrow_exception(writeError);
    }

    return true;
}

} // namespace detail

/// In-place File>Save (Document::save). Drains held pages so the full-rewrite
/// is not locked, then rebinds the undo commands. @return true on success.
inline bool saveWithCommandPageRebind(
    Document& doc,
    pdfe::commands::UndoManager& undo,
    std::vector<std::unique_ptr<Page>>& commandPages,
    const std::filesystem::path& path)
{
    return detail::drainRebindAroundWrite(
        doc, undo, commandPages, [&doc, &path]() { doc.save(path); });
}

/// File>Save As (Document::saveAs, new path). No in-place lock, but saveAs
/// drops+reopens the page tree, so the rebind keeps a post-Save-As undo valid.
/// @return true on success.
inline bool saveAsWithCommandPageRebind(
    Document& doc,
    pdfe::commands::UndoManager& undo,
    std::vector<std::unique_ptr<Page>>& commandPages,
    const std::filesystem::path& path)
{
    return detail::drainRebindAroundWrite(
        doc, undo, commandPages, [&doc, &path]() { doc.saveAs(path); });
}

} // namespace pdfe::core
