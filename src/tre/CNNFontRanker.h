#pragma once

#include "tre/VisualFontRanker.h"

#include <QImage>
#include <string>
#include <vector>

namespace pdfe::tre {

class BitmapCompositor;
class CNNFontClassifier;

/// Re-ranks font candidates using CNN embedding similarity.
/// Drop-in replacement for VisualFontRanker with learned features
/// instead of binarized NCC. Falls back to VisualFontRanker if
/// the CNN model is not available.
class CNNFontRanker {
public:
    /// Construct with a shared CNNFontClassifier instance.
    /// Does NOT take ownership -- caller must keep classifier alive.
    explicit CNNFontRanker(CNNFontClassifier* classifier);

    /// Same signature as VisualFontRanker::rankByVisualSimilarity().
    /// Uses CNN embedding cosine similarity instead of binarized NCC.
    /// Falls back to VisualFontRanker (NCC) if classifier is unavailable.
    std::vector<VisualFontRanker::RankedCandidate> rankByVisualSimilarity(
        const QImage& scannedRegion,
        const std::vector<pdfe::fonts::SubstituteResult>& candidates,
        const std::string& diagnosticText,
        float fontSizePt,
        const RgbColor& textColor,
        BitmapCompositor* compositor = nullptr,
        int dpi = 300
    ) const;

private:
    CNNFontClassifier* classifier_;
};

} // namespace pdfe::tre
