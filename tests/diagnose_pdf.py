# -*- coding: utf-8 -*-
"""
Diagnostic script to understand the state of the user's test PDF.
Checks page classification, text content, and what the editor sees.
"""
import subprocess
import sys
import os
import tempfile

# We'll create a small C++ test program that opens the PDF and reports diagnostics.
# This avoids the need for Python PDF libraries.

DIAGNOSE_CODE = r"""
#include <iostream>
#include <filesystem>
#include <string>
#include <memory>
#include "core/MuPdfContext.h"
#include "core/Document.h"
#include "core/Page.h"
#include "textedit/TextEditModel.h"

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: diagnose_pdf <path.pdf>" << std::endl;
        return 1;
    }

    std::string pdfPath = argv[1];
    std::cout << "=== PDF Diagnostic Report ===" << std::endl;
    std::cout << "File: " << pdfPath << std::endl;
    std::cout << std::endl;

    auto context = std::make_shared<pdfe::core::MuPdfContext>();
    auto doc = pdfe::core::Document::open(context, std::filesystem::path(pdfPath));

    if (!doc) {
        std::cerr << "ERROR: Failed to open PDF" << std::endl;
        return 1;
    }

    int pageCount = doc->pageCount();
    std::cout << "Page count: " << pageCount << std::endl;
    std::cout << std::endl;

    for (int i = 0; i < pageCount && i < 5; ++i) {
        auto page = doc->page(i);
        if (!page) {
            std::cout << "Page " << i << ": FAILED TO LOAD" << std::endl;
            continue;
        }

        auto bbox = page->boundingBox();
        bool hasText = page->hasTextContent();
        bool isImageDom = page->isImageDominated();

        std::cout << "--- Page " << i << " ---" << std::endl;
        std::cout << "  Dimensions: " << bbox.width() << " x " << bbox.height() << " pt" << std::endl;
        std::cout << "  hasTextContent: " << (hasText ? "YES" : "NO") << std::endl;
        std::cout << "  isImageDominated: " << (isImageDom ? "YES" : "NO") << std::endl;

        // Extract text
        auto blocks = page->extractText();
        std::cout << "  Text blocks: " << blocks.size() << std::endl;

        pdfe::textedit::TextEditModel model(blocks);
        std::cout << "  Text spans: " << model.spanCount() << std::endl;

        // Show first few spans with font info
        for (int s = 0; s < model.spanCount() && s < 10; ++s) {
            const auto& span = model.span(s);
            // Convert u32 text to utf8 for printing
            std::string utf8;
            for (char32_t cp : span.text) {
                if (cp < 0x80) utf8.push_back(static_cast<char>(cp));
                else if (cp < 0x800) {
                    utf8.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                    utf8.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                } else {
                    utf8.push_back('?');
                }
            }
            // Truncate
            if (utf8.size() > 60) utf8 = utf8.substr(0, 57) + "...";

            std::cout << "    Span " << s
                      << ": font=\"" << span.fontName << "\""
                      << " size=" << span.fontSize
                      << " bbox=(" << span.boundingBox.left()
                      << "," << span.boundingBox.top()
                      << "," << span.boundingBox.right()
                      << "," << span.boundingBox.bottom() << ")"
                      << " text=\"" << utf8 << "\""
                      << std::endl;
        }
        std::cout << std::endl;
    }

    return 0;
}
"""

print("This diagnostic requires using the PDFE test binary directly.")
print("Writing diagnostic test and building it...")

# Actually, let's just add this as a gtest that we can run
print("\nInstead, let's run the existing test binary with a filter for diagnostics.")
print("We need to add a diagnostic test to the test suite.")
print("For now, the key question is: does the user's PDF report as isImageDominated?")
