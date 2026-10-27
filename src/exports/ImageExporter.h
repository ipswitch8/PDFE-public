#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace pdfe::core {
class Document;
}

namespace pdfe::exports {

enum class ImageFormat { Png, Jpeg };

struct ImageExportOptions {
    ImageFormat format = ImageFormat::Png;
    int         dpi    = 150;     // render DPI (72 = 1:1 PDF points)
    int         jpegQuality = 90; // 0-100; ignored for PNG
    std::string filenamePattern = "{basename}_page_{page}";  // no extension
    std::vector<int> explicitPages;  // 0-based; empty = all pages
};

struct ImageExportFileResult {
    int pageIndex = -1;
    std::filesystem::path outputPath;
    bool success = false;
    std::string errorMessage;
};

struct ImageExportResult {
    std::vector<ImageExportFileResult> fileResults;
    int totalRequested = 0;
    int successCount   = 0;
    int errorCount     = 0;
    bool cancelled     = false;
};

/// Worker-thread-friendly per-page image exporter. Writes PNG or JPEG
/// per page via a temp-file-and-rename pattern so a crash mid-export
/// cannot leave half-written files.
class ImageExporter {
public:
    using ProgressCallback = std::function<void(int done, int total)>;

    explicit ImageExporter(pdfe::core::Document* document);
    ~ImageExporter();

    /// Run the export synchronously. Progress is reported via the
    /// provided callback after each file (success or failure).
    ImageExportResult exportToFolder(const std::filesystem::path& outputDir,
                                     const ImageExportOptions& opts,
                                     ProgressCallback progress = nullptr);

    /// Request cancellation of an in-flight export. Safe to call from
    /// another thread.
    void cancel();

    /// Expose the filename-pattern substitution for testing.
    /// Supported tokens: {page} (1-based), {pageZ} (0-based),
    /// {basename} (document filename stem), {ext} (the target extension,
    /// without dot).
    static std::string applyPattern(const std::string& pattern,
                                    int pageIndexZeroBased,
                                    int totalPages,
                                    const std::string& docBasename,
                                    const std::string& extension);

private:
    pdfe::core::Document* doc_ = nullptr;
    std::atomic<bool> cancelled_{false};
};

} // namespace pdfe::exports
