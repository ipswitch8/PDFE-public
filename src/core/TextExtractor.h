#pragma once

#include "TextBlock.h"
#include <memory>
#include <vector>
#include <string>

namespace pdfe::core {

class Page;
class MuPdfContext;

/// Extracts structured text from PDF pages using MuPDF's stext API.
///
/// Text is returned as a hierarchy: TextBlock → TextLine → TextChar,
/// preserving spatial layout information for search, selection, and accessibility.
class TextExtractor {
public:
    explicit TextExtractor(std::shared_ptr<MuPdfContext> context);
    ~TextExtractor();

    /// Extract structured text blocks from a page.
    /// Each block contains lines, and each line contains characters with
    /// position, font, and size information.
    std::vector<TextBlock> extract(const Page* page);

    /// Extract plain text from a page (convenience method).
    /// Blocks are separated by double newlines, lines by single newlines.
    std::string extractPlainText(const Page* page);

    /// Fast check: does this page contain any native text content?
    bool hasText(const Page* page);

    // Non-copyable
    TextExtractor(const TextExtractor&) = delete;
    TextExtractor& operator=(const TextExtractor&) = delete;

    // Movable
    TextExtractor(TextExtractor&&) noexcept;
    TextExtractor& operator=(TextExtractor&&) noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

} // namespace pdfe::core
