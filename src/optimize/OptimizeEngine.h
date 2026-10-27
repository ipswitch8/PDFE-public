#pragma once

#include "OptimizeTypes.h"
#include <filesystem>
#include <memory>

namespace pdfe::core {
class MuPdfContext;
}

namespace pdfe::optimize {

/// Runs PDF optimization options through MuPDF's pdf_save_document, which
/// natively supports stream compression, image re-compression, font
/// subsetting, and garbage collection of unused / duplicate objects.
/// Returns an OptimizeResult with before/after sizes and best-effort
/// per-category savings.
class OptimizeEngine {
public:
    explicit OptimizeEngine(std::shared_ptr<pdfe::core::MuPdfContext> context);
    ~OptimizeEngine();

    /// Execute the optimization. `input` and `output` must be distinct
    /// paths. If either path is invalid or the operation fails, the
    /// returned OptimizeResult.errorMessage is populated.
    OptimizeResult run(const std::filesystem::path& input,
                       const std::filesystem::path& output,
                       const OptimizeOptions& opts);

    /// Expose the simple per-step save helper (used for the per-category
    /// size estimates). Saves `input` to `output` with a single option
    /// enabled at a time. Useful for testing and for the engine's own
    /// breakdown computation.
    OptimizeResult runSingleOption(const std::filesystem::path& input,
                                   const std::filesystem::path& output,
                                   const OptimizeOptions& opts);

private:
    std::shared_ptr<pdfe::core::MuPdfContext> context_;
};

} // namespace pdfe::optimize
