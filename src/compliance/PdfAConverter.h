#pragma once

#include "ComplianceTypes.h"
#include <memory>
#include <filesystem>
#include <string>

namespace pdfe::core {
class MuPdfContext;
class Document;
}

namespace pdfe::compliance {

class PdfAConverter {
public:
    explicit PdfAConverter(std::shared_ptr<pdfe::core::MuPdfContext> context);
    ~PdfAConverter();
    PdfAConverter(const PdfAConverter&) = delete;
    PdfAConverter& operator=(const PdfAConverter&) = delete;

    ComplianceResult convertAndSave(
        pdfe::core::Document* doc,
        PdfALevel level,
        const std::filesystem::path& outputPath
    );

    ComplianceResult analyzeForConversion(
        pdfe::core::Document* doc,
        PdfALevel level
    );

    /// Public for testing: returns true if the given /BaseFont name
    /// matches one of the standard-14 PDF base fonts.
    static bool isStandardFourteen(const std::string& baseFontName);

    /// Public for testing: returns the absolute path to the TTF file
    /// chosen as the embedded replacement for the given standard-14
    /// name. Returns an empty string if no replacement is available
    /// on this system (test should skip in that case).
    static std::string resolveTtfReplacement(const std::string& baseFontName);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pdfe::compliance
