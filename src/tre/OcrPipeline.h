#pragma once

#include "tre/TreTypes.h"

#include <QImage>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace pdfe::core {
class MuPdfContext;
class Document;
} // namespace pdfe::core

namespace pdfe::tre {

/// Wraps the Tesseract OCR engine to extract HOCR text from rendered page images.
///
/// Each OcrPipeline instance is bound to a single Document. Page images are
/// rendered via MuPDF at configurable DPI, then fed to Tesseract for text
/// recognition. Results are cached per page and returned as raw HOCR XML.
///
/// Thread safety:
/// - Tesseract is NOT thread-safe. Each call to `ocrPage()` serializes through
///   an internal mutex. For background OCR, callers should use QThreadPool and
///   accept the serialization overhead (each page takes ~1-3 seconds).
class OcrPipeline {
public:
    /// Construct an OCR pipeline bound to a document.
    /// @param context  MuPDF context for rendering (caller retains ownership).
    /// @param document Document whose pages will be OCR'd (caller retains ownership).
    /// @param config   OCR configuration (language, DPI, tessdata path, etc.).
    OcrPipeline(pdfe::core::MuPdfContext* context,
                pdfe::core::Document* document,
                OcrConfig config = {});
    ~OcrPipeline();

    // Non-copyable, movable
    OcrPipeline(const OcrPipeline&) = delete;
    OcrPipeline& operator=(const OcrPipeline&) = delete;
    OcrPipeline(OcrPipeline&&) noexcept;
    OcrPipeline& operator=(OcrPipeline&&) noexcept;

    /// Run OCR on a page and return the HOCR XML output.
    /// Results are cached per (page, language); subsequent calls for the same
    /// pair return the cached HOCR without re-running Tesseract.
    ///
    /// @param pageIndex  Zero-based page index.
    /// @param progress   Optional progress callback.
    /// @param language   Per-call language override (defect #3). Empty = fall
    ///        back to the document-wide OcrConfig::language. The sentinel
    ///        "auto" runs script auto-detection (Tesseract OSD) for this page
    ///        and selects the matching language, falling back to the doc-wide
    ///        preference when OSD is unavailable. Pass an explicit Tesseract
    ///        code (e.g. "ara", "eng+fra") to force that language for this call
    ///        only, without disturbing the doc-wide config.
    /// @return HOCR XML string from Tesseract.
    /// @throws pdfe::core::PdfeException on rendering or OCR failure.
    std::string ocrPage(int pageIndex, OcrProgressCallback progress = {},
                        const std::string& language = "");

    /// Run OCR on a page with character-level bounding boxes enabled.
    /// Uses a separate cache from ocrPage(). Returns HOCR XML with ocrx_cinfo
    /// elements containing per-character bounding boxes and confidence.
    ///
    /// @param pageIndex  Zero-based page index.
    /// @param language   Per-call language override (defect #3); see ocrPage().
    /// @return HOCR XML string with character-level data.
    /// @throws pdfe::core::PdfeException on rendering or OCR failure.
    std::string ocrPageWithCharBoxes(int pageIndex,
                                     const std::string& language = "");

    /// The effective language used by the most recent ocrPage() /
    /// ocrPageWithCharBoxes() call (after override / auto-detect / fallback
    /// resolution). Exposed for testing the per-call routing (defect #3).
    [[nodiscard]] std::string lastOcrLanguage() const;

    /// Run Tesseract OSD on the page image and return the Tesseract language
    /// code for the detected dominant script (e.g. "ara", "eng"), or an empty
    /// string when OSD is unavailable or the script is unrecognized. Unlike the
    /// "auto" path of ocrPage(), this does NOT fall back to the doc-wide
    /// language — the empty return lets a caller decide how to combine the
    /// detected script with its base language (e.g. build "eng+ara" for a
    /// mixed-script page). Defect #3.
    [[nodiscard]] std::string autoDetectLanguage(int pageIndex);

    /// Per-region script detection for a whole page (defect #3). Runs OSD over a
    /// grid of page tiles and returns a combined Tesseract language string of
    /// @p base plus every additional script detected in any region above a
    /// confidence floor — e.g. "eng+ara" for a page whose body is Latin but
    /// which carries an Arabic block. This is the per-region replacement for the
    /// page-dominant single-script verdict (which only sees the majority
    /// script). @p base (the doc-wide preference) is always included.
    [[nodiscard]] std::string detectPageLanguages(int pageIndex,
                                                  const std::string& base);

    /// Map a Tesseract OSD script name (e.g. "Arabic", "Latin", "Han") to a
    /// Tesseract language code (e.g. "ara", "eng", "chi_sim"). Returns an empty
    /// string for an unrecognized script so the caller can fall back to the
    /// doc-wide preference.
    [[nodiscard]] static std::string scriptToLanguage(const std::string& scriptName);

    /// Heuristic script→language detection from a sample of Unicode codepoints
    /// (defect #3). Picks the language for the dominant non-Latin script in the
    /// sample (Arabic, Han, Cyrillic, Hebrew, Greek), else "eng" for
    /// Latin-dominant text. Used to route per-region OCR language without a
    /// full OSD pass when the region's text is already known.
    [[nodiscard]] static std::string detectLanguageForText(const std::u32string& sample);

    /// Check whether word-level OCR results are cached for a page.
    [[nodiscard]] bool isCached(int pageIndex) const;

    /// Check whether char-box-level OCR results are cached for a page.
    [[nodiscard]] bool isCharBoxCached(int pageIndex) const;

    /// Invalidate cached OCR results for a single page.
    void invalidateCache(int pageIndex);

    /// Invalidate all cached OCR results.
    void invalidateAllCaches();

    /// Get the current OCR configuration.
    [[nodiscard]] const OcrConfig& config() const;

    /// Update the OCR configuration. Invalidates all cached results.
    void setConfig(const OcrConfig& config);

    /// Render a page to a QImage at the configured OCR DPI.
    /// This is exposed for testing and for the bitmap compositor.
    QImage renderPage(int pageIndex);

    /// Render a page at an explicit DPI. Used by the image-edit path
    /// when the source image's native DPI differs from config.ocrDpi.
    /// Falls back to config.ocrDpi if dpi <= 0. See docs/image-pdf-risks.md (A3).
    QImage renderPageAtDpi(int pageIndex, int dpi);

private:
    /// Initialize (or re-initialize) the Tesseract API instance for the given
    /// language. Called lazily on each OCR request; re-inits Tesseract only when
    /// the requested language differs from the currently-loaded one (defect #3).
    void ensureTesseractInit(const std::string& language);

    /// Resolve a per-call language argument to the effective Tesseract language:
    /// empty -> config_.language; "auto" -> OSD script detection on the page
    /// image (falling back to config_.language); anything else -> verbatim.
    std::string resolveLanguage(int pageIndex, const std::string& requested);

    pdfe::core::MuPdfContext* context_ = nullptr;
    pdfe::core::Document* document_ = nullptr;
    OcrConfig config_;

    class Impl;
    std::unique_ptr<Impl> pImpl_;
};

} // namespace pdfe::tre
