#pragma once

#include <string>

namespace pdfe::annotations {

/// Strongly-typed description of a PDF link action. PDF supports a
/// richer action dictionary than just URI — most importantly GoTo
/// (internal page navigation) and Named (standard actions like
/// NextPage, PrevPage, FirstPage, LastPage, Print, Save, ZoomIn,
/// ZoomOut). Launch is intentionally omitted for now: it lets a PDF
/// execute arbitrary host-side files, which is a sharp security
/// edge we don't want to expose through the UI without explicit
/// sandboxing design.
enum class LinkActionType {
    Uri,    ///< External URL, mailto:, tel:, etc.
    GoTo,   ///< Jump to an absolute page within the same document.
    Named,  ///< Execute a named action (NextPage, Print, ...).
    Launch, ///< Open an external file via the OS shell.
};

struct LinkAction {
    LinkActionType type = LinkActionType::Uri;

    /// Populated for Uri actions; any well-formed URI string.
    std::string uri;

    /// Populated for GoTo actions; 0-based page index.
    int pageIndex = 0;

    /// Populated for Named actions. Accepted values per PDF spec:
    /// NextPage, PrevPage, FirstPage, LastPage, Print, GoBack,
    /// GoForward, Find, FullScreen, ZoomIn, ZoomOut, SaveAs.
    std::string namedAction;

    /// Populated for Launch actions. Path to the external file the
    /// link opens (encoded as /F in the /A dict). Security note: the
    /// PDFE viewer deliberately does NOT auto-execute Launch actions
    /// on click — supporting the *writing* of these links is parity
    /// with Acrobat's link editor, but any future follow-link UI
    /// should prompt the user before invoking the OS shell. A
    /// malicious launch path is the classic PDF-as-malware vector.
    std::string launchPath;

    [[nodiscard]] bool isValid() const {
        switch (type) {
        case LinkActionType::Uri:    return !uri.empty();
        case LinkActionType::GoTo:   return pageIndex >= 0;
        case LinkActionType::Named:  return !namedAction.empty();
        case LinkActionType::Launch: return !launchPath.empty();
        }
        return false;
    }
};

} // namespace pdfe::annotations
