#pragma once

#include "DiffTypes.h"
#include <memory>
#include <filesystem>

namespace pdfe::core {
class MuPdfContext;
class Document;
}

namespace pdfe::diff {

class DiffEngine {
public:
    explicit DiffEngine(std::shared_ptr<pdfe::core::MuPdfContext> context);
    ~DiffEngine();

    DiffEngine(const DiffEngine&) = delete;
    DiffEngine& operator=(const DiffEngine&) = delete;

    void setRenderDpi(float dpi);
    void setDiffColor(int r, int g, int b);

    // GitHub #352: the DPI the engine rendered at. The display view needs
    // this to map the diff overlay (rendered at renderDpi_) back onto a page
    // rendered at a different (display) scale. Exposed so callers and tests
    // can derive the engine render scale = renderDpi() / 72.0.
    float renderDpi() const { return renderDpi_; }

    DiffResult compare(
        pdfe::core::Document* docA,
        pdfe::core::Document* docB
    );

    PageDiffResult comparePage(
        pdfe::core::Document* docA,
        pdfe::core::Document* docB,
        int pageIndex
    );

    static QImage computePixelDiff(const QImage& imageA, const QImage& imageB,
                                    int diffR, int diffG, int diffB);
    static double computeSimilarity(const QImage& imageA, const QImage& imageB);
    static std::vector<QRectF> findChangeRegions(const QImage& diffImage, int blockSize = 16);
    static std::vector<TextChange> diffText(const std::string& textA, const std::string& textB);

private:
    std::shared_ptr<pdfe::core::MuPdfContext> context_;
    float renderDpi_ = 150.0f;
    int diffR_ = 255, diffG_ = 0, diffB_ = 0;
};

} // namespace pdfe::diff
