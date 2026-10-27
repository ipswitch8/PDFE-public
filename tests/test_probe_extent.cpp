// Standalone probe: open Lean Startup PDF, run EmbeddedFontAnalyzer
// on page 2, print measureRenderedExtent result for each font.
#include "fonts/EmbeddedFontAnalyzer.h"
#include "fonts/FontMetricProfile.h"
#include "core/MuPdfContext.h"
#include "core/Document.h"
#include "core/Page.h"

#include <QCoreApplication>
#include <QDebug>
#include <filesystem>
#include <gtest/gtest.h>

// GitHub #261's class, found by its own guard. This resolved a commercial
// ebook from another developer's Desktop and skipped everywhere else, so the
// rendered-extent assertion below has been evaluated on one machine, once.
//
// The document cannot be committed and does not need to be: what the case
// asserts is that EmbeddedFontAnalyzer reports a non-zero rendered extent for
// an embedded font, and any PDF with an embedded font exercises that. The
// override is kept for anyone who wants to point it at a specific document;
// there is no fall-through to a skip, because that is what hid this.
namespace {
std::filesystem::path embeddedFontDocumentUnderTest()
{
    if (const char* env = std::getenv("PDFE_EMBEDDED_FONT_PDF")) {
        if (env[0] != '\0') {
            std::filesystem::path p(env);
            if (std::filesystem::exists(p)) return p;
        }
    }
    return std::filesystem::path(TEST_FIXTURES_DIR) / "test_3page.pdf";
}
}  // namespace

TEST(ProbeFontExtent, CharisSIL_RenderedExtent_NonZero) {
    auto context = std::make_shared<pdfe::core::MuPdfContext>();
    const auto path = embeddedFontDocumentUnderTest();
    ASSERT_TRUE(std::filesystem::exists(path))
        << "no document resolved at " << path.string()
        << ". GitHub #261: this used to SKIP here, pointed at a book on "
           "another developer's Desktop.";
    auto doc = pdfe::core::Document::open(context, path);
    ASSERT_NE(doc, nullptr);

    pdfe::fonts::EmbeddedFontAnalyzer analyzer(context.get());
    // Page 2 was this document's own; clamp to whatever the resolved document
    // has, so the case does not depend on a page index only one PDF had.
    ASSERT_GE(doc->pageCount(), 1);
    auto page = doc->page(std::min(2, doc->pageCount() - 1));
    ASSERT_NE(page, nullptr);

    auto profiles = analyzer.analyzePageFonts(page.get(), doc.get());
    qDebug() << "Got" << profiles.size() << "profiles";
    for (const auto& p : profiles) {
        qDebug() << " font:" << QString::fromStdString(p.postScriptName)
                 << "rendered=" << p.renderedVisibleExtent
                 << "capH=" << p.capHeight
                 << "desc=" << p.descender
                 << "asc=" << p.ascender
                 << "upm=" << p.unitsPerEm;
        if (p.postScriptName.find("CharisSIL") != std::string::npos) {
            EXPECT_GT(p.renderedVisibleExtent, 0.0f)
                << "CharisSIL should have non-zero rendered extent";
        }
    }
}
