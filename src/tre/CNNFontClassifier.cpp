#include "tre/CNNFontClassifier.h"

#include <QDebug>
#include <QPainter>
#include <algorithm>
#include <cmath>
#include <mutex>
#include <numeric>

#ifdef PDFE_HAS_ONNXRUNTIME
#include <onnxruntime_cxx_api.h>
#include <filesystem>
#endif

namespace pdfe::tre {

// ImageNet normalization constants
static constexpr float kMeans[3] = {0.485f, 0.456f, 0.406f};
static constexpr float kStds[3]  = {0.229f, 0.224f, 0.225f};
static constexpr int kInputSize = 224;

// ---------------------------------------------------------------------------
// Pimpl
// ---------------------------------------------------------------------------
class CNNFontClassifier::Impl {
public:
    explicit Impl(std::string modelPath)
        : modelPath_(std::move(modelPath)) {}

    bool isAvailable() const {
        ensureLoaded();
        return loaded_;
    }

    std::vector<float> extractEmbedding(const QImage& textImage) const {
#ifdef PDFE_HAS_ONNXRUNTIME
        ensureLoaded();
        if (!loaded_ || !session_)
            return {};

        // Preprocess image to NCHW tensor
        auto tensor = preprocess(textImage);
        if (tensor.empty())
            return {};

        // Create input tensor
        const std::array<int64_t, 4> inputShape = {1, 3, kInputSize, kInputSize};
        auto memInfo = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        Ort::Value inputTensor = Ort::Value::CreateTensor<float>(
            memInfo, tensor.data(), tensor.size(),
            inputShape.data(), inputShape.size());

        // Run inference
        const char* inputNames[] = {"input"};
        const char* outputNames[] = {"embedding"};
        auto outputs = session_->Run(
            Ort::RunOptions{nullptr},
            inputNames, &inputTensor, 1,
            outputNames, 1);

        if (outputs.empty())
            return {};

        // Extract embedding vector
        const float* outputData = outputs[0].GetTensorData<float>();
        auto outputInfo = outputs[0].GetTensorTypeAndShapeInfo();
        const size_t outputSize = outputInfo.GetElementCount();

        return std::vector<float>(outputData, outputData + outputSize);
#else
        (void)textImage;
        return {};
#endif
    }

private:
    std::string modelPath_;

#ifdef PDFE_HAS_ONNXRUNTIME
    mutable std::unique_ptr<Ort::Session> session_;
    mutable std::once_flag loadFlag_;
    mutable bool loaded_ = false;

    void ensureLoaded() const {
        std::call_once(loadFlag_, [this] { doLoad(); });
    }

    void doLoad() const {
        if (modelPath_.empty()) {
            qDebug() << "[CNNFontClassifier] No model path specified";
            return;
        }

        if (!std::filesystem::exists(modelPath_)) {
            qDebug() << "[CNNFontClassifier] Model file not found:"
                     << QString::fromStdString(modelPath_);
            return;
        }

        try {
            // Static Ort::Env (thread-safe singleton per ONNX Runtime docs)
            static Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "pdfe_font");

            Ort::SessionOptions opts;
            opts.SetIntraOpNumThreads(2);
            opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

            // Windows requires wide path
#ifdef _WIN32
            auto widePath = std::filesystem::path(modelPath_).wstring();
            session_ = std::make_unique<Ort::Session>(env, widePath.c_str(), opts);
#else
            session_ = std::make_unique<Ort::Session>(env, modelPath_.c_str(), opts);
#endif
            loaded_ = true;
            qDebug() << "[CNNFontClassifier] Model loaded successfully from"
                     << QString::fromStdString(modelPath_);
        } catch (const Ort::Exception& e) {
            qWarning() << "[CNNFontClassifier] ONNX Runtime error:" << e.what();
        } catch (const std::exception& e) {
            qWarning() << "[CNNFontClassifier] Error loading model:" << e.what();
        }
    }

    static std::vector<float> preprocess(const QImage& img) {
        if (img.isNull())
            return {};

        // Convert to grayscale
        QImage gray = img.convertToFormat(QImage::Format_Grayscale8);
        if (gray.isNull() || gray.width() == 0 || gray.height() == 0)
            return {};

        // Scale so glyphs fill the canvas height, then crop/pad width.
        // A scanned crop (e.g. 629x36) scaled to fit produces a 224x13 sliver
        // with ~2% ink — far too sparse for the CNN.  Scaling to fill the
        // height instead gives 224-tall glyphs with natural proportions.
        // If the result is wider than 224, we crop from the left (first few
        // words are enough for shape comparison).  If narrower, pad with white.
        float scale = static_cast<float>(kInputSize) / gray.height();
        int scaledW = std::max(1, static_cast<int>(gray.width() * scale));
        int scaledH = kInputSize;

        QImage scaled = gray.scaled(scaledW, scaledH,
                                    Qt::IgnoreAspectRatio,
                                    Qt::SmoothTransformation);

        QImage letterboxed(kInputSize, kInputSize, QImage::Format_Grayscale8);
        letterboxed.fill(255);  // white padding

        // Left-align; crop if wider than 224, pad if narrower
        int srcW = std::min(scaledW, kInputSize);
        QPainter painter(&letterboxed);
        painter.drawImage(0, 0, scaled.copy(0, 0, srcW, scaledH));
        painter.end();

        // Pack into NCHW float tensor [1, 3, 224, 224]
        // Replicate grayscale to 3 channels, normalize with ImageNet stats
        std::vector<float> tensor(3 * kInputSize * kInputSize);
        for (int y = 0; y < kInputSize; ++y) {
            const uchar* row = letterboxed.constScanLine(y);
            for (int x = 0; x < kInputSize; ++x) {
                float pixel = static_cast<float>(row[x]) / 255.0f;
                for (int c = 0; c < 3; ++c) {
                    float normalized = (pixel - kMeans[c]) / kStds[c];
                    tensor[c * kInputSize * kInputSize + y * kInputSize + x] = normalized;
                }
            }
        }
        return tensor;
    }
#else
    // Stub when ONNX Runtime not available
    void ensureLoaded() const {}
    mutable bool loaded_ = false;
#endif
};

// ---------------------------------------------------------------------------
// CNNFontClassifier public API
// ---------------------------------------------------------------------------

CNNFontClassifier::CNNFontClassifier(const std::string& modelPath)
    : pImpl_(std::make_unique<Impl>(modelPath)) {}

CNNFontClassifier::~CNNFontClassifier() = default;

CNNFontClassifier::CNNFontClassifier(CNNFontClassifier&&) noexcept = default;
CNNFontClassifier& CNNFontClassifier::operator=(CNNFontClassifier&&) noexcept = default;

bool CNNFontClassifier::isAvailable() const {
    return pImpl_->isAvailable();
}

std::vector<float> CNNFontClassifier::extractEmbedding(const QImage& textImage) const {
    return pImpl_->extractEmbedding(textImage);
}

float CNNFontClassifier::cosineSimilarity(
    const std::vector<float>& a, const std::vector<float>& b) {
    if (a.empty() || b.empty() || a.size() != b.size())
        return 0.0f;

    float dot = 0.0f, normA = 0.0f, normB = 0.0f;
    for (size_t i = 0; i < a.size(); ++i) {
        dot += a[i] * b[i];
        normA += a[i] * a[i];
        normB += b[i] * b[i];
    }

    float denom = std::sqrt(normA) * std::sqrt(normB);
    if (denom < 1e-8f)
        return 0.0f;

    // Clamp to [0, 1] — negative cosine (anti-correlated) treated as zero
    // similarity for ranking purposes (we only care about "how similar")
    return std::max(0.0f, std::min(1.0f, dot / denom));
}

float CNNFontClassifier::embeddingSimilarity(
    const QImage& imageA, const QImage& imageB) const {
    auto embA = extractEmbedding(imageA);
    auto embB = extractEmbedding(imageB);
    return cosineSimilarity(embA, embB);
}

} // namespace pdfe::tre
