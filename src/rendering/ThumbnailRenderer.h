#pragma once

#include <QObject>
#include <QImage>
#include <QSize>
#include <cstddef>
#include <functional>
#include <memory>

namespace pdfe::core {
class Document;
}

namespace pdfe::rendering {

class ThumbnailRenderer : public QObject {
    Q_OBJECT

public:
    explicit ThumbnailRenderer(pdfe::core::Document* document, QObject* parent = nullptr);
    ~ThumbnailRenderer() override;

    void requestThumbnail(int pageIndex, const QSize& size, std::function<void(int, QImage)> callback);
    void cancelPage(int pageIndex);
    void cancelAll();

    /// Drop all cached thumbnails. Call after the document content changes
    /// (page edit, reorder, delete) so stale rasterizations are not delivered.
    void invalidate();

    /// Drop cached thumbnails for a single page (e.g. after that page is edited).
    void invalidatePage(int pageIndex);

    /// Legitimate production/test instrumentation: total number of full-page
    /// rasterizations performed by ALL ThumbnailRenderer instances since the
    /// last resetRenderCount(). A cache hit does NOT increment this. Used to
    /// prove (a) fast-scroll re-requests are served from cache and (b) cancelled
    /// work never reaches the expensive render path.
    [[nodiscard]] static std::size_t renderCount() noexcept;
    static void resetRenderCount() noexcept;

signals:
    void thumbnailReady(int pageIndex, QImage thumbnail);

private:
    friend class ThumbnailRenderTask;

    void onThumbnailRendered(int pageIndex, const QImage& image);

    class Impl;
    std::unique_ptr<Impl> pImpl;
};

} // namespace pdfe::rendering
