#pragma once

#include "HeaderFooterConfig.h"
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace pdfe::core {
class MuPdfContext;
}

namespace pdfe::headerfooter {

/// Applies and removes page headers/footers via content-stream overlay.
/// Each applied stream is wrapped by the marker comments
///   %%PDFE-HDRFTR-START
///   ...
///   %%PDFE-HDRFTR-END
/// so remove() can find and drop them without touching unrelated content.
class HeaderFooterEngine {
public:
    explicit HeaderFooterEngine(std::shared_ptr<pdfe::core::MuPdfContext> context);
    ~HeaderFooterEngine();

    /// Apply header/footer to a PDF file. Writes to outputFile. If the input
    /// already has marker-tagged H/F streams, they are first removed so
    /// re-running does not stack duplicates.
    void apply(const std::filesystem::path& input,
               const std::filesystem::path& output,
               const HeaderFooterConfig& config);

    /// Remove any marker-tagged H/F streams from input, write to output.
    /// Safe no-op if none are present.
    void remove(const std::filesystem::path& input,
                const std::filesystem::path& output);

    /// Resolve tokens {date},{author},{page},{pages},{filename} and
    /// {bates:PREFIX:START[:WIDTH]} in a single slot string for a given
    /// page. WIDTH defaults to 6 digits. Exposed for testing.
    static std::string resolveTokens(const std::string& raw,
                                     int pageIndexZeroBased,
                                     int totalPages,
                                     const std::string& author,
                                     const std::string& filename,
                                     const std::string& isoDate);

    /// Decide whether a page should receive the H/F given the range mode.
    /// Exposed for testing. pageIndex is 0-based.
    static bool pageInRange(int pageIndexZeroBased,
                            int totalPages,
                            const HeaderFooterConfig& cfg);

    /// Marker comments written into every applied content stream.
    static constexpr const char* kStartMarker = "%%PDFE-HDRFTR-START";
    static constexpr const char* kEndMarker   = "%%PDFE-HDRFTR-END";

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pdfe::headerfooter
