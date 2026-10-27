#include "HocrParser.h"

#include "core/XmlSafety.h"

#include <pugixml.hpp>
#include <QDebug>

#include <algorithm>
#include <charconv>
#include <cstring>
#include <limits>
#include <sstream>
#include <string>

namespace pdfe::tre {

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

namespace {

/// Trim leading and trailing whitespace from a string_view.
[[nodiscard]] std::string_view trimWhitespace(std::string_view sv) {
    const auto start = sv.find_first_not_of(" \t\r\n");
    if (start == std::string_view::npos) {
        return {};
    }
    const auto end = sv.find_last_not_of(" \t\r\n");
    return sv.substr(start, end - start + 1);
}

/// Split a string_view by a delimiter character, returning a vector of trimmed parts.
[[nodiscard]] std::vector<std::string_view> splitAndTrim(
    std::string_view sv,
    char delimiter
) {
    std::vector<std::string_view> parts;
    while (!sv.empty()) {
        const auto pos = sv.find(delimiter);
        if (pos == std::string_view::npos) {
            auto trimmed = trimWhitespace(sv);
            if (!trimmed.empty()) {
                parts.push_back(trimmed);
            }
            break;
        }
        auto part = trimWhitespace(sv.substr(0, pos));
        if (!part.empty()) {
            parts.push_back(part);
        }
        sv.remove_prefix(pos + 1);
    }
    return parts;
}

/// Split a string_view by whitespace into tokens.
[[nodiscard]] std::vector<std::string_view> splitByWhitespace(std::string_view sv) {
    std::vector<std::string_view> tokens;
    while (!sv.empty()) {
        const auto start = sv.find_first_not_of(" \t\r\n");
        if (start == std::string_view::npos) {
            break;
        }
        sv.remove_prefix(start);
        const auto end = sv.find_first_of(" \t\r\n");
        if (end == std::string_view::npos) {
            tokens.push_back(sv);
            break;
        }
        tokens.push_back(sv.substr(0, end));
        sv.remove_prefix(end);
    }
    return tokens;
}

/// Try to parse an integer from a string_view. Returns the default on failure.
[[nodiscard]] int parseIntSafe(std::string_view sv, int defaultVal = 0) {
    int result = defaultVal;
    auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), result);
    if (ec != std::errc()) {
        return defaultVal;
    }
    return result;
}

/// Try to parse a double from a string_view. Returns the default on failure.
/// Uses std::stod via a temporary string since from_chars for floating-point
/// is not universally available on all compilers with C++20.
[[nodiscard]] double parseDoubleSafe(std::string_view sv, double defaultVal = 0.0) {
    try {
        std::string str(sv);
        std::size_t pos = 0;
        double val = std::stod(str, &pos);
        // Ensure at least some characters were consumed
        if (pos == 0) {
            return defaultVal;
        }
        return val;
    } catch (...) {
        return defaultVal;
    }
}

/// Check if an XML node has a specific class attribute value.
/// HOCR class attributes can contain multiple space-separated classes.
[[nodiscard]] bool hasClass(const pugi::xml_node& node, std::string_view className) {
    const char* classAttr = node.attribute("class").as_string();
    if (!classAttr || classAttr[0] == '\0') {
        return false;
    }
    std::string_view classes(classAttr);
    // Check for exact match or match within space-separated list
    auto pos = classes.find(className);
    while (pos != std::string_view::npos) {
        // Verify it's a whole-word match
        const bool atStart = (pos == 0) || (classes[pos - 1] == ' ');
        const bool atEnd = (pos + className.size() == classes.size()) ||
                           (classes[pos + className.size()] == ' ');
        if (atStart && atEnd) {
            return true;
        }
        pos = classes.find(className, pos + 1);
    }
    return false;
}

/// Recursively find a descendant node with a given class.
/// Returns the first match in document order, or a null node.
[[nodiscard]] pugi::xml_node findNodeWithClass(
    const pugi::xml_node& root,
    std::string_view className
) {
    if (hasClass(root, className)) {
        return root;
    }
    for (auto child = root.first_child(); child; child = child.next_sibling()) {
        auto found = findNodeWithClass(child, className);
        if (found) {
            return found;
        }
    }
    return pugi::xml_node();
}

/// Collect all direct or descendant children with a specific class.
void collectChildrenWithClass(
    const pugi::xml_node& parent,
    std::string_view className,
    std::vector<pugi::xml_node>& out
) {
    for (auto child = parent.first_child(); child; child = child.next_sibling()) {
        if (hasClass(child, className)) {
            out.push_back(child);
        } else {
            // Recurse: HOCR structure may have intermediate wrapper elements
            collectChildrenWithClass(child, className, out);
        }
    }
}

/// Compute the union bounding box of multiple PixelRects.
[[nodiscard]] PixelRect unionBbox(const std::vector<PixelRect>& rects) {
    if (rects.empty()) {
        return {};
    }

    double minX = std::numeric_limits<double>::max();
    double minY = std::numeric_limits<double>::max();
    double maxX = std::numeric_limits<double>::lowest();
    double maxY = std::numeric_limits<double>::lowest();

    for (const auto& r : rects) {
        minX = std::min(minX, std::min(r.x0, r.x1));
        minY = std::min(minY, std::min(r.y0, r.y1));
        maxX = std::max(maxX, std::max(r.x0, r.x1));
        maxY = std::max(maxY, std::max(r.y0, r.y1));
    }

    return PixelRect{minX, minY, maxX, maxY};
}

// ---------------------------------------------------------------------------
// Column-major reading order (meta-audit F16, gap PDF-107)
// ---------------------------------------------------------------------------
//
// Tesseract's layout analysis does not always separate columns: on a
// two-column scan whose columns are short, it emits ONE ocr_carea / ocr_par
// whose ocr_line elements each run across BOTH columns. Verified with
// tests/fixtures/test_ocr_multicol.pdf, whose word boxes are perfectly
// column-partitioned (left 302..1179, right 1353..2201, an empty 174 px
// corridor between them) while the three hOCR lines each span 302..2201. Every
// page-segmentation mode Tesseract offers (1, 3, 4, 11, 12) produces the same
// merge, so this is not a configuration slip that can be tuned away.
//
// Downstream that merge is not cosmetic. HocrDocument's line order IS the
// reading order for everything the TRE reconstructs: the recognised text of a
// two-column scan came back interleaved row by row ("Left column paragraph.
// Contains several Right column paragraph. Independent content ..."), which is
// the text PDFE would then let a user edit and re-flow.
//
// So the parser re-derives columns from the geometry Tesseract got RIGHT (word
// boxes) rather than trusting the grouping it got wrong. A split happens only
// when the evidence is unambiguous:
//
//   * an x-corridor crossing the block that NO word overlaps,
//   * at least kMinCorridorToLineHeight times the median line height wide, so
//     inter-word and inter-sentence spacing can never qualify, and
//   * crossed by at least kMinSpanningLines separate lines, so a single
//     short line beside a figure, or a paragraph indent, does not qualify.
//
// When no such corridor exists the block is left byte-for-byte as parsed --
// single-column pages, which is nearly every page, take the identity path.

constexpr double kMinCorridorToLineHeight = 1.5;
constexpr size_t kMinSpanningLines = 2;

/// Recompute a line's bbox from its words.
[[nodiscard]] PixelRect lineBboxFromWords(const HocrLine& line) {
    std::vector<PixelRect> boxes;
    boxes.reserve(line.words.size());
    for (const auto& w : line.words) boxes.push_back(w.bbox);
    return unionBbox(boxes);
}

/// Split @p lines into columns, left to right. Returns one entry when the block
/// is single-column (the input, unchanged).
void collectColumns(std::vector<HocrLine> lines,
                    std::vector<std::vector<HocrLine>>& columns) {
    // Gather every word interval; a corridor is a gap in their union.
    std::vector<std::pair<double, double>> spans;
    std::vector<double> heights;
    for (const auto& line : lines) {
        for (const auto& w : line.words)
            if (w.bbox.x1 > w.bbox.x0) spans.emplace_back(w.bbox.x0, w.bbox.x1);
        const double h = line.bbox.y1 - line.bbox.y0;
        if (h > 0) heights.push_back(h);
    }
    if (lines.size() < kMinSpanningLines || spans.size() < 4 || heights.empty()) {
        columns.push_back(std::move(lines));
        return;
    }

    std::sort(heights.begin(), heights.end());
    const double medianHeight = heights[heights.size() / 2];
    const double minCorridor = kMinCorridorToLineHeight * medianHeight;

    std::sort(spans.begin(), spans.end());
    double reach = spans.front().second;
    double bestGapStart = 0.0, bestGapEnd = 0.0, bestGapWidth = 0.0;
    for (size_t i = 1; i < spans.size(); ++i) {
        if (spans[i].first > reach) {
            const double gap = spans[i].first - reach;
            if (gap > bestGapWidth) {
                bestGapWidth = gap;
                bestGapStart = reach;
                bestGapEnd = spans[i].first;
            }
        }
        reach = std::max(reach, spans[i].second);
    }

    if (bestGapWidth < minCorridor) {
        columns.push_back(std::move(lines));
        return;
    }

    const double cut = (bestGapStart + bestGapEnd) / 2.0;
    size_t spanning = 0;
    for (const auto& line : lines) {
        bool left = false, right = false;
        for (const auto& w : line.words) {
            if (w.bbox.center().x < cut) left = true; else right = true;
        }
        if (left && right) ++spanning;
    }
    if (spanning < kMinSpanningLines) {
        columns.push_back(std::move(lines));
        return;
    }

    std::vector<HocrLine> leftLines, rightLines;
    for (const auto& line : lines) {
        HocrLine l = line, r = line;
        l.words.clear();
        r.words.clear();
        for (const auto& w : line.words) {
            if (w.bbox.center().x < cut) l.words.push_back(w);
            else                         r.words.push_back(w);
        }
        if (!l.words.empty()) { l.bbox = lineBboxFromWords(l); leftLines.push_back(std::move(l)); }
        if (!r.words.empty()) { r.bbox = lineBboxFromWords(r); rightLines.push_back(std::move(r)); }
    }

    // Recurse so three or more columns split too; left side first, which is the
    // reading order for the left-to-right scripts Tesseract is configured for
    // here (RTL page layout would need the page's own direction, which hOCR
    // does not carry -- stated in the F16 dispositions, not silently assumed).
    collectColumns(std::move(leftLines), columns);
    collectColumns(std::move(rightLines), columns);
}

/// Re-group any block whose lines span multiple columns into one paragraph per
/// column, ordered left to right, each ordered top to bottom.
void splitColumnSpanningBlocks(HocrDocument& doc) {
    for (auto& block : doc.blocks) {
        std::vector<HocrLine> lines;
        std::string lang;
        for (const auto& par : block.paragraphs) {
            if (lang.empty()) lang = par.lang;
            for (const auto& line : par.lines) lines.push_back(line);
        }
        if (lines.size() < kMinSpanningLines) continue;

        std::vector<std::vector<HocrLine>> columns;
        collectColumns(lines, columns);
        if (columns.size() < 2) continue;    // identity path: leave as parsed

        std::vector<HocrParagraph> rebuilt;
        for (auto& col : columns) {
            if (col.empty()) continue;
            std::stable_sort(col.begin(), col.end(),
                             [](const HocrLine& a, const HocrLine& b) {
                                 return a.bbox.y0 < b.bbox.y0;
                             });
            HocrParagraph par;
            par.lang = lang;
            std::vector<PixelRect> boxes;
            boxes.reserve(col.size());
            for (const auto& l : col) boxes.push_back(l.bbox);
            par.bbox = unionBbox(boxes);
            par.lines = std::move(col);
            rebuilt.push_back(std::move(par));
        }
        if (rebuilt.size() >= 2) block.paragraphs = std::move(rebuilt);
    }
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// HocrParser::parseTitleAttribute
// ---------------------------------------------------------------------------

HocrParser::TitleProps HocrParser::parseTitleAttribute(std::string_view title) {
    TitleProps props;

    if (title.empty()) {
        return props;
    }

    auto parts = splitAndTrim(title, ';');

    for (auto part : parts) {
        auto tokens = splitByWhitespace(part);
        if (tokens.empty()) {
            continue;
        }

        const auto key = tokens[0];

        if (key == "bbox" && tokens.size() >= 5) {
            props.bbox.x0 = parseDoubleSafe(tokens[1]);
            props.bbox.y0 = parseDoubleSafe(tokens[2]);
            props.bbox.x1 = parseDoubleSafe(tokens[3]);
            props.bbox.y1 = parseDoubleSafe(tokens[4]);
            props.hasBbox = true;

        } else if (key == "x_bboxes" && tokens.size() >= 5) {
            // x_bboxes can be:
            // (a) Single bbox for one char: "x_bboxes x0 y0 x1 y1"
            // (b) Multiple bboxes for all chars in word: "x_bboxes x0 y0 x1 y1 x0 y0 x1 y1 ..."
            // Parse first bbox as the element's bbox, then store all as charBboxes.
            props.bbox.x0 = parseDoubleSafe(tokens[1]);
            props.bbox.y0 = parseDoubleSafe(tokens[2]);
            props.bbox.x1 = parseDoubleSafe(tokens[3]);
            props.bbox.y1 = parseDoubleSafe(tokens[4]);
            props.hasBbox = true;

            // Parse all sets of 4 coordinates into charBboxes
            for (size_t t = 1; t + 3 < tokens.size(); t += 4) {
                PixelRect r;
                r.x0 = parseDoubleSafe(tokens[t]);
                r.y0 = parseDoubleSafe(tokens[t + 1]);
                r.x1 = parseDoubleSafe(tokens[t + 2]);
                r.y1 = parseDoubleSafe(tokens[t + 3]);
                props.charBboxes.push_back(r);
            }

        } else if ((key == "x_wconf" || key == "x_conf") && tokens.size() >= 2) {
            props.confidence = parseIntSafe(tokens[1], -1);

        } else if (key == "x_fsize" && tokens.size() >= 2) {
            props.fontSizePx = static_cast<float>(parseDoubleSafe(tokens[1]));

        } else if (key == "baseline" && tokens.size() >= 3) {
            props.baselineSlope = static_cast<float>(parseDoubleSafe(tokens[1]));
            props.baselineOffset = static_cast<float>(parseDoubleSafe(tokens[2]));

        } else if (key == "x_size" && tokens.size() >= 2) {
            props.xSizePx = static_cast<float>(parseDoubleSafe(tokens[1]));

        } else if (key == "x_descenders" && tokens.size() >= 2) {
            props.descendersPx = static_cast<float>(parseDoubleSafe(tokens[1]));

        } else if (key == "x_ascenders" && tokens.size() >= 2) {
            props.ascendersPx = static_cast<float>(parseDoubleSafe(tokens[1]));

        } else if (key == "scan_res" && tokens.size() >= 3) {
            props.scanResX = parseIntSafe(tokens[1]);
            props.scanResY = parseIntSafe(tokens[2]);
        }
    }

    return props;
}

// ---------------------------------------------------------------------------
// HocrParser::parseWord
// ---------------------------------------------------------------------------

HocrWord HocrParser::parseWord(const pugi::xml_node& wordNode) {
    HocrWord word;

    // Extract text content: the inner text of the <span class="ocrx_word"> element.
    // child_value() returns concatenated text of all child PCDATA/CDATA nodes.
    const char* rawText = wordNode.child_value();
    if (rawText && rawText[0] != '\0') {
        word.text = rawText;
    } else {
        // Fallback: try text().get() which works with <span>text</span>
        word.text = wordNode.text().get();
    }

    // Parse the title attribute for bbox, confidence, font size, and x_bboxes
    const char* titleStr = wordNode.attribute("title").as_string();
    TitleProps wordProps;
    if (titleStr && titleStr[0] != '\0') {
        wordProps = parseTitleAttribute(titleStr);
        if (wordProps.hasBbox) {
            word.bbox = wordProps.bbox;
        }
        if (wordProps.confidence >= 0) {
            word.confidence = wordProps.confidence;
        }
        if (wordProps.fontSizePx > 0.0f) {
            word.fontSizePx = wordProps.fontSizePx;
        }
    }

    // Parse per-character data from ocrx_cinfo children (when hocr_char_boxes=1).
    // Tesseract emits: <span class="ocrx_cinfo" title="x_bboxes x0 y0 x1 y1; x_conf N">c</span>
    // Some versions put per-char bboxes in individual span titles; others put ALL
    // char bboxes in the parent word title as "x_bboxes x0 y0 x1 y1 x0 y0 x1 y1 ...".
    std::vector<pugi::xml_node> charNodes;
    collectChildrenWithClass(wordNode, "ocrx_cinfo", charNodes);
    for (size_t ci = 0; ci < charNodes.size(); ++ci) {
        const auto& charNode = charNodes[ci];
        HocrChar ch;

        // Extract single character text
        const char* charText = charNode.child_value();
        if (!charText || charText[0] == '\0') {
            charText = charNode.text().get();
        }
        if (charText && charText[0] != '\0') {
            auto u32 = utf8ToU32(std::string(charText));
            if (!u32.empty()) {
                ch.codepoint = u32[0];
            }
        }

        // Parse title for bbox and confidence (per-char title)
        const char* charTitle = charNode.attribute("title").as_string();
        if (charTitle && charTitle[0] != '\0') {
            auto charProps = parseTitleAttribute(charTitle);
            if (charProps.hasBbox) {
                ch.bbox = charProps.bbox;
            }
            ch.confidence = charProps.confidence >= 0 ? charProps.confidence : 0;
        }

        // Fallback: use word-level x_bboxes if per-char bbox is still zero.
        // Tesseract 4.x/5.x may put all char bboxes in the word title as a
        // flat list: "x_bboxes x0 y0 x1 y1 x0 y0 x1 y1 ..." (4 values per char).
        if (ch.bbox.x0 == 0 && ch.bbox.y0 == 0 && ch.bbox.x1 == 0 && ch.bbox.y1 == 0) {
            if (ci < wordProps.charBboxes.size()) {
                ch.bbox = wordProps.charBboxes[ci];
            }
        }

        if (ch.codepoint != 0) {
            word.chars.push_back(std::move(ch));
        }
    }

    // Warn when chars still have zero bboxes after word-level x_bboxes fallback
    if (!word.chars.empty()) {
        const int zeroBboxCount = static_cast<int>(std::count_if(
            word.chars.begin(), word.chars.end(),
            [](const auto& ch) {
                return ch.bbox.x0 == 0 && ch.bbox.y0 == 0
                    && ch.bbox.x1 == 0 && ch.bbox.y1 == 0;
            }));
        if (zeroBboxCount > 0) {
            qDebug() << "[HocrParser] word=" << QString::fromStdString(word.text)
                     << "has" << zeroBboxCount << "/" << word.chars.size()
                     << "chars with zero bboxes (wordCharBboxes=" << wordProps.charBboxes.size() << ")";
        }
    }

    // When char boxes are present but child_value() returned empty (Tesseract
    // changes structure with hocr_char_boxes=1), reconstruct word text from chars.
    if (word.text.empty() && !word.chars.empty()) {
        std::u32string u32text;
        for (const auto& ch : word.chars) {
            u32text += ch.codepoint;
        }
        // Convert back to UTF-8
        for (char32_t cp : u32text) {
            if (cp < 0x80) {
                word.text += static_cast<char>(cp);
            } else if (cp < 0x800) {
                word.text += static_cast<char>(0xC0 | (cp >> 6));
                word.text += static_cast<char>(0x80 | (cp & 0x3F));
            } else if (cp < 0x10000) {
                word.text += static_cast<char>(0xE0 | (cp >> 12));
                word.text += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                word.text += static_cast<char>(0x80 | (cp & 0x3F));
            } else {
                word.text += static_cast<char>(0xF0 | (cp >> 18));
                word.text += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                word.text += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                word.text += static_cast<char>(0x80 | (cp & 0x3F));
            }
        }
    }

    return word;
}

// ---------------------------------------------------------------------------
// HocrParser::parseLine
// ---------------------------------------------------------------------------

HocrLine HocrParser::parseLine(const pugi::xml_node& lineNode) {
    HocrLine line;

    // Parse the title attribute for line-level properties
    const char* titleStr = lineNode.attribute("title").as_string();
    if (titleStr && titleStr[0] != '\0') {
        auto props = parseTitleAttribute(titleStr);
        if (props.hasBbox) {
            line.bbox = props.bbox;
        }
        line.baselineSlope = props.baselineSlope;
        line.baselineOffset = props.baselineOffset;
        line.xSizePx = props.xSizePx;
        line.descendersPx = props.descendersPx;
        line.ascendersPx = props.ascendersPx;
    }

    // Collect words: <span class="ocrx_word"> children
    std::vector<pugi::xml_node> wordNodes;
    collectChildrenWithClass(lineNode, "ocrx_word", wordNodes);

    for (const auto& wordNode : wordNodes) {
        auto word = parseWord(wordNode);
        // Skip words with completely empty text
        if (!word.text.empty()) {
            line.words.push_back(std::move(word));
        }
    }

    return line;
}

// ---------------------------------------------------------------------------
// HocrParser::parseParagraph
// ---------------------------------------------------------------------------

HocrParagraph HocrParser::parseParagraph(const pugi::xml_node& parNode) {
    HocrParagraph paragraph;

    // Parse the title attribute for paragraph-level bbox
    const char* titleStr = parNode.attribute("title").as_string();
    if (titleStr && titleStr[0] != '\0') {
        auto props = parseTitleAttribute(titleStr);
        if (props.hasBbox) {
            paragraph.bbox = props.bbox;
        }
    }

    // Extract language attribute if present
    const char* langAttr = parNode.attribute("lang").as_string();
    if (langAttr && langAttr[0] != '\0') {
        paragraph.lang = langAttr;
    }

    // Collect lines: <span class="ocr_line"> or <span class="ocr_textfloat"> children
    std::vector<pugi::xml_node> lineNodes;
    collectChildrenWithClass(parNode, "ocr_line", lineNodes);
    // Also collect ocr_textfloat which Tesseract sometimes uses
    collectChildrenWithClass(parNode, "ocr_textfloat", lineNodes);

    for (const auto& lineNode : lineNodes) {
        auto line = parseLine(lineNode);
        paragraph.lines.push_back(std::move(line));
    }

    return paragraph;
}

// ---------------------------------------------------------------------------
// HocrParser::parseBlock
// ---------------------------------------------------------------------------

HocrBlock HocrParser::parseBlock(const pugi::xml_node& blockNode) {
    HocrBlock block;

    // Parse the title attribute for block-level bbox
    const char* titleStr = blockNode.attribute("title").as_string();
    if (titleStr && titleStr[0] != '\0') {
        auto props = parseTitleAttribute(titleStr);
        if (props.hasBbox) {
            block.bbox = props.bbox;
        }
    }

    // Collect paragraphs: <p class="ocr_par"> children
    std::vector<pugi::xml_node> parNodes;
    collectChildrenWithClass(blockNode, "ocr_par", parNodes);

    for (const auto& parNode : parNodes) {
        auto paragraph = parseParagraph(parNode);
        block.paragraphs.push_back(std::move(paragraph));
    }

    return block;
}

// ---------------------------------------------------------------------------
// HocrParser::parse
// ---------------------------------------------------------------------------

HocrDocument HocrParser::parse(std::string_view hocrXml) {
    HocrDocument result;

    if (hocrXml.empty()) {
        return result;
    }

    // Meta-audit F13 (SEC-41): hOCR is the third attacker-reachable XML entry
    // point (an hOCR file can be supplied directly, not only produced by the
    // bundled Tesseract), so it gets the same pre-parse guard as XMP and XFDF —
    // but with Policy::AllowDoctype, NOT the strict policy.
    //
    // The reason matters: real hOCR legitimately begins with an XHTML DOCTYPE
    // (`<!DOCTYPE html PUBLIC "-//W3C//DTD XHTML 1.0 Transitional//EN" ...>`),
    // which Tesseract emits on every document. Refusing that would break OCR on
    // valid input — a functional regression wearing a security control's badge,
    // and the fastest route to the control being switched off. The lenient
    // policy still refuses the primitives that actually matter here: entity
    // declarations, parameter-entity references, and external identifiers
    // anywhere other than the doctype's own (unfetched) external subset.
    //
    // An empty HocrDocument is this function's established "unusable input"
    // return, so refusal needs no new failure channel.
    if (!pdfe::core::xmlsafety::inspect(
            hocrXml, pdfe::core::xmlsafety::Policy::AllowDoctype).safe()) {
        return result;
    }

    // Load XML
    pugi::xml_document xmlDoc;
    pugi::xml_parse_result parseResult = xmlDoc.load_buffer(
        hocrXml.data(),
        hocrXml.size(),
        pugi::parse_default | pugi::parse_ws_pcdata_single
    );

    if (!parseResult) {
        // XML parsing failed entirely -- return empty document
        return result;
    }

    // Find the ocr_page element. Tesseract wraps it in <html><body><div class="ocr_page">.
    // Walk the tree to find it regardless of nesting.
    pugi::xml_node pageNode = findNodeWithClass(xmlDoc, "ocr_page");
    if (!pageNode) {
        // No recognizable page element found
        return result;
    }

    // Parse page-level title attribute for dimensions and scan resolution
    const char* pageTitleStr = pageNode.attribute("title").as_string();
    if (pageTitleStr && pageTitleStr[0] != '\0') {
        auto pageProps = parseTitleAttribute(pageTitleStr);
        if (pageProps.hasBbox) {
            // pageWidth and pageHeight come from the bbox upper-right corner.
            // The bbox is typically "bbox 0 0 W H" for the full page.
            result.pageWidth = static_cast<int>(pageProps.bbox.x1);
            result.pageHeight = static_cast<int>(pageProps.bbox.y1);
        }
        if (pageProps.scanResX > 0) {
            result.scanResX = pageProps.scanResX;
        }
        if (pageProps.scanResY > 0) {
            result.scanResY = pageProps.scanResY;
        }
    }

    // Collect blocks: <div class="ocr_carea"> children within the page
    std::vector<pugi::xml_node> blockNodes;
    collectChildrenWithClass(pageNode, "ocr_carea", blockNodes);

    // Some Tesseract outputs may use "ocr_block" instead of "ocr_carea"
    if (blockNodes.empty()) {
        collectChildrenWithClass(pageNode, "ocr_block", blockNodes);
    }

    // If there are still no explicit blocks but there are paragraphs directly
    // under the page, wrap them in a synthetic block
    if (blockNodes.empty()) {
        std::vector<pugi::xml_node> directPars;
        collectChildrenWithClass(pageNode, "ocr_par", directPars);
        if (!directPars.empty()) {
            // Create a single synthetic block containing all paragraphs
            HocrBlock syntheticBlock;
            syntheticBlock.bbox = PixelRect{
                0, 0,
                static_cast<double>(result.pageWidth),
                static_cast<double>(result.pageHeight)
            };
            for (const auto& parNode : directPars) {
                auto paragraph = parseParagraph(parNode);
                syntheticBlock.paragraphs.push_back(std::move(paragraph));
            }
            result.blocks.push_back(std::move(syntheticBlock));
            splitColumnSpanningBlocks(result);
            return result;
        }
    }

    for (const auto& blockNode : blockNodes) {
        auto block = parseBlock(blockNode);
        result.blocks.push_back(std::move(block));
    }

    splitColumnSpanningBlocks(result);
    return result;
}

// ---------------------------------------------------------------------------
// HocrParser::extractRegion
// ---------------------------------------------------------------------------

RegionTextData HocrParser::extractRegion(
    const HocrDocument& doc,
    const PixelRect& selectionPixels
) {
    RegionTextData result;
    std::vector<PixelRect> includedLineBboxes;
    std::string textAccumulator;
    bool firstLine = true;

    // Iterate all blocks, paragraphs, and lines.
    // Include any line whose bbox overlaps the selection (snap-to-line boundary).
    for (const auto& block : doc.blocks) {
        for (const auto& paragraph : block.paragraphs) {
            for (const auto& line : paragraph.lines) {
                if (line.bbox.intersects(selectionPixels)) {
                    // Include this entire line
                    result.lines.push_back(line);
                    includedLineBboxes.push_back(line.bbox);

                    // Build plain text: words separated by spaces, lines by newlines
                    if (!firstLine) {
                        textAccumulator += '\n';
                    }
                    firstLine = false;

                    bool firstWord = true;
                    for (const auto& word : line.words) {
                        if (!firstWord) {
                            textAccumulator += ' ';
                        }
                        firstWord = false;
                        textAccumulator += word.text;
                    }
                }
            }
        }
    }

    // Set the pixel bounding box as the union of all included lines
    result.pixelBbox = unionBbox(includedLineBboxes);

    // Leave pdfBbox at default (0,0,0,0) -- caller converts via CoordMapper
    result.plainText = std::move(textAccumulator);

    return result;
}

} // namespace pdfe::tre
