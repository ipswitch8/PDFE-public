#include "OcrAllPagesCommand.h"

namespace pdfe::tre {

OcrAllPagesCommand::OcrAllPagesCommand(std::vector<int> pageIndices,
                                       OcrFn ocr,
                                       InvalidateFn invalidate,
                                       ProgressFn progress)
    : pageIndices_(std::move(pageIndices))
    , ocr_(std::move(ocr))
    , invalidate_(std::move(invalidate))
    , progress_(std::move(progress))
{
}

void OcrAllPagesCommand::execute()
{
    processed_.clear();
    cancelled_ = false;

    const int total = static_cast<int>(pageIndices_.size());
    int done = 0;

    for (int pageIdx : pageIndices_) {
        try {
            if (ocr_) ocr_(pageIdx);
            processed_.push_back(pageIdx);
        } catch (...) {
            // Continue on per-page failure — the OCR engine raising for
            // one page (e.g. rendering error on an empty page) should
            // not abort the whole batch.
        }
        ++done;
        if (progress_ && !progress_(done, total)) {
            cancelled_ = true;
            break;
        }
    }
    executed_ = true;
}

void OcrAllPagesCommand::undo()
{
    if (!executed_) return;
    if (!invalidate_) return;
    for (int pageIdx : processed_) {
        try {
            invalidate_(pageIdx);
        } catch (...) {
            // Swallow — undo is best-effort.
        }
    }
    processed_.clear();
}

std::string OcrAllPagesCommand::description() const
{
    return "OCR All Pages";
}

} // namespace pdfe::tre
