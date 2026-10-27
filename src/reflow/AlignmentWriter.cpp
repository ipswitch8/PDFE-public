#include "AlignmentWriter.h"

#include "core/ContentStreamPreamble.h"

#include <cstdio>
#include <sstream>

namespace pdfe::reflow {

namespace {
const char* alignmentWord(Alignment a)
{
    switch (a) {
        case Alignment::Left:    return "left";
        case Alignment::Center:  return "center";
        case Alignment::Right:   return "right";
        case Alignment::Justify: return "justify";
    }
    return "left";
}

Alignment wordToAlignment(const std::string& word)
{
    if (word == "center")  return Alignment::Center;
    if (word == "right")   return Alignment::Right;
    if (word == "justify") return Alignment::Justify;
    return Alignment::Left;
}
} // namespace

std::string AlignmentWriter::escapePdfLiteral(const std::string& utf8)
{
    std::string out;
    out.reserve(utf8.size());
    for (char c : utf8) {
        switch (c) {
            case '(':  out += "\\("; break;
            case ')':  out += "\\)"; break;
            case '\\': out += "\\\\"; break;
            default:   out.push_back(c);
        }
    }
    return out;
}

std::string AlignmentWriter::alignmentMarker(Alignment alignment)
{
    std::ostringstream os;
    os << "%%PDFE-ALIGN: " << alignmentWord(alignment) << "\n";
    return os.str();
}

Alignment AlignmentWriter::parseAlignmentMarker(const std::string& content)
{
    const std::string prefix = "%%PDFE-ALIGN:";
    const auto pos = content.find(prefix);
    if (pos == std::string::npos) return Alignment::Left;
    auto start = pos + prefix.size();
    // Skip whitespace
    while (start < content.size() &&
           (content[start] == ' ' || content[start] == '\t')) {
        ++start;
    }
    auto end = start;
    while (end < content.size() &&
           content[end] != '\n' && content[end] != '\r' &&
           content[end] != ' '  && content[end] != '\t') {
        ++end;
    }
    return wordToAlignment(content.substr(start, end - start));
}

std::string AlignmentWriter::emitFragment(const BlockParams& p)
{
    std::ostringstream os;
    os << alignmentMarker(p.alignment);

    if (p.lines.empty()) {
        return os.str();
    }

    os << "q\n";  // push graphics state
    // Reset ambient text/colour state to a known basis so a non-black ambient
    // fill (very common at end-of-page) can't render the aligned text invisible,
    // and an ambient -100 Tz / Tc / Tw / Ts can't mirror or distort it.
    os << pdfe::core::knownTextStateBasis();

    const float halfPoints = p.fontSizePt;  // Tf takes full points, not half

    // Walk lines top-to-bottom. PDF coordinates are y-up, so the first
    // line's baseline is at (blockTop - lineHeight). Subsequent lines
    // step down by lineHeight.
    float baselineY = p.blockTopPt - p.lineHeightPt;

    // For Justify, Tw (word spacing) adjustment goes outside BT…ET
    // only if we're using Tw per-line. Simplest: emit one BT per line
    // so we can Tw-adjust each line independently.
    for (size_t i = 0; i < p.lines.size(); ++i) {
        const auto& line = p.lines[i];

        const float xOffset = TextReflowEngine::lineXOffset(
            p.blockWidthPt, line.width, p.alignment, line.isLastLine);
        const float extraSpace = TextReflowEngine::justifyExtraSpace(
            p.blockWidthPt, line.width, line.wordCount, p.alignment,
            line.isLastLine);

        os << "BT\n";
        // Font + size
        os << "/" << p.fontResourceName << " "
           << halfPoints << " Tf\n";
        // Word spacing for justify (0 for other modes). The Tw operator
        // units are PDF user-space points; for justify we add exactly
        // (slack / wordGaps) points per space.
        if (extraSpace > 0.0f) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.3f Tw\n", extraSpace);
            os << buf;
        } else {
            os << "0 Tw\n";
        }
        // Tm: [a b c d e f] — identity scale/rotation, translate to
        // (blockLeft + xOffset, baselineY).
        char tmbuf[96];
        std::snprintf(tmbuf, sizeof(tmbuf),
                      "1 0 0 1 %.3f %.3f Tm\n",
                      p.blockLeftPt + xOffset,
                      baselineY);
        os << tmbuf;
        os << "(" << escapePdfLiteral(line.text) << ") Tj\n";
        os << "ET\n";

        baselineY -= p.lineHeightPt;
    }
    os << "Q\n";  // pop graphics state
    return os.str();
}

} // namespace pdfe::reflow
