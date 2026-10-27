#include "TextEditModel.h"

#include <algorithm>
#include <limits>

namespace pdfe::textedit {

TextEditModel::TextEditModel(const std::vector<pdfe::core::TextBlock>& blocks)
{
    for (int bi = 0; bi < static_cast<int>(blocks.size()); ++bi) {
        const auto& block = blocks[bi];
        for (int li = 0; li < static_cast<int>(block.lines.size()); ++li) {
            const auto& line = block.lines[li];
            if (line.chars.empty()) continue;

            // Group characters by font name to create spans.
            // Each run of chars with the same font becomes one span.
            int runStart = 0;
            while (runStart < static_cast<int>(line.chars.size())) {
                const std::string& runFont = line.chars[runStart].fontName;
                float runFontSize = line.chars[runStart].fontSize;
                int runEnd = runStart + 1;

                while (runEnd < static_cast<int>(line.chars.size()) &&
                       line.chars[runEnd].fontName == runFont) {
                    ++runEnd;
                }

                // Build the span
                TextSpan span;
                span.blockIndex = bi;
                span.lineIndex = li;
                span.charStartIndex = runStart;
                span.charEndIndex = runEnd;
                span.fontName = runFont;
                span.fontSize = runFontSize;
                // R2-21a: carry the source line's writing mode onto the editable
                // span so TextEditTool::selectSpan can auto-select vertical/RTL
                // overlay geometry (R2-21b) without a manual setter.
                span.writingMode = line.writingMode;
                // Original fill color from the first char of the run. Carried so a
                // text edit re-emits the same color instead of losing it to the
                // ambient fill / black (see ContentStreamEditor emission).
                span.textColor = line.chars[runStart].color;
                span.textColorKnown = true;

                // Build text and quads, and aggregate the descriptor-derived
                // style across the run's chars (the run shares one font, so the
                // chars agree; a majority vote is robust to any stray outlier).
                QRectF bbox;
                int italicCount = 0;
                int boldCount = 0;
                for (int ci = runStart; ci < runEnd; ++ci) {
                    const auto& ch = line.chars[ci];
                    span.text.push_back(ch.codepoint);
                    span.charQuads.push_back(ch.quad);
                    if (ch.isItalic) ++italicCount;
                    if (ch.isBold) ++boldCount;

                    if (ci == runStart) {
                        bbox = ch.quad;
                    } else {
                        bbox = bbox.united(ch.quad);
                    }
                }
                const int runLen = runEnd - runStart;
                span.isItalic = italicCount * 2 > runLen;
                span.isBold = boldCount * 2 > runLen;
                span.originalText = span.text;
                span.boundingBox = bbox;

                spans_.push_back(std::move(span));
                runStart = runEnd;
            }
        }
    }
}

int TextEditModel::spanCount() const
{
    return static_cast<int>(spans_.size());
}

const TextSpan& TextEditModel::span(int index) const
{
    return spans_.at(index);
}

TextSpan& TextEditModel::mutableSpan(int index)
{
    return spans_.at(index);
}

int TextEditModel::spanAtPoint(const QPointF& pdfPoint) const
{
    // First pass: check bounding boxes
    for (int i = 0; i < static_cast<int>(spans_.size()); ++i) {
        if (spans_[i].boundingBox.contains(pdfPoint)) {
            return i;
        }
    }

    // Second pass: check individual char quads (handles tight bboxes)
    for (int i = 0; i < static_cast<int>(spans_.size()); ++i) {
        const auto& quads = spans_[i].charQuads;
        if (std::any_of(quads.begin(), quads.end(),
                [&pdfPoint](const auto& quad) { return quad.contains(pdfPoint); })) {
            return i;
        }
    }

    return -1;
}

int TextEditModel::charIndexAtPoint(int spanIndex, const QPointF& pdfPoint) const
{
    if (spanIndex < 0 || spanIndex >= static_cast<int>(spans_.size()))
        return 0;

    const auto& span = spans_[spanIndex];
    if (span.charQuads.empty())
        return 0;

    // Find the character whose quad center is closest to the point
    float minDist = std::numeric_limits<float>::max();
    int bestIndex = 0;

    for (int i = 0; i < static_cast<int>(span.charQuads.size()); ++i) {
        QPointF center = span.charQuads[i].center();
        float dx = static_cast<float>(pdfPoint.x() - center.x());
        float dy = static_cast<float>(pdfPoint.y() - center.y());
        float dist = dx * dx + dy * dy;

        if (dist < minDist) {
            minDist = dist;
            bestIndex = i;
        }
    }

    // Determine if cursor should be before or after the character
    // based on horizontal position relative to char center
    if (bestIndex < static_cast<int>(span.charQuads.size())) {
        QPointF center = span.charQuads[bestIndex].center();
        if (pdfPoint.x() > center.x()) {
            ++bestIndex;
        }
    }

    return bestIndex;
}

void TextEditModel::replaceSpanText(int spanIndex, const std::u32string& newText)
{
    if (spanIndex < 0 || spanIndex >= static_cast<int>(spans_.size()))
        return;
    spans_[spanIndex].text = newText;
}

void TextEditModel::replaceCharRange(int spanIndex, int startChar, int endChar,
                                      const std::u32string& newText)
{
    if (spanIndex < 0 || spanIndex >= static_cast<int>(spans_.size()))
        return;

    auto& span = spans_[spanIndex];
    int len = static_cast<int>(span.text.size());
    startChar = std::clamp(startChar, 0, len);
    endChar = std::clamp(endChar, startChar, len);

    // Compute the shift: positive = text grew, negative = text shrank
    int removedLen = endChar - startChar;
    int insertedLen = static_cast<int>(newText.size());
    int delta = insertedLen - removedLen;

    span.text.erase(span.text.begin() + startChar, span.text.begin() + endChar);
    span.text.insert(span.text.begin() + startChar, newText.begin(), newText.end());

    // Adjust formattingRuns indices to stay aligned with the modified text
    if (delta != 0 || removedLen > 0) {
        adjustFormattingRuns(span.formattingRuns, startChar, removedLen, insertedLen);
    }
}

void TextEditModel::adjustFormattingRuns(std::vector<FormattingRun>& runs,
                                          int editPos, int removedLen, int insertedLen)
{
    int delta = insertedLen - removedLen;
    int editEnd = editPos + removedLen;

    for (auto it = runs.begin(); it != runs.end(); ) {
        auto& run = *it;

        if (run.charEnd <= editPos) {
            // Run is entirely before the edit — no change
            ++it;
        } else if (run.charStart >= editEnd) {
            // Run is entirely after the edit — shift by delta
            run.charStart += delta;
            run.charEnd += delta;
            ++it;
        } else if (run.charStart >= editPos && run.charEnd <= editEnd) {
            // Run is entirely within the deleted range — remove it
            it = runs.erase(it);
        } else if (run.charStart < editPos && run.charEnd > editEnd) {
            // Run spans across the edit — adjust its end
            run.charEnd += delta;
            ++it;
        } else if (run.charStart < editPos && run.charEnd <= editEnd) {
            // Run starts before edit, ends within deleted range — trim end.
            // (charEnd > editPos already established by branches above.)
            run.charEnd = editPos + insertedLen;
            ++it;
        } else if (run.charStart >= editPos && run.charEnd > editEnd) {
            // Run starts within deleted range, ends after — trim start.
            // (charStart < editEnd already established by branches above.)
            run.charStart = editPos + insertedLen;
            run.charEnd += delta;
            ++it;
        } else {
            ++it;
        }

        // Remove degenerate runs (charStart >= charEnd)
        if (it != runs.begin()) {
            auto prev = it - 1;
            if (prev->charStart >= prev->charEnd) {
                it = runs.erase(prev);
            }
        }
    }
}

bool TextEditModel::hasModifications() const
{
    return std::any_of(spans_.begin(), spans_.end(),
                       [](const TextSpan& s) { return s.isModified(); });
}

std::vector<int> TextEditModel::modifiedSpanIndices() const
{
    std::vector<int> result;
    for (int i = 0; i < static_cast<int>(spans_.size()); ++i) {
        if (spans_[i].isModified()) {
            result.push_back(i);
        }
    }
    return result;
}

void TextEditModel::applyFormatting(int spanIndex, int startChar, int endChar,
                                     const FormattingRun& formatting)
{
    if (spanIndex < 0 || spanIndex >= static_cast<int>(spans_.size()))
        return;

    auto& span = spans_[spanIndex];
    int len = static_cast<int>(span.text.size());

    // Clamp to valid range
    startChar = std::clamp(startChar, 0, len);
    endChar = std::clamp(endChar, startChar, len);

    if (startChar >= endChar)
        return;

    // Create the new run with the clamped range
    FormattingRun newRun = formatting;
    newRun.charStart = startChar;
    newRun.charEnd = endChar;

    auto& runs = span.formattingRuns;

    // Process existing runs: remove fully contained, trim partially overlapping, split fully containing
    for (auto it = runs.begin(); it != runs.end(); ) {
        auto& run = *it;

        // Case 1: Run is fully contained in [startChar, endChar) - remove it
        if (run.charStart >= startChar && run.charEnd <= endChar) {
            it = runs.erase(it);
            continue;
        }

        // Case 2: Run starts before startChar and ends within [startChar, endChar) - trim end
        if (run.charStart < startChar && run.charEnd > startChar && run.charEnd <= endChar) {
            run.charEnd = startChar;
            ++it;
            continue;
        }

        // Case 3: Run starts within [startChar, endChar) and ends after endChar - trim start
        if (run.charStart >= startChar && run.charStart < endChar && run.charEnd > endChar) {
            run.charStart = endChar;
            ++it;
            continue;
        }

        // Case 4: Run fully contains [startChar, endChar) - split into two
        if (run.charStart < startChar && run.charEnd > endChar) {
            FormattingRun afterRun = run;
            afterRun.charStart = endChar;
            // Keep original run but trim its end
            run.charEnd = startChar;
            ++it;
            // Insert the "after" part
            it = runs.insert(it, afterRun);
            ++it;
            continue;
        }

        ++it;
    }

    // Insert the new run
    runs.push_back(newRun);

    // Sort by charStart
    std::sort(runs.begin(), runs.end(),
              [](const FormattingRun& a, const FormattingRun& b) {
                  return a.charStart < b.charStart;
              });

    // Merge adjacent runs with identical formatting
    for (auto it = runs.begin(); it != runs.end() && it + 1 != runs.end(); ) {
        auto& current = *it;
        auto& next = *(it + 1);

        if (current.charEnd == next.charStart && current.sameFormatAs(next)) {
            current.charEnd = next.charEnd;
            runs.erase(it + 1);
        } else {
            ++it;
        }
    }
}

void TextEditModel::clearFormatting(int spanIndex, int startChar, int endChar)
{
    if (spanIndex < 0 || spanIndex >= static_cast<int>(spans_.size()))
        return;

    auto& span = spans_[spanIndex];
    int len = static_cast<int>(span.text.size());

    // Clamp to valid range
    startChar = std::clamp(startChar, 0, len);
    endChar = std::clamp(endChar, startChar, len);

    if (startChar >= endChar)
        return;

    auto& runs = span.formattingRuns;

    // Process existing runs: remove fully contained, trim partially overlapping, split fully containing
    for (auto it = runs.begin(); it != runs.end(); ) {
        auto& run = *it;

        // Case 1: Run is fully contained in [startChar, endChar) - remove it
        if (run.charStart >= startChar && run.charEnd <= endChar) {
            it = runs.erase(it);
            continue;
        }

        // Case 2: Run starts before startChar and ends within [startChar, endChar) - trim end
        if (run.charStart < startChar && run.charEnd > startChar && run.charEnd <= endChar) {
            run.charEnd = startChar;
            ++it;
            continue;
        }

        // Case 3: Run starts within [startChar, endChar) and ends after endChar - trim start
        if (run.charStart >= startChar && run.charStart < endChar && run.charEnd > endChar) {
            run.charStart = endChar;
            ++it;
            continue;
        }

        // Case 4: Run fully contains [startChar, endChar) - split into two
        if (run.charStart < startChar && run.charEnd > endChar) {
            FormattingRun afterRun = run;
            afterRun.charStart = endChar;
            // Keep original run but trim its end
            run.charEnd = startChar;
            ++it;
            // Insert the "after" part
            it = runs.insert(it, afterRun);
            ++it;
            continue;
        }

        ++it;
    }
}

FormattingRun TextEditModel::formattingAtChar(int spanIndex, int charIndex) const
{
    if (spanIndex < 0 || spanIndex >= static_cast<int>(spans_.size())) {
        return FormattingRun{}; // Return default (charStart=0, charEnd=0)
    }

    const auto& span = spans_[spanIndex];

    // Find the run containing this character
    auto it = std::find_if(span.formattingRuns.begin(),
                           span.formattingRuns.end(),
        [charIndex](const auto& run) {
            return charIndex >= run.charStart && charIndex < run.charEnd;
        });
    if (it != span.formattingRuns.end()) {
        return *it;
    }

    // No run found - return default (indicates span defaults should be used)
    return FormattingRun{};
}

} // namespace pdfe::textedit
