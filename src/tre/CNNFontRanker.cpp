#include "core/TempFileSafety.h"
#include "tre/CNNFontRanker.h"
#include "tre/CNNFontClassifier.h"
#include "tre/BitmapCompositor.h"
#include "tre/TreTypes.h"

#include <QColor>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QPainter>
#include <QStandardPaths>
#include <QTextStream>
#include <algorithm>
#include <memory>
#include <numeric>

namespace pdfe::tre {

CNNFontRanker::CNNFontRanker(CNNFontClassifier* classifier)
    : classifier_(classifier) {}

std::vector<VisualFontRanker::RankedCandidate> CNNFontRanker::rankByVisualSimilarity(
    const QImage& scannedRegion,
    const std::vector<pdfe::fonts::SubstituteResult>& candidates,
    const std::string& diagnosticText,
    float fontSizePt,
    const RgbColor& textColor,
    BitmapCompositor* compositor,
    int dpi) const
{
    // CNN must be available — matchRegion() handles fallback routing
    if (!classifier_ || !classifier_->isAvailable()) {
        qDebug() << "[CNNFontRanker] CNN model unavailable";
        return {};
    }

    if (candidates.empty() || scannedRegion.isNull() || diagnosticText.empty())
        return {};

    // Convert diagnostic text to UTF-32
    const std::u32string u32Text = utf8ToU32(diagnosticText);
    if (u32Text.empty())
        return {};

    // Render candidates as TIGHT single-line crops at scan DPI, so both the
    // scanned crop and candidate renders go through the same preprocess() path:
    //   wide+short crop → tile vertically → scale to 224x224 → embedding
    //
    // Previously, candidates were rendered on a 224x224 canvas at 72 DPI,
    // producing images with ~5% text and 95% whitespace. preprocess() saw
    // aspect ratio 1:1 and skipped tiling. The scanned crop (e.g. 629x36)
    // triggered tiling and filled the entire 224x224 canvas. This domain gap
    // caused visual scores of 0.09-0.17 (model trained on tiled images).
    //
    // Now: render candidate text on a tight canvas (width=generous, height=
    // scan crop height) at scan DPI, auto-crop trailing whitespace, then let
    // preprocess() tile it identically to the scanned crop.
    const int candidateDpi = dpi;
    const float candidateFontSizePt = fontSizePt;  // actual detected size, no clamping

    // Tight canvas dimensions: match the scanned crop height so glyph scaling
    // is consistent. Width is generous to avoid text truncation.
    const int tightH = scannedRegion.height();
    const int tightW = std::max(scannedRegion.width(), tightH * 6);

    // Use black text for all candidate renders -- training data is always black on white.
    const RgbColor kBlack{0, 0, 0};

    // Contrast-normalize the scanned crop before embedding extraction.
    // Scanned text is typically gray (~150) on off-white (~230) background,
    // while training data and candidate renders are black (0) on white (255).
    // Stretching the contrast to full [0, 255] range bridges this domain gap.
    QImage normalizedScan = scannedRegion.convertToFormat(QImage::Format_Grayscale8);
    {
        int minVal = 255, maxVal = 0;
        for (int y = 0; y < normalizedScan.height(); ++y) {
            const uchar* row = normalizedScan.constScanLine(y);
            for (int x = 0; x < normalizedScan.width(); ++x) {
                minVal = std::min(minVal, static_cast<int>(row[x]));
                maxVal = std::max(maxVal, static_cast<int>(row[x]));
            }
        }
        if (maxVal > minVal + 30) {  // avoid amplifying noise on near-uniform images
            const float scale = 255.0f / (maxVal - minVal);
            for (int y = 0; y < normalizedScan.height(); ++y) {
                uchar* row = normalizedScan.scanLine(y);
                for (int x = 0; x < normalizedScan.width(); ++x) {
                    row[x] = static_cast<uchar>(std::clamp(
                        (static_cast<int>(row[x]) - minVal) * scale, 0.0f, 255.0f));
                }
            }
        }
    }

    // Extract scanned region embedding ONCE (reuse for all candidates)
    auto scannedEmbedding = classifier_->extractEmbedding(normalizedScan);
    if (scannedEmbedding.empty()) {
        qDebug() << "[CNNFontRanker] Failed to extract scanned region embedding";
        return {};
    }

    qDebug() << "[CNNFontRanker] candidateFontSizePt=" << candidateFontSizePt
             << "candidateDpi=" << candidateDpi
             << "tightCanvas=" << tightW << "x" << tightH
             << "scannedCropSize=" << scannedRegion.width() << "x" << scannedRegion.height();

    // Find max metric score for normalization
    float maxMetric = 0.0f;
    for (const auto& c : candidates) {
        if (c.matchScore > maxMetric)
            maxMetric = c.matchScore;
    }
    if (maxMetric < 1e-6f)
        maxMetric = 1.0f;

    // Use provided compositor or create a local fallback
    std::unique_ptr<BitmapCompositor> localCompositor;
    if (!compositor) {
        localCompositor = std::make_unique<BitmapCompositor>();
        compositor = localCompositor.get();
    }

    // Debug output directory
    // WIN-87 wider class (phase-60): diagnostics live under the per-process
    // unpredictable root, not at a fixed %TEMP% name an attacker can pre-create.
    // An empty path means diagnostics are unavailable -- skip the write.
    const QString debugDir = QString::fromStdWString(
        pdfe::core::diagnosticsSubdir("pdfe_cnn_debug").wstring());
    // NOT an early return: an unavailable diagnostics directory must skip the
    // DUMPS, never the ranking the caller asked for. `diagnosticsAvailable`
    // gates the writes below and nothing else.
    const bool diagnosticsAvailable = !debugDir.isEmpty();
    if (diagnosticsAvailable) {
        scannedRegion.save(debugDir + "/scanned_crop.png");
        normalizedScan.save(debugDir + "/scanned_crop_normalized.png");
    }

    // Render each candidate and compare embeddings
    std::vector<VisualFontRanker::RankedCandidate> ranked;
    ranked.reserve(candidates.size());
    for (size_t idx = 0; idx < candidates.size(); ++idx) {
        const auto& c = candidates[idx];
        VisualFontRanker::RankedCandidate rc;
        rc.candidate = c;
        rc.metricScore = c.matchScore;

        // Render as tight single-line crop at scan DPI, then auto-crop and
        // composite onto white background. The tight crop triggers the same
        // tiling path in preprocess() as the scanned crop, eliminating the
        // domain gap between dense (scanned) and sparse (224x224 canvas) images.
        QImage rendered = compositor->renderText(
            u32Text, c.filePath, candidateFontSizePt, kBlack,
            tightW, tightH, candidateDpi);

        if (!rendered.isNull()) {
            // Composite onto white background (transparent → black in grayscale)
            QImage whiteBg(rendered.size(), QImage::Format_ARGB32_Premultiplied);
            whiteBg.fill(QColor(255, 255, 255, 255));
            QPainter painter(&whiteBg);
            painter.drawImage(0, 0, rendered);
            painter.end();

            // Auto-crop trailing whitespace so the candidate has similar
            // text density to the scanned crop after tiling
            QImage gray = whiteBg.convertToFormat(QImage::Format_Grayscale8);
            int rightEdge = 0;  // 0 means no ink found
            for (int x = gray.width() - 1; x >= 0; --x) {
                bool hasInk = false;
                for (int y = 0; y < gray.height(); ++y) {
                    if (*(gray.constScanLine(y) + x) < 240) {
                        hasInk = true;
                        break;
                    }
                }
                if (hasInk) {
                    rightEdge = std::min(x + 8, gray.width());  // 8px margin
                    break;
                }
            }

            // Skip blank renders (font can't render the requested text —
            // e.g., Arabic font asked to render Latin characters)
            if (rightEdge < 4) {
                rc.visualScore = 0.0f;
                qDebug() << "[CNNFontRanker] blank render for"
                         << QString::fromStdString(c.profile.familyName);
            } else {
                rendered = whiteBg.copy(0, 0, rightEdge, whiteBg.height());

                auto renderedEmbedding = classifier_->extractEmbedding(rendered);
                rc.visualScore = CNNFontClassifier::cosineSimilarity(
                    scannedEmbedding, renderedEmbedding);
            }
            // Save top few rendered candidates for diagnosis
            if (idx < 5 || rc.visualScore > 0.5f) {
                if (diagnosticsAvailable) {
                    rendered.save(
                        debugDir
                        + QString("/candidate_%1_%2.png")
                              .arg(idx, 2, 10, QChar('0'))
                              .arg(QString::fromStdString(c.profile.familyName)
                                       .replace(' ', '_')));
                }
            }
        } else {
            rc.visualScore = 0.0f;
        }

        // combinedScore is recomputed after all candidates are scored,
        // using adaptive weighting based on the best visual score.
        rc.combinedScore = 0.0f;  // placeholder

        qDebug() << "[CNNFontRanker] candidate"
                 << QString::fromStdString(c.profile.familyName)
                 << "visual=" << rc.visualScore
                 << "combined=" << rc.combinedScore;

        ranked.push_back(std::move(rc));
    }

    // Adaptive weighting: if the CNN's best visual score is low,
    // the model isn't discriminating well (domain gap between scan and
    // clean renders). In that case, rely more on metric scoring which
    // is based on structural font properties (serif, weight, proportions).
    const float bestVisual = std::accumulate(ranked.begin(), ranked.end(), 0.0f,
        [](float acc, const auto& r) { return std::max(acc, r.visualScore); });

    // When bestVisual >= 0.5: CNN is confident → 70% visual, 30% metric
    // When bestVisual <  0.3: CNN is guessing  → 20% visual, 80% metric
    // Linear interpolation between these endpoints
    float t = std::clamp((bestVisual - 0.3f) / 0.2f, 0.0f, 1.0f);
    float visualWeight = 0.2f + 0.5f * t;   // 0.2 → 0.7
    float metricWeight = 1.0f - visualWeight; // 0.8 → 0.3

    qDebug() << "[CNNFontRanker] bestVisual=" << bestVisual
             << "visualWeight=" << visualWeight
             << "metricWeight=" << metricWeight;

    // Recompute combined scores with adaptive weights
    for (auto& r : ranked) {
        float metricNorm = r.metricScore / maxMetric;
        r.combinedScore = visualWeight * (1.0f - r.visualScore)
                        + metricWeight * metricNorm;
    }

    // Sort by combined score ascending (lower = better)
    std::sort(ranked.begin(), ranked.end(),
        [](const VisualFontRanker::RankedCandidate& a,
           const VisualFontRanker::RankedCandidate& b) {
            return a.combinedScore < b.combinedScore;
        });

    // Write ranking log to file
    {
        QFile logFile(diagnosticsAvailable ? debugDir + "/ranking_log.txt"
                                           : QString());
        if (logFile.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
            QTextStream ts(&logFile);
            ts << "CNN Font Ranking Debug Log\n";
            ts << "=========================\n";
            ts << "Text: " << QString::fromStdString(diagnosticText) << "\n";
            ts << "FontSizePt (detected): " << fontSizePt << "\n";
            ts << "FontSizePt (candidate render): " << candidateFontSizePt << "\n";
            ts << "ScannedCrop: " << scannedRegion.width() << "x" << scannedRegion.height() << "\n";
            ts << "CandidateRender: tight-crop (canvas " << tightW << "x" << tightH << ") @ " << candidateDpi << "DPI\n";
            ts << "Candidates: " << ranked.size() << "\n";
            ts << "Scanned embedding dim: " << scannedEmbedding.size() << "\n\n";
            ts << "Rank  Visual   Combined  Metric   Family\n";
            ts << "----  ------   --------  ------   ------\n";
            for (size_t i = 0; i < ranked.size(); ++i) {
                ts << QString("%1").arg(i + 1, 4)
                   << "  " << QString::number(ranked[i].visualScore, 'f', 4)
                   << "   " << QString::number(ranked[i].combinedScore, 'f', 4)
                   << "    " << QString::number(ranked[i].metricScore, 'f', 1)
                   << "   " << QString::fromStdString(ranked[i].candidate.profile.familyName)
                   << " (" << QString::fromStdString(ranked[i].candidate.profile.styleName) << ")"
                   << "\n";
            }
            // Save winner rendered image
            if (!ranked.empty()) {
                ts << "\nWinner: " << QString::fromStdString(ranked[0].candidate.profile.familyName) << "\n";
            }
        }
    }

    return ranked;
}

} // namespace pdfe::tre
