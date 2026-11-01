#include "ThumbnailRenderer.h"
#include "core/Document.h"
#include "core/Page.h"
#include "core/MuPdfContext.h"

#include <QCoreApplication>
#include <QRectF>
#include <QRunnable>
#include <QThread>
#include <QThreadPool>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <list>
#include <mutex>
#include <unordered_map>

namespace pdfe::rendering {

// Legitimate production/test instrumentation: process-wide count of full-page
// rasterizations. Incremented only when the expensive render path actually
// runs (never on a cache hit or a cancelled task).
static std::atomic<std::size_t> g_thumbnailRenderCount{0};

// Thread-affinity oracle (phase-1 claim-check): counts rasterizations that ran
// on the GUI/main thread instead of a worker. The whole point of the worker
// pool is that this stays ZERO — a regression that reverted to rendering inline
// on the event loop (reintroducing the zoom hang) would make this equal
// g_thumbnailRenderCount. The functional tests (cache/cancel/count) are
// thread-agnostic and would NOT catch that; this is the mechanism that does.
static std::atomic<std::size_t> g_guiThreadRenderCount{0};

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

// Exception-safe thumbnail raster (R2-05 discipline, mirrors
// TileRenderer::detail::renderTileToPixmap). fz_run_display_list or the pixmap
// allocation can fz_throw (corrupt embedded image, OOM); MuPDF's setjmp/longjmp
// then unwinds to the nearest fz_try, so WITHOUT the fz_always/fz_catch guards
// the draw device AND the half-built pixmap both leak. Thumbnails are
// re-requested on every scroll, so the leak would be unbounded.
//
// The display list is thread-safe and document-independent: unlike fz_page,
// it can be rasterized on a worker thread with a cloned fz_context while the
// GUI thread is free. Bounds come from fz_bound_display_list (not the page), so
// the worker never touches fz_document/fz_page. Pure C MuPDF only inside the
// fz_try — no C++ objects with non-trivial destructors on the stack.
fz_pixmap* renderThumbnailToPixmap(fz_context* ctx, fz_display_list* list,
                                   int targetW, int targetH) {
    fz_pixmap* pixmap = NULL;
    fz_device* dev = NULL;
    fz_var(pixmap);
    fz_var(dev);

    fz_try(ctx) {
        fz_rect bounds = fz_bound_display_list(ctx, list);
        float bw = bounds.x1 - bounds.x0;
        float bh = bounds.y1 - bounds.y0;
        if (bw <= 0.0f || bh <= 0.0f) {
            fz_throw(ctx, FZ_ERROR_GENERIC, "empty page bounds for thumbnail");
        }

        // Fit the page into the target box, preserving aspect ratio.
        float sx = static_cast<float>(targetW) / bw;
        float sy = static_cast<float>(targetH) / bh;
        float scale = sx < sy ? sx : sy;

        fz_matrix ctm = fz_scale(scale, scale);
        fz_irect bbox = fz_round_rect(fz_transform_rect(bounds, ctm));

        fz_colorspace* cs = fz_device_rgb(ctx);
        // Pixmap carries the transformed bbox origin, so the draw device maps
        // the (possibly offset) page bounds into pixmap space automatically —
        // no explicit translate needed.
        pixmap = fz_new_pixmap_with_bbox(ctx, cs, bbox, NULL, 1);
        fz_clear_pixmap_with_value(ctx, pixmap, 255);

        dev = fz_new_draw_device(ctx, fz_identity, pixmap);
        fz_run_display_list(ctx, list, dev, ctm, fz_infinite_rect, NULL);
        fz_close_device(ctx, dev);
    }
    fz_always(ctx) {
        fz_drop_device(ctx, dev);  // freed on BOTH success and throw
    }
    fz_catch(ctx) {
        fz_drop_pixmap(ctx, pixmap);  // free the half-built pixmap, then rethrow
        fz_rethrow(ctx);
    }
    return pixmap;
}

// Convert an RGBA pixmap (device_rgb + alpha => n == 4) to a deep-copied QImage.
// Pure C++ — no MuPDF throws possible here.
QImage pixmapToImage(fz_pixmap* pixmap) {
    QImage image(pixmap->w, pixmap->h, QImage::Format_RGBA8888);
    for (int y = 0; y < pixmap->h; ++y) {
        std::memcpy(image.scanLine(y),
                    pixmap->samples + static_cast<std::size_t>(y) * pixmap->stride,
                    static_cast<std::size_t>(pixmap->w) * 4);
    }
    return image;
}
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

    // Dedicated worker pool: full-page rasterization runs here, OFF the GUI
    // thread, mirroring TileRenderer's QThreadPool. Sized conservatively (half
    // the cores) so a large document's thumbnail backlog does not starve the
    // main canvas's own TileRenderer pool during interactive zoom/scroll.
    QThreadPool pool;

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
        pool.setMaxThreadCount(std::max(1, QThread::idealThreadCount() / 2));
    }

    ~Impl() {
        // Stop delivery of in-flight work, then BLOCK until every worker has
        // finished before any Impl member (cache, pool, document pointer) is
        // destroyed — equivalent safety to TileRenderer::~TileRenderer()'s
        // cancelAll()+waitForDone(). A worker only touches this Impl's cache and
        // the shared_ptr<MuPdfContext> it holds, both of which outlive the wait.
        cancelAll();
        pool.waitForDone();
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

// Worker task: rasterizes one thumbnail from a pre-built display list on a pool
// thread, then marshals the QImage back to the GUI thread. Owns the display
// list (drops it on every exit path). Mirrors TileRenderer::RenderTask.
class ThumbnailRenderTask : public QRunnable {
public:
    ThumbnailRenderTask(ThumbnailRenderer* renderer,
                        std::shared_ptr<pdfe::core::MuPdfContext> context,
                        fz_display_list* displayList,
                        int pageIndex,
                        const QSize& size,
                        std::shared_ptr<std::atomic<bool>> cancelled)
        : renderer_(renderer)
        , context_(std::move(context))
        , displayList_(displayList)
        , pageIndex_(pageIndex)
        , size_(size)
        , cancelled_(std::move(cancelled))
    {
        setAutoDelete(true);
    }

    ~ThumbnailRenderTask() override {
        // Cancelled before run() executed: free the display list we still own.
        if (displayList_ && context_) {
            auto* list = displayList_;
            context_->execute([list](fz_context* ctx) {
                fz_drop_display_list(ctx, list);
            });
        }
    }

    void run() override {
        if (!cancelled_ || cancelled_->load()) {
            return;  // destructor drops the display list
        }

        try {
            // Establish this worker thread's own context clone (R2-06/#154).
            fz_context* clone = context_->threadLocalClone();
            if (!clone) {
                return;
            }
            if (cancelled_->load()) {
                return;
            }

            fz_display_list* list = displayList_;

            // Count the rasterization exactly once, immediately before it runs
            // and after the final cancel check — a cache hit never reaches here
            // and a task cancelled before this point never increments.
            g_thumbnailRenderCount.fetch_add(1, std::memory_order_relaxed);

            // Thread-affinity oracle: record if this raster is (wrongly) running
            // on the GUI/main thread. Must always be the worker thread here.
            if (QCoreApplication::instance()
                && QThread::currentThread()
                       == QCoreApplication::instance()->thread()) {
                g_guiThreadRenderCount.fetch_add(1, std::memory_order_relaxed);
            }

            fz_pixmap* pixmap = nullptr;
            context_->execute([&pixmap, list, this](fz_context* ctx) {
                pixmap = renderThumbnailToPixmap(ctx, list, size_.width(),
                                                 size_.height());
            });

            if (!pixmap) {
                return;  // destructor drops the display list
            }

            QImage image = pixmapToImage(pixmap);

            context_->execute([pixmap](fz_context* ctx) {
                fz_drop_pixmap(ctx, pixmap);
            });

            // Display list no longer needed — drop it and disarm the destructor.
            context_->execute([list](fz_context* ctx) {
                fz_drop_display_list(ctx, list);
            });
            displayList_ = nullptr;

            // Match the historical exact-size contract: scale to the requested
            // box if the aspect-preserving raster came out a pixel off.
            if (image.width() != size_.width() || image.height() != size_.height()) {
                image = image.scaled(size_, Qt::KeepAspectRatio,
                                     Qt::SmoothTransformation);
            }

            if (cancelled_->load()) {
                return;
            }

            // Store in cache, then deliver on the GUI thread. renderer_ is the
            // context object, so Qt drops this call if the renderer is destroyed
            // before it runs (no use-after-free; the waitForDone in ~Impl keeps
            // the cache alive while cachePut runs here on the worker).
            const CacheKey key{pageIndex_, size_.width(), size_.height()};
            renderer_->pImpl->cachePut(key, image);

            ThumbnailRenderer* renderer = renderer_;
            int pageIndex = pageIndex_;
            QMetaObject::invokeMethod(
                renderer,
                [renderer, pageIndex, image = std::move(image)]() {
                    renderer->onThumbnailRendered(pageIndex, image);
                },
                Qt::QueuedConnection);
        } catch (...) {
            // Silently ignore rendering errors for individual thumbnails.
            // The display list (if still owned) is freed by the destructor.
        }
    }

private:
    ThumbnailRenderer* renderer_;
    std::shared_ptr<pdfe::core::MuPdfContext> context_;
    fz_display_list* displayList_;
    int pageIndex_;
    QSize size_;
    std::shared_ptr<std::atomic<bool>> cancelled_;
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
    g_guiThreadRenderCount.store(0, std::memory_order_relaxed);
}

std::size_t ThumbnailRenderer::guiThreadRenderCount() noexcept
{
    return g_guiThreadRenderCount.load(std::memory_order_relaxed);
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

    // Defer the display-list build + worker submission to the event loop. This
    // is what keeps cancellation real: cancelPage()/cancelAll() called before
    // the loop processes this task set the flag / drop the entry, so the lambda
    // below returns before any display list is built or any worker is started —
    // nothing reaches the (now off-thread) raster path. `this` is the context
    // object, so the call is dropped outright if the renderer is destroyed.
    QMetaObject::invokeMethod(
        this,
        [this, pageIndex, size, cancelled]() {
            if (!cancelled || cancelled->load()) return;
            {
                std::lock_guard<std::mutex> lock(pImpl->tasksMutex);
                if (pImpl->pendingTasks.find(pageIndex) == pImpl->pendingTasks.end()) {
                    return; // cancelled/erased before we got here
                }
            }

            const CacheKey key{pageIndex, size.width(), size.height()};

            // Cache first: a hit avoids re-rasterizing the full page. Delivered
            // on this (GUI) thread, same as before.
            QImage cached;
            if (pImpl->cacheGet(key, cached)) {
                if (cancelled->load()) return;
                onThumbnailRendered(pageIndex, cached);
                return;
            }

            // Build the display list on the GUI thread, serialized by
            // Document::docMutex inside createDisplayList — exactly how
            // TileRenderer::requestTile serializes document access before
            // handing the thread-safe display list to a worker.
            fz_display_list* list = nullptr;
            try {
                list = static_cast<fz_display_list*>(
                    pImpl->document->createDisplayList(pageIndex));
            } catch (...) {
                // Invalid/unloadable page: drop the bookkeeping entry so it does
                // not wedge future dedup, and deliver nothing.
                std::lock_guard<std::mutex> lock(pImpl->tasksMutex);
                pImpl->pendingTasks.erase(pageIndex);
                return;
            }
            if (!list) {
                std::lock_guard<std::mutex> lock(pImpl->tasksMutex);
                pImpl->pendingTasks.erase(pageIndex);
                return;
            }

            if (cancelled->load()) {
                // Cancelled while we built the list — drop it, don't leak.
                auto ctx = pImpl->document->context();
                ctx->execute([list](fz_context* c) {
                    fz_drop_display_list(c, list);
                });
                return;
            }

            auto* task = new ThumbnailRenderTask(
                this, pImpl->document->context(), list, pageIndex, size, cancelled);
            pImpl->pool.start(task);
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
        } else {
            // Cancelled (entry already erased) while the worker was in flight:
            // deliver nothing.
            return;
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
