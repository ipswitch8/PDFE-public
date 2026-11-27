#pragma once

#include <QIcon>
#include <QProxyStyle>
#include <QString>

namespace pdfe::ui {

/// Application style proxy that replaces the few STYLE-drawn icons PDFE shows
/// with palette-following ones, so they stay legible under the dark theme.
///
/// QDockWidget takes its title-bar float and close icons from
/// style()->standardIcon(). Fusion -- the style on Linux -- answers with fixed
/// dark-grey PNGs that ignore the palette, so on the dark theme's #353535
/// title bar the Pages / Search / Comments dock buttons measured about 2.3:1.
/// This proxy answers those standard pixmaps with PaletteIconEngine icons
/// drawn from :/icons/style/*.svg, which are recoloured with the current
/// palette's ButtonText on every paint, the same mechanism the toolbar action
/// icons use. Everything else is forwarded to the wrapped base style
/// unchanged.
class ThemedIconStyle : public QProxyStyle {
    Q_OBJECT
public:
    /// Wraps @p base (takes ownership). A null @p base wraps the
    /// application's default style.
    explicit ThemedIconStyle(QStyle* base = nullptr);

    QIcon standardIcon(StandardPixmap standardIcon, const QStyleOption* option = nullptr,
                       const QWidget* widget = nullptr) const override;

    /// The resource path of the replacement for @p sp, or an empty string
    /// when this proxy leaves @p sp to the base style.
    static QString replacementPathFor(StandardPixmap sp);

    /// Installs a ThemedIconStyle around the current application style, once.
    /// Idempotent: returns false, and changes nothing, if the application
    /// style is already a ThemedIconStyle. Call it before the application
    /// palette is set, because QApplication::setStyle() re-applies the new
    /// style's standard palette.
    static bool installOnApplication();
};

} // namespace pdfe::ui
