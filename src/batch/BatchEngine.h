#pragma once

#include "BatchTypes.h"
#include <memory>
#include <vector>
#include <filesystem>
#include <atomic>
#include <functional>

namespace pdfe::core {
class MuPdfContext;
}

namespace pdfe::batch {

class BatchEngine {
public:
    explicit BatchEngine(std::shared_ptr<pdfe::core::MuPdfContext> context);
    ~BatchEngine();

    BatchEngine(const BatchEngine&) = delete;
    BatchEngine& operator=(const BatchEngine&) = delete;

    using ProgressCallback = std::function<void(int fileIndex, int totalFiles, const std::string& fileName)>;
    using ErrorCallback = std::function<void(int fileIndex, const std::string& fileName, const std::string& error)>;

    void setProgressCallback(ProgressCallback cb);
    void setErrorCallback(ErrorCallback cb);

    BatchResult process(
        const std::vector<std::filesystem::path>& files,
        const std::vector<BatchAction>& actions
    );

    void cancel();
    bool isCancelled() const;

    // Public single-file text-mode watermark (legacy).
    void applyWatermark(
        const std::filesystem::path& file,
        const std::filesystem::path& outputFile,
        const WatermarkConfig& config
    );

    // Phase 4e: text or image watermark, with page-range filtering.
    // `ext` controls mode (text/image), imagePath, rangeMode, and explicitPages.
    // The text/position/opacity/fontSize/rotation fields still come from `base`.
    void applyWatermarkExt(
        const std::filesystem::path& file,
        const std::filesystem::path& outputFile,
        const WatermarkConfig& base,
        const WatermarkExtConfig& ext
    );

private:
    BatchFileResult processFile(
        const std::filesystem::path& file,
        const std::vector<BatchAction>& actions
    );

    void applyFlattenAnnotations(
        const std::filesystem::path& file,
        const std::filesystem::path& outputFile
    );

    void applyConvertToPdfA(
        const std::filesystem::path& file,
        const std::filesystem::path& outputFile,
        const PdfAConfig& config
    );

    void applySetPermissions(
        const std::filesystem::path& file,
        const std::filesystem::path& outputFile,
        const PermissionsConfig& config
    );

    std::shared_ptr<pdfe::core::MuPdfContext> context_;
    std::atomic<bool> cancelled_{false};
    ProgressCallback progressCb_;
    ErrorCallback errorCb_;
};

} // namespace pdfe::batch
