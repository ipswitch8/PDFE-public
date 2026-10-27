#pragma once

#include <QImage>
#include <memory>
#include <string>
#include <vector>

namespace pdfe::tre {

/// CNN-based font feature extractor using ONNX Runtime.
/// Loads a ResNet18-based model that outputs 512-dim embeddings for font
/// similarity comparison. Used as a learned replacement for binarized NCC
/// in VisualFontRanker.
///
/// The model is loaded lazily on first use. If the model file is not found
/// or ONNX Runtime is not available at compile time, all methods return
/// empty/zero results and isAvailable() returns false.
class CNNFontClassifier {
public:
    /// Construct with path to the .onnx model file.
    /// Does NOT load the model immediately (lazy initialization on first use).
    explicit CNNFontClassifier(const std::string& modelPath);
    ~CNNFontClassifier();

    // Non-copyable, movable
    CNNFontClassifier(const CNNFontClassifier&) = delete;
    CNNFontClassifier& operator=(const CNNFontClassifier&) = delete;
    CNNFontClassifier(CNNFontClassifier&&) noexcept;
    CNNFontClassifier& operator=(CNNFontClassifier&&) noexcept;

    /// Whether the model file was found and loaded successfully.
    [[nodiscard]] bool isAvailable() const;

    /// Extract a 512-dimensional embedding vector from a text image.
    /// Preprocessing: grayscale, resize to 224x224, replicate to 3ch,
    /// normalize with ImageNet stats, NCHW layout.
    /// Returns empty vector if model not available.
    [[nodiscard]] std::vector<float> extractEmbedding(const QImage& textImage) const;

    /// Compute cosine similarity between two embedding vectors.
    /// Returns 0.0 if either vector is empty.
    [[nodiscard]] static float cosineSimilarity(
        const std::vector<float>& a, const std::vector<float>& b);

    /// Convenience: extract embeddings from both images and return cosine similarity.
    [[nodiscard]] float embeddingSimilarity(
        const QImage& imageA, const QImage& imageB) const;

private:
    class Impl;
    std::unique_ptr<Impl> pImpl_;
};

} // namespace pdfe::tre
