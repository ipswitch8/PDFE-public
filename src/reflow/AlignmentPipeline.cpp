#include "AlignmentPipeline.h"

#include <QRectF>
#include <stdexcept>

namespace pdfe::reflow {

namespace {

bool boundsEqual(const QRectF& a, const QRectF& b)
{
    // Float equality by small epsilon — block bounds come from the
    // same computation for a given page region so exact match is
    // realistic in practice, but we tolerate tiny FP drift.
    constexpr double kEps = 0.01;
    return std::abs(a.left()   - b.left())   < kEps
        && std::abs(a.top()    - b.top())    < kEps
        && std::abs(a.width()  - b.width())  < kEps
        && std::abs(a.height() - b.height()) < kEps;
}

} // namespace

AlignmentPipeline::AlignmentPipeline(std::shared_ptr<pdfe::core::MuPdfContext> ctx)
    : ctx_(std::move(ctx))
{
    if (!ctx_) throw std::invalid_argument(
        "AlignmentPipeline: context must not be null");
}

void AlignmentPipeline::queueAlignment(int pageIndex,
                                       const AlignmentWriter::BlockParams& params)
{
    auto& list = byPage_[pageIndex];
    // Merge on bounds equality: a second click on the same block
    // replaces the prior alignment instead of stacking.
    const QRectF candidate(params.blockLeftPt, params.blockTopPt,
                           params.blockWidthPt,
                           static_cast<float>(params.lines.size())
                             * params.lineHeightPt);
    for (auto& existing : list) {
        const QRectF existingBounds(
            existing.blockLeftPt, existing.blockTopPt,
            existing.blockWidthPt,
            static_cast<float>(existing.lines.size())
              * existing.lineHeightPt);
        if (boundsEqual(existingBounds, candidate)) {
            existing = params;
            return;
        }
    }
    list.push_back(params);
}

void AlignmentPipeline::clear()
{
    byPage_.clear();
}

void AlignmentPipeline::clearPage(int pageIndex)
{
    byPage_.erase(pageIndex);
}

std::size_t AlignmentPipeline::pendingPageCount() const
{
    std::size_t n = 0;
    for (const auto& [_, v] : byPage_) {
        if (!v.empty()) ++n;
    }
    return n;
}

std::vector<AlignmentInjector::PageBlock> AlignmentPipeline::snapshot() const
{
    std::vector<AlignmentInjector::PageBlock> out;
    for (const auto& [pageIdx, params] : byPage_) {
        for (const auto& p : params) {
            AlignmentInjector::PageBlock b;
            b.pageIndex = pageIdx;
            b.params = p;
            out.push_back(std::move(b));
        }
    }
    return out;
}

void AlignmentPipeline::flush(const std::filesystem::path& input,
                              const std::filesystem::path& output)
{
    AlignmentInjector injector(ctx_);
    injector.apply(input, output, snapshot());
}

void AlignmentPipeline::flushThroughHandles(
    std::FILE* input, const std::filesystem::path& inputPath,
    std::FILE* output, const std::filesystem::path& outputPathForGuard)
{
    AlignmentInjector injector(ctx_);
    injector.applyThroughHandles(input, inputPath, output, outputPathForGuard,
                                 snapshot());
}

} // namespace pdfe::reflow
