#include "ExcelExporter.h"
#include "MiniZip.h"
#include "WordExporter.h"     // reuse escapeXml
#include "../core/Document.h"
#include "../core/Page.h"
#include "../core/TextBlock.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>

namespace pdfe::exports {

namespace {

void appendUtf8(std::string& out, char32_t cp)
{
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

std::string trim(const std::string& s)
{
    auto begin = s.begin();
    while (begin != s.end() && std::isspace(static_cast<unsigned char>(*begin))) ++begin;
    auto end = s.end();
    while (end != begin && std::isspace(static_cast<unsigned char>(*(end - 1)))) --end;
    return std::string(begin, end);
}

std::vector<int> resolvePageList(const pdfe::core::Document* doc,
                                 const ExcelExportOptions& opts)
{
    const int total = doc ? doc->pageCount() : 0;
    std::vector<int> pages;
    if (opts.explicitPages.empty()) {
        pages.reserve(total);
        for (int i = 0; i < total; ++i) pages.push_back(i);
    } else {
        std::copy_if(opts.explicitPages.begin(), opts.explicitPages.end(),
                     std::back_inserter(pages),
                     [total](int i) { return i >= 0 && i < total; });
    }
    return pages;
}

/// Convert a TextLine's chars into (x, tokenStr) pairs. Adjacent chars
/// on the same "word" (x-gap smaller than font-size * 0.4) merge.
std::vector<std::pair<float, std::string>> lineToPositionedTokens(
    const pdfe::core::TextLine& line)
{
    std::vector<std::pair<float, std::string>> out;
    if (line.chars.empty()) return out;

    const float sz = line.chars.front().fontSize > 0 ? line.chars.front().fontSize : 10.0f;
    const float wordGapThreshold = sz * 0.4f;

    std::string current;
    float currentStartX = 0.0f;
    float lastX = 0.0f;
    float lastAdvance = 0.0f;

    for (size_t i = 0; i < line.chars.size(); ++i) {
        const auto& ch = line.chars[i];
        const float x = static_cast<float>(ch.origin.x());
        const float charW = static_cast<float>(ch.quad.width());

        if (current.empty()) {
            currentStartX = x;
        } else if (x - (lastX + lastAdvance) > wordGapThreshold) {
            out.emplace_back(currentStartX, trim(current));
            current.clear();
            currentStartX = x;
        }
        appendUtf8(current, ch.codepoint);
        lastX = x;
        lastAdvance = charW > 0 ? charW : sz * 0.5f;
    }
    if (!current.empty()) {
        auto t = trim(current);
        if (!t.empty()) out.emplace_back(currentStartX, std::move(t));
    }
    return out;
}

} // namespace

// -------- Static helpers --------

bool ExcelExporter::looksLikeNumber(const std::string& s)
{
    if (s.empty()) return false;
    const char* begin = s.c_str();
    char* end = nullptr;
    std::strtod(begin, &end);
    if (end == begin) return false;
    // Accept only if strtod consumed all non-whitespace.
    while (*end != '\0') {
        if (!std::isspace(static_cast<unsigned char>(*end))) return false;
        ++end;
    }
    return true;
}

std::vector<std::string> ExcelExporter::splitLineIntoCells(
    const std::vector<std::pair<float, std::string>>& xPositionedTokens,
    float /*columnGapPt*/)
{
    // Each token is already one cell because lineToPositionedTokens
    // merges on small gaps. The column detection step lives in the
    // caller (it clusters cells across lines into a table grid).
    std::vector<std::string> out;
    out.reserve(xPositionedTokens.size());
    std::transform(xPositionedTokens.begin(), xPositionedTokens.end(),
                   std::back_inserter(out),
                   [](const auto& t) { return t.second; });
    return out;
}

std::string ExcelExporter::columnLetter(int zeroBasedCol)
{
    std::string out;
    int n = zeroBasedCol;
    while (true) {
        out.insert(out.begin(), static_cast<char>('A' + (n % 26)));
        n = n / 26 - 1;
        if (n < 0) break;
    }
    return out;
}

// -------- Sheet/Workbook XML builders --------

std::string ExcelExporter::buildSheetXml(int pageIndex,
                                         const ExcelExportOptions& opts,
                                         int& tablesDetected)
{
    std::ostringstream os;
    os <<
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">\n"
        "  <sheetData>\n";

    auto page = doc_->page(pageIndex);
    if (!page) {
        os << "  </sheetData>\n</worksheet>\n";
        return os.str();
    }
    auto blocks = page->extractText();

    // Flatten to a vector of (cells-per-line) grouped by block.
    struct LineCells { std::vector<std::string> cells; };
    std::vector<LineCells> allLines;

    for (const auto& blk : blocks) {
        if (blk.type == pdfe::core::TextBlockType::Image) continue;
        for (const auto& line : blk.lines) {
            auto tokens = lineToPositionedTokens(line);
            auto cells  = splitLineIntoCells(tokens, 0.0f);
            if (cells.empty()) continue;
            allLines.push_back({ std::move(cells) });
        }
    }

    // Group consecutive lines with >= minColumnsForTable cells into
    // candidate tables. This is intentionally simple — real Acrobat-class
    // table detection needs cluster analysis we can grow into later.
    int excelRow = 1;
    std::vector<LineCells> currentTable;

    auto flushTable = [&]() {
        if (static_cast<int>(currentTable.size()) < opts.minRowsForTable) {
            // Too few rows — emit as plain text rows (one cell per row).
            for (const auto& lc : currentTable) {
                std::ostringstream joined;
                for (size_t i = 0; i < lc.cells.size(); ++i) {
                    if (i > 0) joined << ' ';
                    joined << lc.cells[i];
                }
                const std::string text = joined.str();
                os << "    <row r=\"" << excelRow << "\">\n"
                   << "      <c r=\"A" << excelRow << "\" t=\"inlineStr\">"
                   << "<is><t xml:space=\"preserve\">"
                   << WordExporter::escapeXml(text)
                   << "</t></is></c>\n"
                   << "    </row>\n";
                ++excelRow;
            }
        } else {
            ++tablesDetected;
            for (const auto& lc : currentTable) {
                os << "    <row r=\"" << excelRow << "\">\n";
                for (size_t col = 0; col < lc.cells.size(); ++col) {
                    const std::string& v = lc.cells[col];
                    const std::string ref = columnLetter(static_cast<int>(col))
                                          + std::to_string(excelRow);
                    if (looksLikeNumber(v)) {
                        os << "      <c r=\"" << ref << "\" t=\"n\"><v>"
                           << v << "</v></c>\n";
                    } else {
                        os << "      <c r=\"" << ref << "\" t=\"inlineStr\">"
                           << "<is><t xml:space=\"preserve\">"
                           << WordExporter::escapeXml(v)
                           << "</t></is></c>\n";
                    }
                }
                os << "    </row>\n";
                ++excelRow;
            }
        }
        currentTable.clear();
    };

    for (auto& lc : allLines) {
        const int cols = static_cast<int>(lc.cells.size());
        if (cols >= opts.minColumnsForTable) {
            if (currentTable.empty() ||
                static_cast<int>(currentTable.back().cells.size()) == cols) {
                currentTable.push_back(std::move(lc));
            } else {
                flushTable();
                currentTable.push_back(std::move(lc));
            }
        } else {
            flushTable();
            // Single-column line emitted inline.
            os << "    <row r=\"" << excelRow << "\">\n"
               << "      <c r=\"A" << excelRow << "\" t=\"inlineStr\">"
               << "<is><t xml:space=\"preserve\">"
               << WordExporter::escapeXml(lc.cells.empty() ? "" : lc.cells[0])
               << "</t></is></c>\n"
               << "    </row>\n";
            ++excelRow;
        }
    }
    flushTable();

    os << "  </sheetData>\n</worksheet>\n";
    return os.str();
}

std::string ExcelExporter::buildWorkbookXml(int sheetCount)
{
    std::ostringstream os;
    os <<
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<workbook xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\"\n"
        "          xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\">\n"
        "  <sheets>\n";
    for (int i = 1; i <= sheetCount; ++i) {
        os << "    <sheet name=\"Page " << i << "\" sheetId=\"" << i
           << "\" r:id=\"rId" << i << "\"/>\n";
    }
    os << "  </sheets>\n</workbook>\n";
    return os.str();
}

std::string ExcelExporter::buildWorkbookRels(int sheetCount)
{
    std::ostringstream os;
    os <<
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">\n";
    for (int i = 1; i <= sheetCount; ++i) {
        os << "  <Relationship Id=\"rId" << i
           << "\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet\""
           << " Target=\"worksheets/sheet" << i << ".xml\"/>\n";
    }
    os << "</Relationships>\n";
    return os.str();
}

// -------- Entry --------

ExcelExporter::ExcelExporter(pdfe::core::Document* document)
    : doc_(document)
{
}

ExcelExporter::~ExcelExporter() = default;

ExcelExportResult ExcelExporter::exportTo(
    const std::filesystem::path& outputPath,
    const ExcelExportOptions& opts)
{
    ExcelExportResult r;
    r.outputPath = outputPath;
    if (!doc_) {
        r.errorMessage = "No document";
        return r;
    }

    // PDF-21 (meta-audit follow-up F2): table extraction reads the same text
    // MuPDF's structured-text pass yields, so it is content extraction and is
    // gated on copyAllowed() like PlainTextExporter. Refused BEFORE the
    // ZipWriter is opened, so no .xlsx is created.
    if (!doc_->copyAllowed()) {
        r.errorMessage = "Content extraction is not permitted for this document";
        return r;
    }

    const auto pages = resolvePageList(doc_, opts);

    // Build sheet XMLs first so we know how many we actually emit.
    std::vector<std::string> sheetXmls;
    sheetXmls.reserve(pages.size());
    for (int pageIdx : pages) {
        int tables = 0;
        std::string xml = buildSheetXml(pageIdx, opts, tables);
        r.tablesDetected += tables;
        sheetXmls.push_back(std::move(xml));
    }
    r.sheetsEmitted = static_cast<int>(sheetXmls.size());
    const int sheetCount = r.sheetsEmitted;

    minizip::ZipWriter zw(outputPath);
    if (!zw.valid()) {
        r.errorMessage = "Failed to open XLSX for writing: " + outputPath.string();
        return r;
    }

    // Content types covering both workbook and each worksheet part.
    std::ostringstream ct;
    ct <<
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">\n"
        "  <Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>\n"
        "  <Default Extension=\"xml\" ContentType=\"application/xml\"/>\n"
        "  <Override PartName=\"/xl/workbook.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml\"/>\n";
    for (int i = 1; i <= sheetCount; ++i) {
        ct << "  <Override PartName=\"/xl/worksheets/sheet" << i
           << ".xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml\"/>\n";
    }
    ct << "</Types>\n";

    const std::string topRels =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">\n"
        "  <Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"xl/workbook.xml\"/>\n"
        "</Relationships>\n";

    bool ok = true;
    ok &= zw.addFile("[Content_Types].xml", ct.str());
    ok &= zw.addFile("_rels/.rels",          topRels);
    ok &= zw.addFile("xl/workbook.xml",      buildWorkbookXml(sheetCount));
    ok &= zw.addFile("xl/_rels/workbook.xml.rels", buildWorkbookRels(sheetCount));
    for (int i = 0; i < sheetCount; ++i) {
        ok &= zw.addFile("xl/worksheets/sheet" + std::to_string(i + 1) + ".xml",
                         sheetXmls[i]);
    }
    if (!zw.close() || !ok) {
        r.errorMessage = "Failed to write XLSX parts";
        return r;
    }
    r.success = true;
    return r;
}

} // namespace pdfe::exports
