#pragma once

#include <QImage>
#include <QSizeF>
#include <QString>
#include <QStringList>

namespace pdfe::ui {

/// The two caption lines drawn under the signature image of a visual
/// signature: "Signed <when>" and the line saying it is not a digital
/// signature. Kept separate so the wording is tested where it is defined.
QStringList visualSignatureCaption(const QString& when);

/// The appearance of a visual signature, sized for a stamp of @p sizePt
/// points: the signature image fitted (aspect kept) into the upper part, and
/// visualSignatureCaption() drawn in the lower part. Rendered at 4 px per
/// point; the result has the same aspect ratio as @p sizePt, so the stamp
/// shows it unstretched when @p sizePt is the stamp's own rectangle.
QImage composeVisualSignature(const QImage& signature, const QString& when,
                              const QSizeF& sizePt);

/// The fraction of the appearance's height, from the bottom, that holds the
/// caption. Exposed for tests.
constexpr int kVisualSignatureCaptionPercent = 32;

} // namespace pdfe::ui
