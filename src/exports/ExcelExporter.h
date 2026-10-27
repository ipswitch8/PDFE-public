#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace pdfe::core {
class Document;
}

namespace pdfe::exports {

struct ExcelExportOptions {
    std::vector<int> explicitPages;  // 0-based; empty = all pages
    /// Minimum number of columns (distinct x-origin clusters) a line
    /// must have to be considered part of a table. Defaults to 2.
    int minColumnsForTable = 2;
    /// Minimum number of rows (consecutive lines) sharing the same
    /// column structure to qualify as a table. Defaults to 2.
    int minRowsForTable = 2;
};

struct ExcelExportResult {
    std::filesystem::path outputPath;
    bool success = false;
    std::string errorMessage;
    int sheetsEmitted = 0;
    int tablesDetected = 0;
};

/// Exports tabular regions from a PDF to an XLSX workbook.
///
/// Table detection is a simple single-page column-clustering heuristic:
///   1. Extract all text lines from the page.
///   2. Cluster character x-origins into columns via 1D binning.
///   3. For each consecutive run of lines sharing the same column
///      count, emit a table block.
///
/// Output:
///   - One sheet per source page that has at least one detected table.
///   - Cells that parse cleanly as numbers are emitted as numeric
///     (t="n"); everything else is a shared-string cell.
class ExcelExporter {
public:
    explicit ExcelExporter(pdfe::core::Document* document);
    ~ExcelExporter();

    ExcelExportResult exportTo(const std::filesystem::path& outputPath,
                               const ExcelExportOptions& opts);

    /// True if `s` parses cleanly as a double with no leftover text.
    /// Exposed for testing.
    static bool looksLikeNumber(const std::string& s);

    /// Split a line of characters into cell-string values by clustering
    /// adjacent chars whose x-origins are closer than `columnGapPt`.
    /// Exposed for testing.
    static std::vector<std::string> splitLineIntoCells(
        const std::vector<std::pair<float, std::string>>& xPositionedTokens,
        float columnGapPt);

    /// Convert a 0-based column index to an Excel A1 reference letter
    /// (0 → "A", 25 → "Z", 26 → "AA"). Exposed for testing.
    static std::string columnLetter(int zeroBasedCol);

private:
    std::string buildSheetXml(int pageIndex, const ExcelExportOptions& opts,
                              int& tablesDetected);
    std::string buildWorkbookXml(int sheetCount);
    std::string buildWorkbookRels(int sheetCount);

    pdfe::core::Document* doc_ = nullptr;
};

} // namespace pdfe::exports
