#include "tre/OcrPipeline.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"

#include <QImage>
#include <QRectF>

#include <tesseract/baseapi.h>
#include <leptonica/allheaders.h>

#include <algorithm>
#include <mutex>
#include <stdexcept>
#include <unordered_map>

namespace pdfe::tre {

// ---------------------------------------------------------------------------
// OcrPipeline::Impl
// ---------------------------------------------------------------------------

class OcrPipeline::Impl {
public:
    ~Impl() {
        if (tessApi_) {
            tessApi_->End();
        }
    }

    /// Initialize Tesseract for a specific language (defect #3: the language is
    /// no longer pinned to config.language — it is the per-call effective
    /// language). Returns false on failure (sets errorMsg).
    bool initTesseract(const OcrConfig& config, const std::string& language,
                       std::string& errorMsg) {
        tessApi_ = std::make_unique<tesseract::TessBaseAPI>();

        const char* datapath = config.tessdataPath.empty()
            ? nullptr
            : config.tessdataPath.c_str();

        int rc = tessApi_->Init(datapath, language.c_str(),
                                tesseract::OEM_LSTM_ONLY);
        if (rc != 0) {
            errorMsg = "Tesseract initialization failed (rc=" +
                       std::to_string(rc) + ") for language '" +
                       language + "'";
            tessApi_.reset();
            return false;
        }

        // Set page segmentation mode
        tessApi_->SetPageSegMode(
            static_cast<tesseract::PageSegMode>(config.pageSegMode));

        // Request HOCR output with word-level (or character-level) bounding boxes
        tessApi_->SetVariable("hocr_char_boxes", config.enableCharBoxes ? "1" : "0");

        tessInitialized_ = true;
        currentLanguage_ = language;
        return true;
    }

    /// Run Tesseract OSD on a grayscale page image to detect its dominant
    /// script, returning a Tesseract language code or @p fallback when OSD is
    /// unavailable (e.g. osd.traineddata missing). Uses a transient OSD-only
    /// API so it does not disturb the recognition engine state.
    std::string detectScriptLanguage(const QImage& gray,
                                     const std::string& tessdataPath,
                                     const std::string& fallback,
                                     float minConfidence = 0.0f) {
        try {
            tesseract::TessBaseAPI osd;
            const char* datapath = tessdataPath.empty() ? nullptr
                                                        : tessdataPath.c_str();
            if (osd.Init(datapath, "osd", tesseract::OEM_TESSERACT_ONLY) != 0) {
                return fallback;
            }
            osd.SetPageSegMode(tesseract::PSM_OSD_ONLY);
            osd.SetImage(gray.constBits(), gray.width(), gray.height(), 1,
                         static_cast<int>(gray.bytesPerLine()));
            int orientDeg = 0;
            float orientConf = 0.0f;
            const char* scriptName = nullptr;
            float scriptConf = 0.0f;
            const bool ok = osd.DetectOrientationScript(
                &orientDeg, &orientConf, &scriptName, &scriptConf);
            osd.End();
            if (!ok || !scriptName || scriptConf < minConfidence) {
                return fallback;
            }
            std::string lang = OcrPipeline::scriptToLanguage(scriptName);
            return lang.empty() ? fallback : lang;
        } catch (...) {
            return fallback;
        }
    }

    std::unique_ptr<tesseract::TessBaseAPI> tessApi_;
    bool tessInitialized_ = false;
    std::string currentLanguage_;        ///< Language the live tessApi_ is initialized for
    std::string lastLanguage_;           ///< Effective language of the most recent OCR call
    mutable std::mutex mutex_;
    // Per-(page, language) caches (defect #3): a page may be OCR'd in more than
    // one language across its lifetime.
    std::unordered_map<int, std::unordered_map<std::string, std::string>> hocrCache_;
    std::unordered_map<int, std::unordered_map<std::string, std::string>> charBoxHocrCache_;
};

// ---------------------------------------------------------------------------
// OcrPipeline public interface
// ---------------------------------------------------------------------------

OcrPipeline::OcrPipeline(pdfe::core::MuPdfContext* context,
                         pdfe::core::Document* document,
                         OcrConfig config)
    : context_(context)
    , document_(document)
    , config_(std::move(config))
    , pImpl_(std::make_unique<Impl>())
{
}

OcrPipeline::~OcrPipeline() = default;

OcrPipeline::OcrPipeline(OcrPipeline&&) noexcept = default;
OcrPipeline& OcrPipeline::operator=(OcrPipeline&&) noexcept = default;

std::string OcrPipeline::ocrPage(int pageIndex, OcrProgressCallback progress,
                                const std::string& language)
{
    std::lock_guard<std::mutex> lock(pImpl_->mutex_);

    // Resolve the effective language (override / auto-detect / doc-wide fallback)
    // BEFORE the cache lookup so the cache is keyed on the real language used.
    const std::string lang = resolveLanguage(pageIndex, language);
    pImpl_->lastLanguage_ = lang;

    // Check the per-(page, language) cache first.
    auto pageIt = pImpl_->hocrCache_.find(pageIndex);
    if (pageIt != pImpl_->hocrCache_.end()) {
        auto langIt = pageIt->second.find(lang);
        if (langIt != pageIt->second.end()) {
            return langIt->second;
        }
    }

    if (progress) {
        progress(0, 3, "Initializing OCR engine...");
    }

    // Lazy init / re-init for this language.
    ensureTesseractInit(lang);

    if (progress) {
        progress(1, 3, "Rendering page image...");
    }

    // Render page to QImage at OCR DPI
    QImage pageImage = renderPage(pageIndex);
    if (pageImage.isNull()) {
        throw std::runtime_error("Failed to render page " +
                                 std::to_string(pageIndex) + " for OCR");
    }

    // Convert QImage to format Tesseract can consume (RGBA -> Grayscale helps accuracy)
    QImage grayImage = pageImage.convertToFormat(QImage::Format_Grayscale8);

    if (progress) {
        progress(2, 3, "Running OCR...");
    }

    // Feed image to Tesseract
    pImpl_->tessApi_->SetImage(
        grayImage.constBits(),
        grayImage.width(),
        grayImage.height(),
        1,  // bytes per pixel (grayscale)
        static_cast<int>(grayImage.bytesPerLine()));

    pImpl_->tessApi_->SetSourceResolution(config_.ocrDpi);

    // Get HOCR output
    char* hocrText = pImpl_->tessApi_->GetHOCRText(0);
    if (!hocrText) {
        throw std::runtime_error("Tesseract GetHOCRText returned null for page " +
                                 std::to_string(pageIndex));
    }

    std::string hocr(hocrText);
    delete[] hocrText;

    // Clear the Tesseract image to free internal buffers
    pImpl_->tessApi_->Clear();

    // Cache result under (page, language).
    pImpl_->hocrCache_[pageIndex][lang] = hocr;

    if (progress) {
        progress(3, 3, "OCR complete.");
    }

    return hocr;
}

std::string OcrPipeline::ocrPageWithCharBoxes(int pageIndex,
                                             const std::string& language)
{
    std::lock_guard<std::mutex> lock(pImpl_->mutex_);

    const std::string lang = resolveLanguage(pageIndex, language);
    pImpl_->lastLanguage_ = lang;

    // Check the per-(page, language) char-box cache first.
    auto pageIt = pImpl_->charBoxHocrCache_.find(pageIndex);
    if (pageIt != pImpl_->charBoxHocrCache_.end()) {
        auto langIt = pageIt->second.find(lang);
        if (langIt != pageIt->second.end()) {
            return langIt->second;
        }
    }

    // Lazy init / re-init for this language.
    ensureTesseractInit(lang);

    // Render page to QImage at OCR DPI
    QImage pageImage = renderPage(pageIndex);
    if (pageImage.isNull()) {
        throw std::runtime_error("Failed to render page " +
                                 std::to_string(pageIndex) + " for char-box OCR");
    }

    QImage grayImage = pageImage.convertToFormat(QImage::Format_Grayscale8);

    // Temporarily enable char boxes
    pImpl_->tessApi_->SetVariable("hocr_char_boxes", "1");

    pImpl_->tessApi_->SetImage(
        grayImage.constBits(),
        grayImage.width(),
        grayImage.height(),
        1,
        static_cast<int>(grayImage.bytesPerLine()));

    pImpl_->tessApi_->SetSourceResolution(config_.ocrDpi);

    char* hocrText = pImpl_->tessApi_->GetHOCRText(0);
    if (!hocrText) {
        // Restore setting before throwing
        pImpl_->tessApi_->SetVariable("hocr_char_boxes",
                                       config_.enableCharBoxes ? "1" : "0");
        throw std::runtime_error("Tesseract GetHOCRText returned null for page " +
                                 std::to_string(pageIndex) + " (char-box mode)");
    }

    std::string hocr(hocrText);
    delete[] hocrText;

    pImpl_->tessApi_->Clear();

    // Restore original setting
    pImpl_->tessApi_->SetVariable("hocr_char_boxes",
                                   config_.enableCharBoxes ? "1" : "0");

    // Cache result under (page, language).
    pImpl_->charBoxHocrCache_[pageIndex][lang] = hocr;

    return hocr;
}

bool OcrPipeline::isCached(int pageIndex) const
{
    std::lock_guard<std::mutex> lock(pImpl_->mutex_);
    auto it = pImpl_->hocrCache_.find(pageIndex);
    return it != pImpl_->hocrCache_.end() && !it->second.empty();
}

bool OcrPipeline::isCharBoxCached(int pageIndex) const
{
    std::lock_guard<std::mutex> lock(pImpl_->mutex_);
    auto it = pImpl_->charBoxHocrCache_.find(pageIndex);
    return it != pImpl_->charBoxHocrCache_.end() && !it->second.empty();
}

void OcrPipeline::invalidateCache(int pageIndex)
{
    std::lock_guard<std::mutex> lock(pImpl_->mutex_);
    pImpl_->hocrCache_.erase(pageIndex);
    pImpl_->charBoxHocrCache_.erase(pageIndex);
}

void OcrPipeline::invalidateAllCaches()
{
    std::lock_guard<std::mutex> lock(pImpl_->mutex_);
    pImpl_->hocrCache_.clear();
    pImpl_->charBoxHocrCache_.clear();
}

const OcrConfig& OcrPipeline::config() const
{
    return config_;
}

void OcrPipeline::setConfig(const OcrConfig& config)
{
    std::lock_guard<std::mutex> lock(pImpl_->mutex_);
    config_ = config;
    pImpl_->hocrCache_.clear();
    pImpl_->charBoxHocrCache_.clear();

    // Re-init Tesseract with new config on next use
    if (pImpl_->tessApi_) {
        pImpl_->tessApi_->End();
        pImpl_->tessApi_.reset();
    }
    pImpl_->tessInitialized_ = false;
}

QImage OcrPipeline::renderPage(int pageIndex)
{
    return renderPageAtDpi(pageIndex, config_.ocrDpi);
}

QImage OcrPipeline::renderPageAtDpi(int pageIndex, int dpi)
{
    if (!document_ || !context_) {
        return {};
    }

    auto pageObj = document_->page(pageIndex);
    if (!pageObj) {
        return {};
    }

    QRectF bbox = pageObj->boundingBox();
    if (bbox.isEmpty()) {
        return {};
    }

    // Fall back to configured ocrDpi when caller passes a non-positive
    // value. This keeps the legacy renderPage() behavior unchanged and
    // lets the image-edit path request a specific working DPI.
    int effectiveDpi = dpi > 0 ? dpi : config_.ocrDpi;

    // Scale factor: target DPI / PDF native DPI (72)
    float scale = static_cast<float>(effectiveDpi) / 72.0f;

    return pageObj->render(bbox, scale);
}

void OcrPipeline::ensureTesseractInit(const std::string& language)
{
    // Re-init only when the requested language differs from the loaded one.
    if (pImpl_->tessInitialized_ && pImpl_->currentLanguage_ == language) {
        return;
    }
    if (pImpl_->tessApi_) {
        pImpl_->tessApi_->End();
        pImpl_->tessApi_.reset();
        pImpl_->tessInitialized_ = false;
    }

    std::string errorMsg;
    if (!pImpl_->initTesseract(config_, language, errorMsg)) {
        throw std::runtime_error(errorMsg);
    }
}

std::string OcrPipeline::resolveLanguage(int pageIndex,
                                        const std::string& requested)
{
    // Empty -> doc-wide preference (backward-compatible default for the existing
    // call sites). Explicit code -> verbatim. "auto" -> OSD script detection.
    if (requested.empty()) {
        return config_.language;
    }
    if (requested != "auto") {
        return requested;
    }

    // Auto-detect: render the page and run Tesseract OSD. Falls back to the
    // doc-wide language when OSD data is unavailable.
    QImage pageImage = renderPage(pageIndex);
    if (pageImage.isNull()) {
        return config_.language;
    }
    QImage gray = pageImage.convertToFormat(QImage::Format_Grayscale8);
    return pImpl_->detectScriptLanguage(gray, config_.tessdataPath,
                                        config_.language);
}

std::string OcrPipeline::lastOcrLanguage() const
{
    std::lock_guard<std::mutex> lock(pImpl_->mutex_);
    return pImpl_->lastLanguage_;
}

std::string OcrPipeline::autoDetectLanguage(int pageIndex)
{
    std::lock_guard<std::mutex> lock(pImpl_->mutex_);
    QImage pageImage = renderPage(pageIndex);
    if (pageImage.isNull()) {
        return {};
    }
    QImage gray = pageImage.convertToFormat(QImage::Format_Grayscale8);
    // Empty fallback: undetected -> "" so the caller can decide how to combine.
    return pImpl_->detectScriptLanguage(gray, config_.tessdataPath, "");
}

std::string OcrPipeline::detectPageLanguages(int pageIndex,
                                            const std::string& base)
{
    std::lock_guard<std::mutex> lock(pImpl_->mutex_);
    QImage pageImage = renderPage(pageIndex);
    if (pageImage.isNull()) {
        return base;
    }
    QImage gray = pageImage.convertToFormat(QImage::Format_Grayscale8);

    // OSD over a grid of tiles. The whole-page OSD returns only the MAJORITY
    // script (a Latin body hides an Arabic block — the defect-#3 anti-pattern);
    // tiling detects each region's script independently. A confidence floor
    // rejects blank / sparse tiles. base (doc-wide preference) is always kept;
    // only ADDITIONAL scripts found in some region are appended.
    constexpr int kRows = 4;
    constexpr int kCols = 2;
    constexpr float kMinScriptConfidence = 4.0f;  // Arabic block measured ~40; blanks << 4

    const int w = gray.width();
    const int h = gray.height();
    if (w <= 0 || h <= 0) {
        return base;
    }

    // Ordered: base first, then additional scripts in first-seen order, de-duped.
    std::vector<std::string> langs;
    auto addLang = [&langs](const std::string& l) {
        if (l.empty()) return;
        if (std::find(langs.begin(), langs.end(), l) == langs.end()) {
            langs.push_back(l);
        }
    };
    addLang(base);

    for (int r = 0; r < kRows; ++r) {
        for (int c = 0; c < kCols; ++c) {
            const int x0 = c * w / kCols;
            const int y0 = r * h / kRows;
            const int tw = (c + 1) * w / kCols - x0;
            const int th = (r + 1) * h / kRows - y0;
            if (tw <= 1 || th <= 1) continue;
            QImage tile = gray.copy(x0, y0, tw, th);
            if (tile.isNull()) continue;
            std::string lang = pImpl_->detectScriptLanguage(
                tile, config_.tessdataPath, "", kMinScriptConfidence);
            addLang(lang);
        }
    }

    if (langs.empty()) {
        return base;
    }
    std::string combined = langs.front();
    for (size_t i = 1; i < langs.size(); ++i) {
        combined += "+" + langs[i];
    }
    return combined;
}

std::string OcrPipeline::scriptToLanguage(const std::string& scriptName)
{
    // Tesseract OSD script names -> language codes. Only scripts we ship or can
    // reasonably acquire traineddata for; unknown -> empty (caller falls back).
    if (scriptName == "Arabic")    return "ara";
    if (scriptName == "Latin")     return "eng";
    if (scriptName == "Han" || scriptName == "HanS" ||
        scriptName == "Hangul_Han") return "chi_sim";
    if (scriptName == "Cyrillic")  return "rus";
    if (scriptName == "Hebrew")    return "heb";
    if (scriptName == "Greek")     return "ell";
    if (scriptName == "Japanese")  return "jpn";
    if (scriptName == "Hangul" || scriptName == "Korean") return "kor";
    if (scriptName == "Devanagari") return "hin";
    if (scriptName == "Thai")      return "tha";
    return {};
}

std::string OcrPipeline::detectLanguageForText(const std::u32string& sample)
{
    // Count codepoints by major Unicode block. The dominant non-Latin script
    // wins; otherwise Latin ("eng"). Used to route per-region OCR language when
    // a region's text is already known (no OSD pass required).
    int arabic = 0, han = 0, cyrillic = 0, hebrew = 0, greek = 0, latin = 0;
    for (char32_t cp : sample) {
        if ((cp >= 0x0600 && cp <= 0x06FF) || (cp >= 0x0750 && cp <= 0x077F) ||
            (cp >= 0x08A0 && cp <= 0x08FF) || (cp >= 0xFB50 && cp <= 0xFDFF) ||
            (cp >= 0xFE70 && cp <= 0xFEFF)) {
            ++arabic;
        } else if ((cp >= 0x4E00 && cp <= 0x9FFF) ||
                   (cp >= 0x3400 && cp <= 0x4DBF)) {
            ++han;
        } else if (cp >= 0x0400 && cp <= 0x04FF) {
            ++cyrillic;
        } else if (cp >= 0x0590 && cp <= 0x05FF) {
            ++hebrew;
        } else if (cp >= 0x0370 && cp <= 0x03FF) {
            ++greek;
        } else if ((cp >= 0x0041 && cp <= 0x005A) ||
                   (cp >= 0x0061 && cp <= 0x007A) ||
                   (cp >= 0x00C0 && cp <= 0x024F)) {
            ++latin;
        }
    }
    const int nonLatinMax = std::max({arabic, han, cyrillic, hebrew, greek});
    if (nonLatinMax == 0 || latin >= nonLatinMax) {
        return "eng";
    }
    if (nonLatinMax == arabic)   return "ara";
    if (nonLatinMax == han)      return "chi_sim";
    if (nonLatinMax == cyrillic) return "rus";
    if (nonLatinMax == hebrew)   return "heb";
    if (nonLatinMax == greek)    return "ell";
    return "eng";
}

} // namespace pdfe::tre
