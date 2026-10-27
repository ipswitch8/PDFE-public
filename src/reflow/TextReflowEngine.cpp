#include "TextReflowEngine.h"

#include <cctype>
#include <sstream>
#include <string>

namespace pdfe::reflow {

namespace {

bool isWhitespace(unsigned char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

std::vector<std::string> splitOnWhitespace(const std::string& s)
{
    std::vector<std::string> out;
    std::string cur;
    for (unsigned char c : s) {
        if (isWhitespace(c)) {
            if (!cur.empty()) {
                out.push_back(std::move(cur));
                cur.clear();
            }
        } else {
            cur.push_back(static_cast<char>(c));
        }
    }
    if (!cur.empty()) out.push_back(std::move(cur));
    return out;
}

} // namespace

std::vector<ReflowWord> TextReflowEngine::tokenize(const std::string& text,
                                                   const Measurer& measurer)
{
    std::vector<ReflowWord> out;
    auto words = splitOnWhitespace(text);
    out.reserve(words.size());
    for (auto& w : words) {
        ReflowWord rw;
        rw.width = measurer ? measurer(w) : 0.0f;
        rw.text  = std::move(w);
        out.push_back(std::move(rw));
    }
    return out;
}

std::vector<ReflowLine> TextReflowEngine::reflow(const std::vector<ReflowWord>& words,
                                                 float maxWidthPt,
                                                 float spaceWidthPt)
{
    std::vector<ReflowLine> lines;
    if (words.empty()) return lines;

    ReflowLine cur;
    cur.wordCount = 0;
    cur.width = 0.0f;
    cur.text.clear();

    for (size_t i = 0; i < words.size(); ++i) {
        const auto& w = words[i];
        // Prospective width if we add this word: current + space (if any) + word.
        const float spacer = (cur.wordCount > 0) ? spaceWidthPt : 0.0f;
        const float candidate = cur.width + spacer + w.width;

        if (cur.wordCount > 0 && candidate > maxWidthPt && maxWidthPt > 0) {
            // Commit current line and start a new one with this word.
            lines.push_back(std::move(cur));
            cur = {};
            cur.text = w.text;
            cur.width = w.width;
            cur.wordCount = 1;
        } else {
            if (cur.wordCount > 0) cur.text.push_back(' ');
            cur.text += w.text;
            cur.width = candidate;
            cur.wordCount += 1;
        }
    }
    if (cur.wordCount > 0) {
        lines.push_back(std::move(cur));
    }
    if (!lines.empty()) {
        lines.back().isLastLine = true;
    }
    return lines;
}

float TextReflowEngine::lineXOffset(float blockWidth, float lineWidth,
                                    Alignment alignment, bool isLastLine)
{
    if (blockWidth <= 0) return 0.0f;
    switch (alignment) {
        case Alignment::Left:    return 0.0f;
        case Alignment::Center:  return (blockWidth - lineWidth) / 2.0f;
        case Alignment::Right:   return (blockWidth - lineWidth);
        case Alignment::Justify:
            // Justify always anchors at offset 0; the per-glyph
            // stretching for non-last lines is computed separately
            // by justifyExtraSpace(). isLastLine doesn't influence
            // this offset value.
            (void)isLastLine;
            return 0.0f;
    }
    return 0.0f;
}

float TextReflowEngine::justifyExtraSpace(float blockWidth, float lineWidth,
                                          int wordCount, Alignment alignment,
                                          bool isLastLine)
{
    if (alignment != Alignment::Justify) return 0.0f;
    if (isLastLine) return 0.0f;
    if (wordCount < 2) return 0.0f;
    if (blockWidth <= lineWidth) return 0.0f;
    return (blockWidth - lineWidth) / static_cast<float>(wordCount - 1);
}

} // namespace pdfe::reflow
