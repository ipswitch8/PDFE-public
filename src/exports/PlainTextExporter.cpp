// -*- coding: utf-8 -*-
#include "PlainTextExporter.h"
#include "core/Document.h"
#include "core/Page.h"

#include <fstream>

namespace pdfe::exports {

constexpr const char PlainTextExporter::kUtf8Bom[];
constexpr const char PlainTextExporter::kPageSeparator[];

bool PlainTextExporter::write(pdfe::core::Document* doc,
                              const std::filesystem::path& outPath)
{
    if (!doc) return false;

    // R2-10: content extraction is gated on the document's copy/extract
    // permission, exactly like MainWindow::onCopy / onSelectAll. The batch /
    // programmatic export path must not become a permission bypass that writes
    // the full text of a copy-denied document to disk. Unencrypted documents
    // allow extraction (copyAllowed()==true); refuse (no file written) otherwise.
    if (!doc->copyAllowed()) return false;

    std::ofstream out(outPath, std::ios::binary | std::ios::trunc);
    if (!out) return false;

    out.write(kUtf8Bom, 3);
    const int n = doc->pageCount();
    for (int i = 0; i < n; ++i) {
        auto page = doc->page(i);
        if (!page) continue;
        const std::string text = page->extractPlainText();
        out.write(text.data(), text.size());
        if (i + 1 < n) {
            out.write(kPageSeparator, 3);
        }
    }
    return out.good();
}

} // namespace pdfe::exports
