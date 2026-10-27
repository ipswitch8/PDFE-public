#pragma once
#include "TileId.h"
#include <QObject>
#include <QImage>
#include <QThreadPool>
#include <functional>
#include <memory>
#include <atomic>
#include <unordered_map>
#include <mutex>

namespace pdfe::core {
    class Document;
}

namespace pdfe::rendering {

class TileRenderer : public QObject {
    Q_OBJECT

public:
    explicit TileRenderer(pdfe::core::Document* document, QObject* parent = nullptr);
    ~TileRenderer();

    // virtual so a headless test can subclass and observe the exact TileId
    // (and hence the effective render zoom = logical-zoom * devicePixelRatio)
    // the canvas requests, without depending on async tile rasterization.
    virtual void requestTile(const TileId& id, std::function<void(TileId, QImage)> callback);
    void cancelPage(int pageIndex);
    void cancelAll();
    void setPriority(const TileId& id, int priority);
    /// Block until every in-flight render task completes. Useful when
    /// the caller is about to swap out the backing document and needs
    /// workers to stop touching the old pointer.
    void waitForDone();

signals:
    void tileReady(TileId id, QImage image);

private:
    class RenderTask;
    friend class RenderTask;

    void onTileRendered(const TileId& id, const QImage& image, uint64_t sequence);

    pdfe::core::Document* m_document;
    QThreadPool* m_threadPool;

    struct TaskInfo {
        std::shared_ptr<std::atomic<bool>> cancelled;
        std::function<void(TileId, QImage)> callback;
        uint64_t sequence = 0;
    };

    std::mutex m_tasksMutex;
    std::unordered_map<TileId, TaskInfo> m_pendingTasks;
    uint64_t m_nextSequence = 0;
};

} // namespace pdfe::rendering
