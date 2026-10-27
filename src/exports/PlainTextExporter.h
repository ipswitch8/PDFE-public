#pragma once

#include <filesystem>

namespace pdfe::core { class Document; }

namespace pdfe::exports {

/// Writes a document's extracted text to a .txt file. UTF-8 with BOM
/// (Windows editor compatibility) + form-feed page separators. Returns
/// true on success. Extracted into a standalone function so the
/// MainWindow File > Export Plain Text... slot is a thin wrapper and
/// the actual write path is unit-testable without driving the UI.
class PlainTextExporter {
public:
    static bool write(pdfe::core::Document* doc,
                      const std::filesystem::path& outPath);

    /// Byte sequences exposed for test assertions.
    static constexpr const char kUtf8Bom[] = "\xEF\xBB\xBF";
    static constexpr const char kPageSeparator[] = "\n\f\n";
};

} // namespace pdfe::exports
