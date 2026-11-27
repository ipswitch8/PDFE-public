#include "ThemedIconStyle.h"

#include "PaletteIconEngine.h"

#include <QApplication>
#include <QStyleFactory>

// The replacement SVGs live in action_icons.qrc (prefix /icons/style) in the
// pdfe_ui STATIC library. Pin its RCC init here as MainWindow does, so the
// resource tables are kept and registered before the first icon request.
extern int qInitResources_action_icons();

namespace pdfe::ui {

ThemedIconStyle::ThemedIconStyle(QStyle* base)
    : QProxyStyle(base)
{
    ::qInitResources_action_icons();
}

QString ThemedIconStyle::replacementPathFor(StandardPixmap sp)
{
    switch (sp) {
    // QDockWidget's float button. Qt 6 asks for SP_TitleBarNormalButton.
    case SP_TitleBarNormalButton:
        return QStringLiteral(":/icons/style/dock_float.svg");
    // QDockWidget's close button (SP_TitleBarCloseButton in Qt 6; older
    // releases and some styles use SP_DockWidgetCloseButton).
    case SP_TitleBarCloseButton:
    case SP_DockWidgetCloseButton:
        return QStringLiteral(":/icons/style/dock_close.svg");
    default:
        return QString();
    }
}

QIcon ThemedIconStyle::standardIcon(StandardPixmap sp, const QStyleOption* option,
                                    const QWidget* widget) const
{
    const QString path = replacementPathFor(sp);
    if (!path.isEmpty()) {
        const QIcon icon = PaletteIconEngine::icon(path);
        if (!icon.isNull()) return icon;
    }
    return QProxyStyle::standardIcon(sp, option, widget);
}

bool ThemedIconStyle::installOnApplication()
{
    QStyle* current = QApplication::style();
    if (qobject_cast<ThemedIconStyle*>(current)) return false;
    // Wrap a fresh instance of the style in use (Fusion on Linux, windows11 /
    // windowsvista on Windows, macOS on macOS), so nothing but the replaced
    // icons changes. QApplication::setStyle() takes ownership of the proxy,
    // and the proxy owns the base.
    QStyle* base = current ? QStyleFactory::create(current->name()) : nullptr;
    QApplication::setStyle(new ThemedIconStyle(base));
    return true;
}

} // namespace pdfe::ui
