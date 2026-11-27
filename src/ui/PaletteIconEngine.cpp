#include "PaletteIconEngine.h"

#include <QFile>
#include <QGuiApplication>
#include <QIcon>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QPixmapCache>
#include <QSvgRenderer>

#include <cmath>

namespace pdfe::ui {

PaletteIconEngine::PaletteIconEngine(const QString& resourcePath)
    : path_(resourcePath)
{
    QFile f(path_);
    if (f.open(QIODevice::ReadOnly)) {
        svg_ = f.readAll();
    }
    valid_ = !svg_.isEmpty() && QSvgRenderer(svg_).isValid();
}

QColor PaletteIconEngine::inkColorFor(QIcon::Mode mode)
{
    const QPalette pal = QGuiApplication::palette();
    switch (mode) {
    case QIcon::Disabled:
        return pal.color(QPalette::Disabled, QPalette::ButtonText);
    case QIcon::Selected:
        return pal.color(QPalette::Active, QPalette::HighlightedText);
    case QIcon::Normal:
    case QIcon::Active:
        break;
    }
    return pal.color(QPalette::Active, QPalette::ButtonText);
}

QPixmap PaletteIconEngine::scaledPixmap(const QSize& size, QIcon::Mode mode,
                                        QIcon::State state, qreal scale)
{
    Q_UNUSED(state);
    if (!valid_ || size.isEmpty()) {
        return QPixmap();
    }
    if (scale <= 0) {
        scale = 1.0;
    }

    const QColor ink = inkColorFor(mode);
    const QSize device(qMax(1, int(std::lround(size.width() * scale))),
                       qMax(1, int(std::lround(size.height() * scale))));

    // The colour is part of the key: a palette switch changes the key, so a
    // pixmap rendered under the previous theme is never handed back.
    const QString cacheKey = QStringLiteral("pdfe-palette-icon|%1|%2|%3x%4")
                                 .arg(path_, ink.name(QColor::HexArgb))
                                 .arg(device.width())
                                 .arg(device.height());
    QPixmap pm;
    if (QPixmapCache::find(cacheKey, &pm)) {
        pm.setDevicePixelRatio(scale);
        return pm;
    }

    QByteArray tinted = svg_;
    tinted.replace("currentColor", ink.name(QColor::HexRgb).toLatin1());
    QSvgRenderer renderer(tinted);
    if (!renderer.isValid()) {
        return QPixmap();
    }

    pm = QPixmap(device);
    pm.fill(Qt::transparent);
    {
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        renderer.render(&p, QRectF(QPointF(0, 0), QSizeF(device)));
    }
    // The alpha channel carries ink.alpha() (e.g. a translucent disabled
    // colour) -- apply it, since the SVG was given an opaque #rrggbb.
    if (ink.alpha() < 255) {
        QPixmap faded(device);
        faded.fill(Qt::transparent);
        QPainter p(&faded);
        p.setOpacity(ink.alphaF());
        p.drawPixmap(0, 0, pm);
        p.end();
        pm = faded;
    }
    QPixmapCache::insert(cacheKey, pm);
    pm.setDevicePixelRatio(scale);
    return pm;
}

QPixmap PaletteIconEngine::pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state)
{
    return scaledPixmap(size, mode, state, 1.0);
}

void PaletteIconEngine::paint(QPainter* painter, const QRect& rect, QIcon::Mode mode,
                              QIcon::State state)
{
    if (!painter || rect.isEmpty()) {
        return;
    }
    const qreal dpr = painter->device() ? painter->device()->devicePixelRatioF() : 1.0;
    const QPixmap pm = scaledPixmap(rect.size(), mode, state, dpr);
    if (!pm.isNull()) {
        painter->drawPixmap(rect, pm);
    }
}

QSize PaletteIconEngine::actualSize(const QSize& size, QIcon::Mode, QIcon::State)
{
    return size;
}

QIconEngine* PaletteIconEngine::clone() const
{
    return new PaletteIconEngine(*this);
}

QString PaletteIconEngine::key() const
{
    return QStringLiteral("PaletteIconEngine");
}

bool PaletteIconEngine::isNull()
{
    return !valid_;
}

QIcon PaletteIconEngine::icon(const QString& resourcePath)
{
    return QIcon(new PaletteIconEngine(resourcePath));
}

} // namespace pdfe::ui
