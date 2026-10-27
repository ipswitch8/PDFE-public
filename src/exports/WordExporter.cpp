#include "WordExporter.h"
#include "MiniZip.h"
#include "../core/Document.h"
#include "../core/Page.h"
#include "../core/TextBlock.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <sstream>
#include <string>

namespace pdfe::exports {

namespace {

/// Emit a UTF-8 codepoint into `out`.
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

bool containsCaseInsensitive(const std::string& haystack, const std::string& needle)
{
    auto it = std::search(
        haystack.begin(), haystack.end(), needle.begin(), needle.end(),
        [](char a, char b) {
            return std::tolower(static_cast<unsigned char>(a)) ==
                   std::tolower(static_cast<unsigned char>(b));
        });
    return it != haystack.end();
}

struct Run {
    std::string text;       // UTF-8
    std::string fontName;
    float       fontSize = 0.0f;
    bool        bold     = false;
    bool        italic   = false;
};

/// Convert a TextLine's chars into one or more Runs, splitting where
/// font name or size changes.
std::vector<Run> lineToRuns(const pdfe::core::TextLine& line)
{
    std::vector<Run> runs;
    for (const auto& ch : line.chars) {
        const std::string fn = ch.fontName;
        const float sz = ch.fontSize;
        const bool b  = WordExporter::fontNameImpliesBold(fn);
        const bool i  = WordExporter::fontNameImpliesItalic(fn);

        if (runs.empty() || runs.back().fontName != fn ||
            runs.back().fontSize != sz) {
            Run r;
            r.fontName = fn;
            r.fontSize = sz;
            r.bold = b;
            r.italic = i;
            runs.push_back(std::move(r));
        }
        appendUtf8(runs.back().text, ch.codepoint);
    }
    return runs;
}

std::vector<int> resolvePageList(const pdfe::core::Document* doc,
                                 const WordExportOptions& opts)
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

} // namespace

// -------- Heuristics (static, unit-testable) --------

bool WordExporter::fontNameImpliesBold(const std::string& fontName)
{
    if (containsCaseInsensitive(fontName, "Bold")) return true;
    if (containsCaseInsensitive(fontName, "Heavy")) return true;
    if (containsCaseInsensitive(fontName, "Black")) return true;
    return false;
}

bool WordExporter::fontNameImpliesItalic(const std::string& fontName)
{
    if (containsCaseInsensitive(fontName, "Italic"))  return true;
    if (containsCaseInsensitive(fontName, "Oblique")) return true;
    return false;
}

// -------- Escaping --------

std::string WordExporter::escapeXml(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    // NOTE: iterate as `unsigned char`. The previous `char` loop was signed on
    // MSVC, so every non-ASCII UTF-8 continuation byte compared as negative —
    // which would make any `c < 0x20` test below silently swallow the whole
    // upper half of UTF-8. The sibling FormEncoder::xmlEscape already iterates
    // unsigned for exactly this reason.
    for (unsigned char c : s) {
        switch (c) {
            case '&':  out += "&amp;";  break;
            case '<':  out += "&lt;";   break;
            case '>':  out += "&gt;";   break;
            case '"':  out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default:
                // XML-ILLEGAL CONTROL CHARACTERS. XML 1.0 §2.2 permits exactly
                // three characters below 0x20 — TAB, LF, CR — and nothing else.
                // 0x00-0x08, 0x0B, 0x0C and 0x0E-0x1F are forbidden OUTRIGHT:
                // not merely in need of escaping but unrepresentable, because a
                // numeric character reference to them (`&#x0B;`) is itself
                // illegal in XML 1.0. They cannot be escaped, only dropped.
                //
                // The text reaching here is document-controlled — it comes from
                // the PDF's own content streams — and the output is the
                // word/document.xml (or sheet XML) inside a .docx/.xlsx zip.
                // Emitting a raw control byte produces a part that is not
                // well-formed, and Word and Excel both reject the whole file as
                // corrupt rather than skipping the run. So one stray 0x0B in a
                // scanned page's reconstructed text loses the entire export.
                //
                // 0x7F (DEL) is legal in XML 1.0 but is dropped on the same
                // argument used in FormEncoder and sanitizeFilename: invisible,
                // meaningless in exported text, and useful only for making two
                // different strings render identically.
                if ((c < 0x20 && c != '\t' && c != '\n' && c != '\r') ||
                    c == 0x7F) {
                    break;      // dropped
                }
                out.push_back(static_cast<char>(c));
        }
    }
    return out;
}

std::string WordExporter::escapeRtf(const std::string& s)
{
    // Iterate the string as UTF-8 and emit \uN? for non-ASCII codepoints.
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            if (c == '{' || c == '}' || c == '\\') { out.push_back('\\'); out.push_back(static_cast<char>(c)); }
            else if (c == '\n') { out += "\\par "; }
            else { out.push_back(static_cast<char>(c)); }
            ++i;
            continue;
        }

        // Decode one UTF-8 codepoint.
        char32_t cp = 0;
        int extra = 0;
        if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
        else { ++i; continue; }  // invalid lead byte — skip

        if (i + extra >= s.size()) break;
        for (int k = 0; k < extra; ++k) {
            (void)k;
            cp = (cp << 6) | (static_cast<unsigned char>(s[++i]) & 0x3F);
        }
        i += 1;

        // RTF \u directive takes a signed 16-bit integer; cap at 0x7FFF
        // and emit as two 16-bit surrogates if beyond BMP.
        if (cp <= 0x7FFF) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "\\u%d?", static_cast<int>(cp));
            out += buf;
        } else {
            // Surrogate pair
            char32_t adj = cp - 0x10000;
            char32_t hi = 0xD800 | (adj >> 10);
            char32_t lo = 0xDC00 | (adj & 0x3FF);
            char buf[32];
            std::snprintf(buf, sizeof(buf), "\\u%d?\\u%d?",
                          static_cast<short>(hi), static_cast<short>(lo));
            out += buf;
        }
    }
    return out;
}

// -------- DOCX --------

namespace {

std::string xmlContentTypes()
{
    return
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">\n"
        "  <Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>\n"
        "  <Default Extension=\"xml\" ContentType=\"application/xml\"/>\n"
        "  <Override PartName=\"/word/document.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml\"/>\n"
        "</Types>\n";
}

std::string xmlRels()
{
    return
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">\n"
        "  <Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"word/document.xml\"/>\n"
        "</Relationships>\n";
}

std::string xmlDocumentRels()
{
    return
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">\n"
        "</Relationships>\n";
}

} // namespace

std::string WordExporter::buildDocxDocumentXml(const WordExportOptions& opts)
{
    std::ostringstream os;
    os <<
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">\n"
        "  <w:body>\n";

    const auto pages = resolvePageList(doc_, opts);
    for (int pageIdx : pages) {
        auto page = doc_->page(pageIdx);
        if (!page) continue;
        auto blocks = page->extractText();
        for (const auto& blk : blocks) {
            if (blk.type == pdfe::core::TextBlockType::Image) continue;
            os << "    <w:p>\n";
            for (const auto& line : blk.lines) {
                const auto runs = lineToRuns(line);
                for (const Run& run : runs) {
                    os << "      <w:r>\n"
                       << "        <w:rPr>\n";
                    if (run.bold)   os << "          <w:b/>\n";
                    if (run.italic) os << "          <w:i/>\n";
                    if (run.fontSize > 0.0f) {
                        // OOXML half-points.
                        os << "          <w:sz w:val=\""
                           << static_cast<int>(run.fontSize * 2) << "\"/>\n";
                    }
                    os << "        </w:rPr>\n"
                       << "        <w:t xml:space=\"preserve\">"
                       << escapeXml(run.text)
                       << "</w:t>\n"
                       << "      </w:r>\n";
                }
            }
            os << "    </w:p>\n";
        }
        // Page break between source pages so reflow in Word stays sensible.
        os << "    <w:p><w:r><w:br w:type=\"page\"/></w:r></w:p>\n";
    }

    os <<
        "  </w:body>\n"
        "</w:document>\n";
    return os.str();
}

bool WordExporter::writeDocx(const std::filesystem::path& out,
                             const WordExportOptions& opts,
                             std::string& errorOut)
{
    minizip::ZipWriter zw(out);
    if (!zw.valid()) {
        errorOut = "Failed to open DOCX for writing: " + out.string();
        return false;
    }
    if (!zw.addFile("[Content_Types].xml",         xmlContentTypes()))    { errorOut = "addFile [Content_Types].xml failed"; return false; }
    if (!zw.addFile("_rels/.rels",                 xmlRels()))            { errorOut = "addFile _rels/.rels failed"; return false; }
    if (!zw.addFile("word/_rels/document.xml.rels", xmlDocumentRels()))   { errorOut = "addFile document.xml.rels failed"; return false; }
    if (!zw.addFile("word/document.xml",           buildDocxDocumentXml(opts))) { errorOut = "addFile document.xml failed"; return false; }
    if (!zw.close()) { errorOut = "Zip finalization failed"; return false; }
    return true;
}

// -------- RTF --------

std::string WordExporter::buildRtfBody(const WordExportOptions& opts)
{
    std::ostringstream os;
    os << "{\\rtf1\\ansi\\ansicpg1252\\uc1\\deff0\n";
    os << "{\\fonttbl{\\f0\\fswiss\\fcharset0 Helvetica;}}\n";

    const auto pages = resolvePageList(doc_, opts);
    bool firstPage = true;
    for (int pageIdx : pages) {
        if (!firstPage) {
            os << "\\page\n";
        }
        firstPage = false;

        auto page = doc_->page(pageIdx);
        if (!page) continue;
        auto blocks = page->extractText();
        for (const auto& blk : blocks) {
            if (blk.type == pdfe::core::TextBlockType::Image) continue;
            for (const auto& line : blk.lines) {
                const auto runs = lineToRuns(line);
                for (const Run& run : runs) {
                    os << "{";
                    if (run.bold)   os << "\\b ";
                    if (run.italic) os << "\\i ";
                    if (run.fontSize > 0.0f) {
                        // RTF font size: half-points.
                        os << "\\fs" << static_cast<int>(run.fontSize * 2) << " ";
                    }
                    os << escapeRtf(run.text);
                    os << "}";
                }
                os << "\\line\n";
            }
            os << "\\par\n";
        }
    }
    os << "}\n";
    return os.str();
}

bool WordExporter::writeRtf(const std::filesystem::path& out,
                            const WordExportOptions& opts,
                            std::string& errorOut)
{
    std::ofstream fs(out, std::ios::binary);
    if (!fs) {
        errorOut = "Failed to open RTF for writing: " + out.string();
        return false;
    }
    const std::string body = buildRtfBody(opts);
    fs.write(body.data(), static_cast<std::streamsize>(body.size()));
    if (!fs) { errorOut = "Write failed"; return false; }
    return true;
}

// -------- Public entry --------

WordExporter::WordExporter(pdfe::core::Document* document)
    : doc_(document)
{
}

WordExporter::~WordExporter() = default;

WordExportResult WordExporter::exportTo(const std::filesystem::path& outputPath,
                                        const WordExportOptions& opts)
{
    WordExportResult r;
    r.outputPath = outputPath;
    if (!doc_) {
        r.errorMessage = "No document";
        return r;
    }

    // PDF-21 (meta-audit follow-up F2): content extraction is gated on the
    // document's copy/extract permission, exactly as PlainTextExporter does.
    // A DOCX/RTF export is a full text extraction — leaving it ungated made
    // "Export to Word" a one-click bypass of a copy-denied document. Refuse
    // before any page is touched so NO output file is created.
    if (!doc_->copyAllowed()) {
        r.errorMessage = "Content extraction is not permitted for this document";
        return r;
    }

    const auto pages = resolvePageList(doc_, opts);
    r.pagesExported = static_cast<int>(pages.size());

    std::string err;
    bool ok = false;
    switch (opts.format) {
        case WordFormat::Docx: ok = writeDocx(outputPath, opts, err); break;
        case WordFormat::Rtf:  ok = writeRtf (outputPath, opts, err); break;
    }
    if (!ok) {
        r.errorMessage = err;
        return r;
    }
    r.success = true;
    return r;
}

} // namespace pdfe::exports
