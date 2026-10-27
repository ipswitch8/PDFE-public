#include "TextSelector.h"
#include "core/Page.h"
#include "core/MuPdfContext.h"
#include "core/TextBlock.h"
#include "core/TextLine.h"
#include "core/TextChar.h"
#include <algorithm>
#include <cmath>

namespace pdfe::annotations {

/// Internal character info for selection computation.
struct CharInfo {
    QRectF bbox;          ///< Character bounding box
    char32_t codepoint;   ///< Unicode codepoint
    int lineIndex;        ///< Which line this char belongs to
    int blockIndex;       ///< Which block this char belongs to
};

/// Internal line info for grouping selected characters.
struct LineInfo {
    QRectF bbox;          ///< Line bounding box
};

class TextSelector::Impl {
public:
    explicit Impl(std::shared_ptr<pdfe::core::MuPdfContext> ctx)
        : context(std::move(ctx)) {}

    std::shared_ptr<pdfe::core::MuPdfContext> context;
    pdfe::core::Page* page = nullptr;
    QPointF startPoint;
    QPointF currentPoint;
    bool selecting = false;

    // Extracted text data (cached on beginSelection)
    std::vector<CharInfo> allChars;
    std::vector<LineInfo> allLines;
    int pageIndex = -1;

    // Current selection result
    std::vector<QRectF> selectedQuads;
    QString selectedText;

    /// Extract all character data from the page and build index structures.
    void extractCharData();

    /// Compute which characters are selected based on start and current points.
    void computeSelection();

    /// Find the index of the character nearest to the given point.
    /// Returns -1 if no characters exist.
    int findCharAt(QPointF pt) const;

    /// Helper: compute center point of a rectangle.
    static QPointF center(const QRectF& rect) {
        return QPointF(rect.x() + rect.width() / 2.0, rect.y() + rect.height() / 2.0);
    }

    /// Helper: distance between two points.
    static qreal distance(const QPointF& a, const QPointF& b) {
        qreal dx = a.x() - b.x();
        qreal dy = a.y() - b.y();
        return std::sqrt(dx * dx + dy * dy);
    }
};

TextSelector::TextSelector(std::shared_ptr<pdfe::core::MuPdfContext> ctx)
    : pImpl(std::make_unique<Impl>(std::move(ctx)))
{
}

TextSelector::~TextSelector() = default;

TextSelector::TextSelector(TextSelector&&) noexcept = default;
TextSelector& TextSelector::operator=(TextSelector&&) noexcept = default;

void TextSelector::beginSelection(pdfe::core::Page* page, QPointF pdfPoint)
{
    if (!page) {
        return;
    }

    pImpl->page = page;
    pImpl->pageIndex = page->index();
    pImpl->startPoint = pdfPoint;
    pImpl->currentPoint = pdfPoint;
    pImpl->selecting = true;

    // Extract all text from the page
    pImpl->extractCharData();

    // Initial selection (might be empty)
    pImpl->computeSelection();
}

void TextSelector::updateSelection(QPointF pdfPoint)
{
    if (!pImpl->selecting) {
        return;
    }

    pImpl->currentPoint = pdfPoint;
    pImpl->computeSelection();
}

TextSelection TextSelector::endSelection()
{
    if (!pImpl->selecting) {
        return TextSelection{};
    }

    pImpl->selecting = false;

    TextSelection result;
    result.quads = pImpl->selectedQuads;
    result.selectedText = pImpl->selectedText;
    result.pageIndex = pImpl->pageIndex;

    // Reset state
    pImpl->selectedQuads.clear();
    pImpl->selectedText.clear();
    pImpl->allChars.clear();
    pImpl->allLines.clear();
    pImpl->page = nullptr;
    pImpl->pageIndex = -1;

    return result;
}

std::vector<QRectF> TextSelector::currentQuads() const
{
    return pImpl->selectedQuads;
}

bool TextSelector::isSelecting() const
{
    return pImpl->selecting;
}

void TextSelector::cancel()
{
    pImpl->selecting = false;
    pImpl->selectedQuads.clear();
    pImpl->selectedText.clear();
    pImpl->allChars.clear();
    pImpl->allLines.clear();
    pImpl->page = nullptr;
    pImpl->pageIndex = -1;
}

void TextSelector::Impl::extractCharData()
{
    allChars.clear();
    allLines.clear();

    if (!page) {
        return;
    }

    // Extract all text blocks from the page
    std::vector<pdfe::core::TextBlock> blocks = page->extractText();

    int lineIndex = 0;
    int blockIndex = 0;

    for (const auto& block : blocks) {
        for (const auto& line : block.lines) {
            // Store line info
            LineInfo lineInfo;
            lineInfo.bbox = line.boundingBox;
            allLines.push_back(lineInfo);

            // Store all characters in this line
            for (const auto& charData : line.chars) {
                CharInfo charInfo;
                charInfo.bbox = charData.quad;
                charInfo.codepoint = charData.codepoint;
                charInfo.lineIndex = lineIndex;
                charInfo.blockIndex = blockIndex;
                allChars.push_back(charInfo);
            }

            ++lineIndex;
        }
        ++blockIndex;
    }
}

void TextSelector::Impl::computeSelection()
{
    selectedQuads.clear();
    selectedText.clear();

    if (allChars.empty()) {
        return;
    }

    // Find start and end character indices
    int startIdx = findCharAt(startPoint);
    int endIdx = findCharAt(currentPoint);

    if (startIdx == -1 || endIdx == -1) {
        return;
    }

    // Ensure startIdx <= endIdx
    if (startIdx > endIdx) {
        std::swap(startIdx, endIdx);
    }

    // Group selected characters by line
    struct LineGroup {
        int lineIndex;
        std::vector<int> charIndices;
    };
    std::vector<LineGroup> lineGroups;

    for (int i = startIdx; i <= endIdx; ++i) {
        int lineIdx = allChars[i].lineIndex;

        // Find or create line group
        auto it = std::find_if(lineGroups.begin(), lineGroups.end(),
                               [lineIdx](const LineGroup& group) { return group.lineIndex == lineIdx; });

        if (it == lineGroups.end()) {
            lineGroups.push_back(LineGroup{lineIdx, {i}});
        } else {
            it->charIndices.push_back(i);
        }
    }

    // Sort line groups by line index (top to bottom on page)
    std::sort(lineGroups.begin(), lineGroups.end(),
              [](const LineGroup& a, const LineGroup& b) { return a.lineIndex < b.lineIndex; });

    // Compute bounding quad for each line and build text
    for (const auto& group : lineGroups) {
        if (group.charIndices.empty()) {
            continue;
        }

        // Compute tight bounding box for selected chars in this line
        QRectF lineBounds;
        bool first = true;

        for (int charIdx : group.charIndices) {
            const QRectF& bbox = allChars[charIdx].bbox;
            if (first) {
                lineBounds = bbox;
                first = false;
            } else {
                lineBounds = lineBounds.united(bbox);
            }
        }

        selectedQuads.push_back(lineBounds);

        // Append text from this line
        for (int charIdx : group.charIndices) {
            char32_t cp = allChars[charIdx].codepoint;
            // Convert codepoint to QString. QChar::fromUcs4 has an
            // auto return type that mangles differently between MSVC
            // cl.exe (used to build vcpkg's Qt6) and clang-cl, which
            // breaks linking. QString::fromUcs4 has a stable mangled
            // name across both compilers.
            if (cp < 0x10000) {
                selectedText.append(QChar(static_cast<char16_t>(cp)));
            } else {
                selectedText.append(QString::fromUcs4(&cp, 1));
            }
        }

        // Add newline after each line except the last
        if (&group != &lineGroups.back()) {
            selectedText.append('\n');
        }
    }
}

int TextSelector::Impl::findCharAt(QPointF pt) const
{
    if (allChars.empty()) {
        return -1;
    }

    // Find the character whose center is closest to the point
    int bestIdx = -1;
    qreal bestDist = std::numeric_limits<qreal>::max();

    for (size_t i = 0; i < allChars.size(); ++i) {
        QPointF charCenter = center(allChars[i].bbox);
        qreal dist = distance(pt, charCenter);

        if (dist < bestDist) {
            bestDist = dist;
            bestIdx = static_cast<int>(i);
        }
    }

    return bestIdx;
}

} // namespace pdfe::annotations
