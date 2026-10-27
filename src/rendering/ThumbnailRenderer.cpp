#include "ThumbnailRenderer.h"
#include "core/Document.h"
#include "core/Page.h"
#include "core/MuPdfContext.h"

#include <QRectF>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <list>
#include <mutex>
#include <unordered_map>

namespace pdfe::rendering {

// Legitimate production/test instrumentation: process-wide count of full-page
// rasterizations. Incremented only when the expensive render path actually
// runs (never on a cache hit or a cancelled task).
static std::atomic<std::size_t> g_thumbnailRenderCount{0};

namespace {
// Cache key: page index + exact requested target size. Two scroll positions
// asking for the same page at the same thumbnail size share one rasterization.
struct CacheKey {
    int pageIndex;
    int width;
    int height;

    bool operator==(const CacheKey& other) const noexcept {
        return pageIndex == other.pageIndex
            && width == other.width
            && height == other.height;
    }
};

struct CacheKeyHash {
    std::size_t operator()(const CacheKey& k) const noexcept {
        std::size_t h = static_cast<std::size_t>(static_cast<uint32_t>(k.pageIndex));
        h = h * 1000003u + static_cast<std::size_t>(static_cast<uint32_t>(k.width));
        h = h * 1000003u + static_cast<std::size_t>(static_cast<uint32_t>(k.height));
        return h;
    }
};
} // namespace

class ThumbnailRenderer::Impl {
public:
    pdfe::core::Document* document;

    struct TaskInfo {
        std::shared_ptr<std::atomic<bool>> cancelled;
        std::function<void(int, QImage)> callback;
        QSize size;
    };

    std::mutex tasksMutex;
    std::unordered_map<int, TaskInfo> pendingTasks;

    // Bounded LRU result cache (mirrors TileCache's list+map LRU discipline)
    // so fast scrolling does not re-rasterize already-rendered thumbnails and
    // memory stays capped.
    struct CacheEntry {
        CacheKey key;
        QImage image;
    };
    using CacheList = std::list<CacheEntry>;
    using CacheMap = std::unordered_map<CacheKey, CacheList::iterator, CacheKeyHash>;

    std::mutex cacheMutex;
    CacheList lruList;                 // front = most-recently-used
    CacheMap cacheMap;
    std::size_t maxEntries{256};       // capacity cap; thumbnails are small

    explicit Impl(pdfe::core::Document* doc)
        : document(doc)
    {
    }

    ~Impl() {
        cancelAll();
    }

    void cancelAll() {
        std::lock_guard<std::mutex> lock(tasksMutex);

        for (auto& [pageIndex, info] : pendingTasks) {
            info.cancelled->store(true);
        }

        pendingTasks.clear();
    }

    // Returns cached image for (pageIndex,size) if present, marking it MRU.
    bool cacheGet(const CacheKey& key, QImage& out) {
        std::lock_guard<std::mutex> lock(cacheMutex);
        auto it = cacheMap.find(key);
        if (it == cacheMap.end()) return false;
        lruList.splice(lruList.begin(), lruList, it->second);
        out = it->second->image;
        return true;
    }

    void cachePut(const CacheKey& key, const QImage& image) {
        std::lock_guard<std::mutex> lock(cacheMutex);
        auto it = cacheMap.find(key);
        if (it != cacheMap.end()) {
            it->second->image = image;
            lruList.splice(lruList.begin(), lruList, it->second);
            return;
        }
        lruList.push_front(CacheEntry{key, image});
        cacheMap[key] = lruList.begin();
        while (cacheMap.size() > maxEntries) {
            auto& victim = lruList.back();
            cacheMap.erase(victim.key);
            lruList.pop_back();
        }
    }

    void cacheClear() {
        std::lock_guard<std::mutex> lock(cacheMutex);
        cacheMap.clear();
        lruList.clear();
    }

    void cacheClearPage(int pageIndex) {
        std::lock_guard<std::mutex> lock(cacheMutex);
        for (auto it = lruList.begin(); it != lruList.end();) {
            if (it->key.pageIndex == pageIndex) {
                cacheMap.erase(it->key);
                it = lruList.erase(it);
            } else {
                ++it;
            }
        }
    }
};

ThumbnailRenderer::ThumbnailRenderer(pdfe::core::Document* document, QObject* parent)
    : QObject(parent)
    , pImpl(std::make_unique<Impl>(document))
{
}

ThumbnailRenderer::~ThumbnailRenderer() = default;

std::size_t ThumbnailRenderer::renderCount() noexcept
{
    return g_thumbnailRenderCount.load(std::memory_order_relaxed);
}

void ThumbnailRenderer::resetRenderCount() noexcept
{
    g_thumbnailRenderCount.store(0, std::memory_order_relaxed);
}

void ThumbnailRenderer::invalidate()
{
    pImpl->cacheClear();
}

void ThumbnailRenderer::invalidatePage(int pageIndex)
{
    pImpl->cacheClearPage(pageIndex);
}

void ThumbnailRenderer::requestThumbnail(
    int pageIndex,
    const QSize& size,
    std::function<void(int, QImage)> callback)
{
    std::shared_ptr<std::atomic<bool>> cancelled;
    {
        std::lock_guard<std::mutex> lock(pImpl->tasksMutex);

        // Check if already pending (dedup in-flight requests for the page).
        auto it = pImpl->pendingTasks.find(pageIndex);
        if (it != pImpl->pendingTasks.end()) {
            it->second.callback = callback;
            it->second.size = size;
            return;
        }

        cancelled = std::make_shared<std::atomic<bool>>(false);

        Impl::TaskInfo info;
        info.cancelled = cancelled;
        info.callback = callback;
        info.size = size;
        pImpl->pendingTasks[pageIndex] = info;
    }

    // Defer the actual work to the event loop. This is what makes cancellation
    // real: cancelPage()/cancelAll() called before the loop processes this task
    // set the flag / drop the entry, and processTask() below skips the expensive
    // render entirely. Rendering itself remains on the (UI) thread because
    // MuPDF fz_document/fz_page are not thread-safe across contexts; the win is
    // the cache (no re-raster) plus honored cancellation, not off-thread render.
    QMetaObject::invokeMethod(
        this,
        [this, pageIndex, size, cancelled]() {
            // Pre-render cancel check: skip work that was cancelled before it ran.
            if (!cancelled || cancelled->load()) return;
            {
                std::lock_guard<std::mutex> lock(pImpl->tasksMutex);
                if (pImpl->pendingTasks.find(pageIndex) == pImpl->pendingTasks.end()) {
                    return; // cancelled/erased before we got here
                }
            }

            const CacheKey key{pageIndex, size.width(), size.height()};

            // Cache first: a hit avoids re-rasterizing the full page.
            QImage cached;
            if (pImpl->cacheGet(key, cached)) {
                if (cancelled->load()) return;
                onThumbnailRendered(pageIndex, cached);
                return;
            }

            // Miss: rasterize, then store in the cache.
            try {
                auto page = pImpl->document->page(pageIndex);
                if (!page) { onThumbnailRendered(pageIndex, QImage()); return; }

                QRectF pageBounds = page->boundingBox();
                if (pageBounds.isEmpty()) { onThumbnailRendered(pageIndex, QImage()); return; }

                float scaleX = static_cast<float>(size.width()) / pageBounds.width();
                float scaleY = static_cast<float>(size.height()) / pageBounds.height();
                float scale = std::min(scaleX, scaleY);

                // Final cancel check before the expensive call.
                if (cancelled->load()) return;

                g_thumbnailRenderCount.fetch_add(1, std::memory_order_relaxed);
                QImage image = page->render(pageBounds, scale);
                if (image.isNull()) { onThumbnailRendered(pageIndex, QImage()); return; }

                if (image.width() != size.width() || image.height() != size.height()) {
                    image = image.scaled(size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
                }

                pImpl->cachePut(key, image);

                // Post-render cancel check: do not deliver work cancelled while
                // it was rendering.
                if (cancelled->load()) return;

                onThumbnailRendered(pageIndex, image);
            } catch (...) {
                // Silently ignore rendering errors for individual thumbnails.
            }
        },
        Qt::QueuedConnection
    );
}

void ThumbnailRenderer::cancelPage(int pageIndex)
{
    std::lock_guard<std::mutex> lock(pImpl->tasksMutex);

    auto it = pImpl->pendingTasks.find(pageIndex);
    if (it != pImpl->pendingTasks.end()) {
        it->second.cancelled->store(true);
        pImpl->pendingTasks.erase(it);
    }
}

void ThumbnailRenderer::cancelAll()
{
    pImpl->cancelAll();
}

void ThumbnailRenderer::onThumbnailRendered(int pageIndex, const QImage& image)
{
    std::function<void(int, QImage)> callback;

    {
        std::lock_guard<std::mutex> lock(pImpl->tasksMutex);

        auto it = pImpl->pendingTasks.find(pageIndex);
        if (it != pImpl->pendingTasks.end()) {
            callback = it->second.callback;
            pImpl->pendingTasks.erase(it);
        }
    }

    // Emit signal
    emit thumbnailReady(pageIndex, image);

    // Call callback if provided
    if (callback) {
        callback(pageIndex, image);
    }
}

} // namespace pdfe::rendering
