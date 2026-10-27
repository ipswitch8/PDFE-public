#pragma once

#include "AlignmentWriter.h"
#include "ReflowTypes.h"

#include <cstdio>
#include <filesystem>
#include <memory>
#include <vector>

namespace pdfe::core {
class MuPdfContext;
}

namespace pdfe::reflow {

/// Splices one or more alignment-adjusted content-stream fragments
/// (produced by AlignmentWriter) into a PDF's pages. Uses the same
/// marker-based idempotency pattern as HeaderFooterEngine:
///
///   %%PDFE-ALIGN: <mode>
///   q
///     ...BT...Tm...ET...
///   Q
///
/// On apply(), any existing content streams that begin with
/// %%PDFE-ALIGN are removed first so re-running doesn't stack
/// duplicate paragraphs on the page.
class AlignmentInjector {
public:
    /// One block's worth of content for a specific page.
    struct PageBlock {
        int pageIndex = 0;                         // 0-based
        AlignmentWriter::BlockParams params;
    };

    explicit AlignmentInjector(std::shared_ptr<pdfe::core::MuPdfContext> ctx);
    ~AlignmentInjector();

    /// Apply `blocks` to the PDF at `input` and write the result to
    /// `output`. Existing PDFE-ALIGN streams are stripped first so
    /// applying + re-applying is idempotent. Throws on I/O failure.
    void apply(const std::filesystem::path& input,
               const std::filesystem::path& output,
               const std::vector<PageBlock>& blocks);

    /// apply(), with both ends as ALREADY-OPEN streams — WIN-87 site #5,
    /// phase-57. The path form resolves both names inside MuPDF, which is the
    /// substitution window MainWindow's alignment flush was exposed at (it
    /// runs with the user's document deliberately closed, so nothing holds
    /// either name). This form never resolves a name.
    ///
    /// `inputPath` and `outputPathForGuard` are used for DIAGNOSTICS and for
    /// the F24 certification guard's in-place test only — nothing opens them.
    /// Pass the staging file's path as `outputPathForGuard` so the guard sees
    /// exactly what the path form showed it.
    ///
    /// OWNERSHIP: both streams are closed by this call, on every path
    /// including the throwing one. The caller must not close either.
    void applyThroughHandles(std::FILE* input,
                             const std::filesystem::path& inputPath,
                             std::FILE* output,
                             const std::filesystem::path& outputPathForGuard,
                             const std::vector<PageBlock>& blocks);

    /// Remove any PDFE-ALIGN-marked streams from the input and write
    /// the cleaned-up document to `output`. Safe no-op if the input
    /// has no such markers.
    void remove(const std::filesystem::path& input,
                const std::filesystem::path& output);

    /// Marker start — a stream whose content begins with this literal
    /// comment is considered an alignment-injected stream and is
    /// eligible for strip/replace.
    static constexpr const char* kStartMarker = "%%PDFE-ALIGN";

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pdfe::reflow
