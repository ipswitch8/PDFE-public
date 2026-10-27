#include "TileRenderer.h"
#include "Tile.h"
#include "TileRenderInternal.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include <QRunnable>
#include <QThread>
#include <algorithm>
#include <cstring>

namespace pdfe::rendering {

namespace detail {

// Exception-safe tile raster (R2-05). fz_run_display_list (or fz_new_draw_device
// / the pixmap clear) can fz_throw during rasterization — e.g. a corrupt
// embedded image, or memory exhaustion on a large/complex tile. MuPDF's
// setjmp/longjmp then unwinds to the nearest enclosing fz_try, so WITHOUT a
// guard here the longjmp jumps clean past fz_close_device/fz_drop_device AND
// orphans the pixmap: BOTH leak. Tiles are re-requested on every scroll/zoom,
// so the leak is unbounded.
//
// The fz_always block drops the draw device on BOTH the success and throw paths;
// the fz_catch drops the half-built pixmap and rethrows, so a throwing render
// frees everything and returns nothing. Pure C MuPDF only — no C++ objects with
// non-trivial destructors on the stack inside fz_try/fz_catch.
fz_pixmap* renderTileToPixmap(fz_context* ctx, fz_display_list* list, fz_matrix ctm)
{
    fz_pixmap* pixmap = NULL;
    fz_device* dev = NULL;
    fz_var(pixmap);
    fz_var(dev);

    fz_try(ctx) {
        fz_colorspace* cs = fz_device_rgb(ctx);
        pixmap = fz_new_pixmap(ctx, cs, TILE_SIZE, TILE_SIZE, NULL, 1);
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

} // namespace detail

// RenderTask takes a pre-built fz_display_list (opaque void*) rather than
// accessing the Document directly.  This decouples document access (serialized
// by Document::createDisplayList) from pixel rendering (parallel, thread-safe).
class TileRenderer::RenderTask : public QRunnable {
public:
    RenderTask(TileRenderer* renderer,
               std::shared_ptr<pdfe::core::MuPdfContext> context,
               void* displayList,
               const TileId& id,
               std::shared_ptr<std::atomic<bool>> cancelled,
               uint64_t sequence)
        : m_renderer(renderer)
        , m_context(std::move(context))
        , m_displayList(displayList)
        , m_id(id)
        , m_cancelled(cancelled)
        , m_sequence(sequence)
    {
        setAutoDelete(true);
    }

    ~RenderTask()
    {
        // If run() didn't execute (cancelled before start), clean up
        if (m_displayList && m_context) {
            m_context->execute([list = m_displayList](fz_context* ctx) {
                fz_drop_display_list(ctx, static_cast<fz_display_list*>(list));
            });
        }
    }

    void run() override
    {
        if (m_cancelled->load()) {
            return;  // destructor will clean up display list
        }

        try {
            // Get thread-local MuPDF context clone for this worker thread
            auto* ctx = m_context->threadLocalClone();
            if (!ctx) {
                return;
            }

            if (m_cancelled->load()) {
                return;
            }

            auto* list = static_cast<fz_display_list*>(m_displayList);

            // Calculate tile position in PDF coordinates
            float tileX = static_cast<float>(m_id.col * TILE_SIZE) / m_id.zoom;
            float tileY = static_cast<float>(m_id.row * TILE_SIZE) / m_id.zoom;

            // Transform: translate tile origin to (0,0), then scale by zoom
            // PDF (x,y) → (x-tileX, y-tileY) → ((x-tileX)*zoom, (y-tileY)*zoom)
            fz_matrix ctm = fz_pre_translate(fz_scale(m_id.zoom, m_id.zoom),
                                             -tileX, -tileY);

            // Render display list to a TILE_SIZE x TILE_SIZE pixmap
            fz_pixmap* pixmap = nullptr;
            m_context->execute([&pixmap, list, ctm](fz_context* ctx) {
                // Exception-safe raster (R2-05): on an fz_throw here (corrupt
                // embedded image), renderTileToPixmap frees BOTH the draw
                // device and the half-built pixmap before rethrowing, so
                // execute() below sees a MuPdfException with nothing leaked and
                // pixmap left nullptr for run()'s error handling.
                pixmap = detail::renderTileToPixmap(ctx, list, ctm);
            });

            if (!pixmap || m_cancelled->load()) {
                if (pixmap) {
                    m_context->execute([pixmap](fz_context* ctx) {
                        fz_drop_pixmap(ctx, pixmap);
                    });
                }
                return;
            }

            // Convert pixmap to QImage (pure C++ — no MuPDF throws possible)
            QImage image(pixmap->w, pixmap->h, QImage::Format_RGBA8888);
            for (int y = 0; y < pixmap->h; ++y) {
                std::memcpy(image.scanLine(y),
                            pixmap->samples + y * pixmap->stride,
                            pixmap->w * 4);
            }

            m_context->execute([pixmap](fz_context* ctx) {
                fz_drop_pixmap(ctx, pixmap);
            });

            if (m_cancelled->load()) {
                return;
            }

            // Drop display list before delivering (no longer needed)
            m_context->execute([list](fz_context* ctx) {
                fz_drop_display_list(ctx, list);
            });
            m_displayList = nullptr;

            // Notify renderer on main thread.
            // Capture by value — RenderTask will be auto-deleted after run().
            auto* renderer = m_renderer;
            auto id = m_id;
            auto seq = m_sequence;
            QMetaObject::invokeMethod(
                renderer,
                [renderer, id, image = std::move(image), seq]() {
                    renderer->onTileRendered(id, image, seq);
                },
                Qt::QueuedConnection
            );

        } catch (...) {
            // Silently ignore rendering errors for individual tiles
        }
    }

private:
    TileRenderer* m_renderer;
    std::shared_ptr<pdfe::core::MuPdfContext> m_context;
    void* m_displayList;
    TileId m_id;
    std::shared_ptr<std::atomic<bool>> m_cancelled;
    uint64_t m_sequence;
};

TileRenderer::TileRenderer(pdfe::core::Document* document, QObject* parent)
    : QObject(parent)
    , m_document(document)
    , m_threadPool(new QThreadPool(this))
{
    // Use all but one CPU core for rendering, minimum 1
    int threadCount = std::max(1, QThread::idealThreadCount() - 1);
    m_threadPool->setMaxThreadCount(threadCount);
}

TileRenderer::~TileRenderer()
{
    cancelAll();
    m_threadPool->waitForDone();
}

void TileRenderer::requestTile(const TileId& id, std::function<void(TileId, QImage)> callback)
{
    std::lock_guard<std::mutex> lock(m_tasksMutex);

    // Check if already pending
    auto it = m_pendingTasks.find(id);
    if (it != m_pendingTasks.end()) {
        // Update callback
        it->second.callback = callback;
        return;
    }

    // Create display list on the calling thread (document access serialized
    // internally by Document::createDisplayList via docMutex).
    void* displayList = nullptr;
    try {
        displayList = m_document->createDisplayList(id.pageIndex);
    } catch (...) {
        return;  // silently ignore invalid pages
    }

    // Create cancellation flag
    auto cancelled = std::make_shared<std::atomic<bool>>(false);

    // Store task info with unique sequence number
    TaskInfo info;
    info.cancelled = cancelled;
    info.callback = callback;
    info.sequence = m_nextSequence++;
    m_pendingTasks[id] = info;

    // Create and queue render task (owns the display list)
    auto* task = new RenderTask(this, m_document->context(),
                                displayList, id, cancelled, info.sequence);
    m_threadPool->start(task);
}

void TileRenderer::cancelPage(int pageIndex)
{
    std::lock_guard<std::mutex> lock(m_tasksMutex);

    auto it = m_pendingTasks.begin();
    while (it != m_pendingTasks.end()) {
        if (it->first.pageIndex == pageIndex) {
            it->second.cancelled->store(true);
            it = m_pendingTasks.erase(it);
        } else {
            ++it;
        }
    }
}

void TileRenderer::cancelAll()
{
    std::lock_guard<std::mutex> lock(m_tasksMutex);

    for (auto& [id, info] : m_pendingTasks) {
        info.cancelled->store(true);
    }

    m_pendingTasks.clear();
}

void TileRenderer::waitForDone()
{
    if (m_threadPool) {
        m_threadPool->waitForDone();
    }
}

void TileRenderer::setPriority(const TileId& id, int priority)
{
    // QThreadPool doesn't support dynamic priority changes
    // This could be implemented with a custom priority queue if needed
    // For now, this is a no-op
    (void)id;
    (void)priority;
}

void TileRenderer::onTileRendered(const TileId& id, const QImage& image, uint64_t sequence)
{
    std::function<void(TileId, QImage)> callback;
    bool valid = false;

    {
        std::lock_guard<std::mutex> lock(m_tasksMutex);

        auto it = m_pendingTasks.find(id);
        if (it != m_pendingTasks.end() && it->second.sequence == sequence) {
            callback = it->second.callback;
            m_pendingTasks.erase(it);
            valid = true;
        }
        // If sequence doesn't match, this is a stale render — discard it
    }

    if (valid) {
        emit tileReady(id, image);

        if (callback) {
            callback(id, image);
        }
    }
}

} // namespace pdfe::rendering
