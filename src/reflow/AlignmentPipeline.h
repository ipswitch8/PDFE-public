#pragma once

#include "AlignmentInjector.h"
#include "AlignmentWriter.h"
#include "ReflowTypes.h"

#include <cstdio>
#include <filesystem>
#include <memory>
#include <unordered_map>
#include <vector>

namespace pdfe::core {
class MuPdfContext;
}

namespace pdfe::reflow {

/// Coordinates alignment changes across a user session: accumulates
/// per-page FormattingBlock edits as the user clicks L/C/R/J on the
/// TextEditToolBar, and flushes them via AlignmentInjector when the
/// document is saved.
///
/// This is the MainWindow-layer integration point. The underlying
/// engine (TextReflowEngine), emitter (AlignmentWriter), and splicer
/// (AlignmentInjector) are all tested in isolation; this class is the
/// thin coordinator that ties them to a document lifecycle.
class AlignmentPipeline {
public:
    explicit AlignmentPipeline(std::shared_ptr<pdfe::core::MuPdfContext> ctx);

    /// Record a pending alignment change for a block on the given page.
    /// If a block with the same bounds is already pending on the page,
    /// its alignment is updated in place (last-write-wins).
    void queueAlignment(int pageIndex,
                        const AlignmentWriter::BlockParams& params);

    /// Clear every pending block.
    void clear();

    /// Clear the pending blocks for a single page.
    void clearPage(int pageIndex);

    /// Number of pages with pending changes (for UI dirty indicators).
    std::size_t pendingPageCount() const;

    /// Write pending blocks to `output` based on `input`. Does NOT
    /// clear the queue — caller decides whether to commit by calling
    /// clear() after a successful save.
    void flush(const std::filesystem::path& input,
               const std::filesystem::path& output);

    /// flush(), with both ends as ALREADY-OPEN streams — WIN-87 site #5,
    /// phase-57. See AlignmentInjector::applyThroughHandles for the ownership
    /// contract (both streams are closed by this call) and for why the flush
    /// must not resolve either name: MainWindow invokes it with the user's
    /// document deliberately closed, so both names are unheld and predictable
    /// for the whole flush. Like flush(), it does NOT clear the queue.
    void flushThroughHandles(std::FILE* input,
                             const std::filesystem::path& inputPath,
                             std::FILE* output,
                             const std::filesystem::path& outputPathForGuard);

    /// Expose the queue for tests.
    std::vector<AlignmentInjector::PageBlock> snapshot() const;

private:
    std::shared_ptr<pdfe::core::MuPdfContext> ctx_;
    // Per-page list of block params; bounds are the key for merge.
    std::unordered_map<int, std::vector<AlignmentWriter::BlockParams>> byPage_;
};

} // namespace pdfe::reflow
