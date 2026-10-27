#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace pdfe::core {
class Document;
}

namespace pdfe::exports {

enum class WordFormat { Docx, Rtf };

struct WordExportOptions {
    WordFormat format = WordFormat::Docx;
    std::vector<int> explicitPages;  // 0-based; empty = all pages
};

struct WordExportResult {
    std::filesystem::path outputPath;
    bool success = false;
    std::string errorMessage;
    int pagesExported = 0;
};

/// Exports a PDF to a single DOCX or RTF file.
///
/// Formatting preservation: text runs inherit font-size from MuPDF's
/// structured text output; bold / italic are inferred from the font
/// name (suffixes "Bold", "Italic", "Oblique") because MuPDF's fz_font
/// doesn't expose weight/slant directly. Underline is detected from
/// run-level formatting when available. Color is black by default.
///
/// Paragraph breaks come from MuPDF text blocks — each block becomes a
/// DOCX <w:p> or an RTF \par-terminated paragraph.
class WordExporter {
public:
    explicit WordExporter(pdfe::core::Document* document);
    ~WordExporter();

    /// Run the export synchronously to `outputPath`. The caller picks
    /// the path (no QFileDialog here — the dialog layer calls this
    /// with the already-chosen path).
    WordExportResult exportTo(const std::filesystem::path& outputPath,
                              const WordExportOptions& opts);

    /// Heuristic: does the given MuPDF font name imply bold weight?
    /// Static for testing.
    static bool fontNameImpliesBold(const std::string& fontName);

    /// Heuristic: does the font name imply italic / oblique?
    /// Static for testing.
    static bool fontNameImpliesItalic(const std::string& fontName);

    /// Escape text for a DOCX XML run (<, >, &, ").
    /// Static for testing.
    static std::string escapeXml(const std::string& s);

    /// Escape text for an RTF run ({, }, \). Also escapes non-ASCII
    /// characters as \u<N>? sequences.
    /// Static for testing.
    static std::string escapeRtf(const std::string& s);

private:
    std::string buildDocxDocumentXml(const WordExportOptions& opts);
    std::string buildRtfBody(const WordExportOptions& opts);
    bool writeDocx(const std::filesystem::path& out, const WordExportOptions& opts,
                   std::string& errorOut);
    bool writeRtf(const std::filesystem::path& out, const WordExportOptions& opts,
                  std::string& errorOut);

    pdfe::core::Document* doc_ = nullptr;
};

} // namespace pdfe::exports
