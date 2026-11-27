#include "VisualSignature.h"

#include <QCoreApplication>
#include <QFont>
#include <QPainter>

#include <algorithm>
#include <cmath>

namespace pdfe::ui {

QStringList visualSignatureCaption(const QString& when)
{
    return {
        QCoreApplication::translate("VisualSignature", "Signed %1").arg(when),
        QCoreApplication::translate("VisualSignature", "Visual signature, not a digital signature"),
    };
}

QImage composeVisualSignature(const QImage& signature, const QString& when,
                              const QSizeF& sizePt)
{
    constexpr qreal kPxPerPt = 4.0;
    const int w = std::max(40, static_cast<int>(std::lround(sizePt.width() * kPxPerPt)));
    const int h = std::max(20, static_cast<int>(std::lround(sizePt.height() * kPxPerPt)));
    QImage out(w, h, QImage::Format_ARGB32_Premultiplied);
    out.fill(Qt::transparent);

    QPainter p(&out);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);

    const int textH = std::max(12, h * kVisualSignatureCaptionPercent / 100);
    const QRect imageArea(0, 0, w, h - textH);
    if (!signature.isNull()) {
        const QSize fitted = signature.size().scaled(imageArea.size(), Qt::KeepAspectRatio);
        const QRect target((w - fitted.width()) / 2,
                           (imageArea.height() - fitted.height()) / 2,
                           fitted.width(), fitted.height());
        p.drawImage(target, signature);
    }

    const QStringList caption = visualSignatureCaption(when);

    QFont dateFont;
    dateFont.setPixelSize(std::max(8, textH * 45 / 100));
    p.setFont(dateFont);
    p.setPen(Qt::black);
    p.drawText(QRect(0, h - textH, w, textH * 55 / 100), Qt::AlignCenter, caption.value(0));

    QFont noteFont;
    noteFont.setPixelSize(std::max(6, textH * 30 / 100));
    p.setFont(noteFont);
    p.setPen(QColor(90, 90, 90));
    p.drawText(QRect(0, h - textH * 45 / 100, w, textH * 45 / 100), Qt::AlignCenter,
               caption.value(1));
    p.end();
    return out;
}

} // namespace pdfe::ui
