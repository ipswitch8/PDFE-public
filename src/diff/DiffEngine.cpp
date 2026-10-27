#include "DiffEngine.h"
#include "core/MuPdfContext.h"
#include "core/Document.h"
#include "core/Page.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <sstream>

namespace pdfe::diff {

DiffEngine::DiffEngine(std::shared_ptr<pdfe::core::MuPdfContext> context)
    : context_(std::move(context))
{
}

DiffEngine::~DiffEngine() = default;

void DiffEngine::setRenderDpi(float dpi) { renderDpi_ = dpi; }
void DiffEngine::setDiffColor(int r, int g, int b) { diffR_ = r; diffG_ = g; diffB_ = b; }

DiffResult DiffEngine::compare(
    pdfe::core::Document* docA,
    pdfe::core::Document* docB)
{
    DiffResult result;
    if (!docA || !docB) {
        result.errorMessage = "Both documents must be provided";
        return result;
    }

    result.totalPages = std::max(docA->pageCount(), docB->pageCount());
    double totalSimilarity = 0.0;

    for (int i = 0; i < result.totalPages; ++i) {
        auto pageResult = comparePage(docA, docB, i);
        if (pageResult.similarityScore < 1.0) {
            result.changedPages++;
        }
        totalSimilarity += pageResult.similarityScore;
        result.pages.push_back(std::move(pageResult));
    }

    result.overallSimilarity = result.totalPages > 0
        ? totalSimilarity / result.totalPages : 1.0;
    return result;
}

PageDiffResult DiffEngine::comparePage(
    pdfe::core::Document* docA,
    pdfe::core::Document* docB,
    int pageIndex)
{
    PageDiffResult result;
    result.pageIndex = pageIndex;

    float scale = renderDpi_ / 72.0f;

    // Render both pages
    QImage imageA, imageB;
    if (pageIndex < docA->pageCount()) {
        auto pageA = docA->page(pageIndex);
        if (pageA) imageA = pageA->render(pageA->boundingBox(), scale);
    }
    if (pageIndex < docB->pageCount()) {
        auto pageB = docB->page(pageIndex);
        if (pageB) imageB = pageB->render(pageB->boundingBox(), scale);
    }

    // Handle missing pages
    if (imageA.isNull() && imageB.isNull()) {
        result.similarityScore = 1.0;
        return result;
    }
    if (imageA.isNull() || imageB.isNull()) {
        result.similarityScore = 0.0;
        result.diffImage = imageB.isNull() ? imageA : imageB;
        return result;
    }

    // Pixel diff + similarity in a single pixel pass (perf hot path).
    {
        QImage a = imageA.convertToFormat(QImage::Format_RGBA8888);
        QImage b = imageB.convertToFormat(QImage::Format_RGBA8888);

        // Fast path: byte-identical images (common for identical PDFs).
        if (a.size() == b.size() && a.sizeInBytes() == b.sizeInBytes()
            && std::memcmp(a.constBits(), b.constBits(),
                           static_cast<size_t>(a.sizeInBytes())) == 0) {
            QImage diffImg(a.width(), a.height(), QImage::Format_RGBA8888);
            diffImg.fill(Qt::transparent);
            result.diffImage = std::move(diffImg);
            result.similarityScore = 1.0;
            result.changeRegions.clear();
            // Still run text diff below.
            //
            // PERMISSION NOTE (a11y-bypass sweep): the text diff is deliberately
            // NOT gated. It is an in-process comparison of two documents the
            // user has already opened, rendered into the in-app diff view; it
            // emits no file, clipboard entry or accessibility text. See the note
            // in SearchEngine.cpp for the boundary being drawn.
            std::string textA, textB;
            if (pageIndex < docA->pageCount()) {
                auto pgA = docA->page(pageIndex);
                if (pgA) textA = pgA->extractPlainText();
            }
            if (pageIndex < docB->pageCount()) {
                auto pgB = docB->page(pageIndex);
                if (pgB) textB = pgB->extractPlainText();
            }
            result.textChanges = diffText(textA, textB);
            for (const auto& change : result.textChanges) {
                switch (change.type) {
                case ChangeType::Inserted: result.insertionCount++; break;
                case ChangeType::Deleted: result.deletionCount++; break;
                case ChangeType::Modified: result.modificationCount++; break;
                default: break;
                }
            }
            return result;
        }

        const int aw = a.width(), ah = a.height();
        const int bw = b.width(), bh = b.height();
        const int W = std::max(aw, bw);
        const int H = std::max(ah, bh);
        const int simW = std::min(aw, bw);
        const int simH = std::min(ah, bh);

        QImage diffImg(W, H, QImage::Format_RGBA8888);
        diffImg.fill(Qt::transparent);

        long long totalDiff = 0;
        const long long maxDiff = static_cast<long long>(simW) * simH * 255 * 3;

        for (int y = 0; y < H; ++y) {
            const uchar* aLine = (y < ah) ? a.constScanLine(y) : nullptr;
            const uchar* bLine = (y < bh) ? b.constScanLine(y) : nullptr;
            uchar* dLine = diffImg.scanLine(y);
            for (int x = 0; x < W; ++x) {
                int ar = 255, ag = 255, ab = 255;
                int br = 255, bg = 255, bb = 255;
                if (aLine && x < aw) {
                    const uchar* p = aLine + 4 * x;
                    ar = p[0]; ag = p[1]; ab = p[2];
                }
                if (bLine && x < bw) {
                    const uchar* p = bLine + 4 * x;
                    br = p[0]; bg = p[1]; bb = p[2];
                }
                const int dr = std::abs(ar - br);
                const int dg = std::abs(ag - bg);
                const int dbB = std::abs(ab - bb);
                const int sum = dr + dg + dbB;
                if (sum > 30) {
                    const int alpha = std::min(255, sum * 2);
                    uchar* dp = dLine + 4 * x;
                    dp[0] = static_cast<uchar>(diffR_);
                    dp[1] = static_cast<uchar>(diffG_);
                    dp[2] = static_cast<uchar>(diffB_);
                    dp[3] = static_cast<uchar>(alpha);
                }
                if (x < simW && y < simH) {
                    totalDiff += sum;
                }
            }
        }

        if (simW > 0 && simH > 0) {
            const int maxW = std::max(aw, bw);
            const int maxH = std::max(ah, bh);
            const double areaCoverage = static_cast<double>(simW * simH) / (maxW * maxH);
            const double pixelSimilarity = 1.0 - static_cast<double>(totalDiff) / maxDiff;
            result.similarityScore = pixelSimilarity * areaCoverage;
        } else {
            result.similarityScore = 0.0;
        }
        result.diffImage = std::move(diffImg);
    }
    result.changeRegions = findChangeRegions(result.diffImage);

    // Text diff — see the PERMISSION NOTE above: internal comparison, not an
    // outward materialisation of content, so not permission-gated.
    std::string textA, textB;
    if (pageIndex < docA->pageCount()) {
        auto pageA = docA->page(pageIndex);
        if (pageA) textA = pageA->extractPlainText();
    }
    if (pageIndex < docB->pageCount()) {
        auto pageB = docB->page(pageIndex);
        if (pageB) textB = pageB->extractPlainText();
    }

    result.textChanges = diffText(textA, textB);
    for (const auto& change : result.textChanges) {
        switch (change.type) {
        case ChangeType::Inserted: result.insertionCount++; break;
        case ChangeType::Deleted: result.deletionCount++; break;
        case ChangeType::Modified: result.modificationCount++; break;
        default: break;
        }
    }

    return result;
}

QImage DiffEngine::computePixelDiff(const QImage& imageA, const QImage& imageB,
                                      int diffR, int diffG, int diffB)
{
    int w = std::max(imageA.width(), imageB.width());
    int h = std::max(imageA.height(), imageB.height());

    QImage diffImg(w, h, QImage::Format_RGBA8888);
    diffImg.fill(Qt::transparent);

    QImage a = imageA.convertToFormat(QImage::Format_RGBA8888);
    QImage b = imageB.convertToFormat(QImage::Format_RGBA8888);
    const int aw = a.width(), ah = a.height();
    const int bw = b.width(), bh = b.height();

    for (int y = 0; y < h; ++y) {
        const uchar* aLine = (y < ah) ? a.constScanLine(y) : nullptr;
        const uchar* bLine = (y < bh) ? b.constScanLine(y) : nullptr;
        uchar* dLine = diffImg.scanLine(y);
        for (int x = 0; x < w; ++x) {
            int ar = 255, ag = 255, ab = 255;
            int br = 255, bg = 255, bb = 255;
            if (aLine && x < aw) {
                const uchar* p = aLine + 4 * x;
                ar = p[0]; ag = p[1]; ab = p[2];
            }
            if (bLine && x < bw) {
                const uchar* p = bLine + 4 * x;
                br = p[0]; bg = p[1]; bb = p[2];
            }
            int dr = std::abs(ar - br);
            int dg = std::abs(ag - bg);
            int db = std::abs(ab - bb);
            int sum = dr + dg + db;
            if (sum > 30) {
                int alpha = std::min(255, sum * 2);
                uchar* dp = dLine + 4 * x;
                dp[0] = static_cast<uchar>(diffR);
                dp[1] = static_cast<uchar>(diffG);
                dp[2] = static_cast<uchar>(diffB);
                dp[3] = static_cast<uchar>(alpha);
            }
        }
    }

    return diffImg;
}

double DiffEngine::computeSimilarity(const QImage& imageA, const QImage& imageB)
{
    if (imageA.isNull() || imageB.isNull()) return 0.0;

    QImage a = imageA.convertToFormat(QImage::Format_RGBA8888);
    QImage b = imageB.convertToFormat(QImage::Format_RGBA8888);

    const int w = std::min(a.width(), b.width());
    const int h = std::min(a.height(), b.height());
    if (w == 0 || h == 0) return 0.0;

    long long totalDiff = 0;
    const long long maxDiff = static_cast<long long>(w) * h * 255 * 3;

    for (int y = 0; y < h; ++y) {
        const uchar* aLine = a.constScanLine(y);
        const uchar* bLine = b.constScanLine(y);
        for (int x = 0; x < w; ++x) {
            const uchar* ap = aLine + 4 * x;
            const uchar* bp = bLine + 4 * x;
            totalDiff += std::abs(int(ap[0]) - int(bp[0]));
            totalDiff += std::abs(int(ap[1]) - int(bp[1]));
            totalDiff += std::abs(int(ap[2]) - int(bp[2]));
        }
    }

    // Penalize size mismatch
    const int maxW = std::max(a.width(), b.width());
    const int maxH = std::max(a.height(), b.height());
    const double areaCoverage = static_cast<double>(w * h) / (maxW * maxH);

    const double pixelSimilarity = 1.0 - static_cast<double>(totalDiff) / maxDiff;
    return pixelSimilarity * areaCoverage;
}

std::vector<QRectF> DiffEngine::findChangeRegions(const QImage& diffImage, int blockSize)
{
    std::vector<QRectF> regions;
    if (diffImage.isNull()) return regions;

    const int bw = (diffImage.width() + blockSize - 1) / blockSize;
    const int bh = (diffImage.height() + blockSize - 1) / blockSize;

    for (int by = 0; by < bh; ++by) {
        for (int bx = 0; bx < bw; ++bx) {
            const int x0 = bx * blockSize;
            const int y0 = by * blockSize;
            const int x1 = std::min(x0 + blockSize, diffImage.width());
            const int y1 = std::min(y0 + blockSize, diffImage.height());

            bool hasChange = false;
            for (int y = y0; y < y1 && !hasChange; ++y) {
                const uchar* line = diffImage.constScanLine(y);
                for (int x = x0; x < x1; ++x) {
                    if (line[4 * x + 3] > 0) {
                        hasChange = true;
                        break;
                    }
                }
            }

            if (hasChange) {
                regions.emplace_back(x0, y0, x1 - x0, y1 - y0);
            }
        }
    }

    return regions;
}

std::vector<TextChange> DiffEngine::diffText(const std::string& textA, const std::string& textB)
{
    std::vector<TextChange> changes;

    // Split into lines
    auto splitLines = [](const std::string& text) -> std::vector<std::string> {
        std::vector<std::string> lines;
        std::istringstream stream(text);
        std::string line;
        while (std::getline(stream, line)) {
            lines.push_back(line);
        }
        return lines;
    };

    auto linesA = splitLines(textA);
    auto linesB = splitLines(textB);

    // Simple LCS-based diff (Myers-like for line sequences)
    size_t m = linesA.size();
    size_t n = linesB.size();

    // Build LCS table
    std::vector<std::vector<int>> dp(m + 1, std::vector<int>(n + 1, 0));
    for (size_t i = 1; i <= m; ++i) {
        for (size_t j = 1; j <= n; ++j) {
            if (linesA[i - 1] == linesB[j - 1]) {
                dp[i][j] = dp[i - 1][j - 1] + 1;
            } else {
                dp[i][j] = std::max(dp[i - 1][j], dp[i][j - 1]);
            }
        }
    }

    // Backtrack to produce diff
    size_t i = m, j = n;
    std::vector<TextChange> reversed;
    while (i > 0 || j > 0) {
        if (i > 0 && j > 0 && linesA[i - 1] == linesB[j - 1]) {
            reversed.push_back({ChangeType::Unchanged, linesA[i - 1], static_cast<int>(i - 1)});
            --i; --j;
        } else if (j > 0 && (i == 0 || dp[i][j - 1] >= dp[i - 1][j])) {
            reversed.push_back({ChangeType::Inserted, linesB[j - 1], static_cast<int>(j - 1)});
            --j;
        } else {
            reversed.push_back({ChangeType::Deleted, linesA[i - 1], static_cast<int>(i - 1)});
            --i;
        }
    }

    changes.assign(reversed.rbegin(), reversed.rend());
    return changes;
}

} // namespace pdfe::diff
