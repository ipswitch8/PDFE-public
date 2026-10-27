#pragma once

#include "core/Command.h"

#include <atomic>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace pdfe::tre {

/// Runs OCR on a contiguous set of pages and remembers which pages it
/// actually processed so that undo() can invalidate just those cache
/// entries (single composite undo across N pages, per Phase-7d spec).
///
/// OCR invocation and cache invalidation are provided as callables so
/// the command can be unit-tested without a live Tesseract instance.
/// Production wiring passes lambdas that delegate to OcrPipeline.
class OcrAllPagesCommand : public pdfe::commands::Command {
public:
    using OcrFn        = std::function<void(int pageIndex)>;
    using InvalidateFn = std::function<void(int pageIndex)>;
    using ProgressFn   = std::function<bool(int done, int total)>;  // return false → cancel

    OcrAllPagesCommand(std::vector<int> pageIndices,
                       OcrFn ocr,
                       InvalidateFn invalidate,
                       ProgressFn progress = {});

    ~OcrAllPagesCommand() override = default;

    void execute() override;
    void undo() override;
    [[nodiscard]] std::string description() const override;

    /// True if execute() was interrupted by the progress callback
    /// returning false. The command is still undoable in this case —
    /// partially-processed pages will be invalidated.
    [[nodiscard]] bool wasCancelled() const { return cancelled_; }

    /// Pages successfully OCR'd (for tests + status reporting).
    [[nodiscard]] const std::vector<int>& processedPages() const { return processed_; }

private:
    std::vector<int> pageIndices_;
    OcrFn ocr_;
    InvalidateFn invalidate_;
    ProgressFn progress_;
    std::vector<int> processed_;
    bool cancelled_ = false;
    bool executed_  = false;
};

} // namespace pdfe::tre
