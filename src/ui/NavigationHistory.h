#pragma once

#include <vector>

namespace pdfe::ui {

/// Browser-style back / forward page-view history. Each time the
/// user navigates to a new page (goToPage, bookmark click, link
/// follow, search jump), the caller pushes the target page index
/// via recordVisit(). goBack() / goForward() return the page to
/// navigate to, or -1 when the stack edge is reached.
///
/// Navigation triggered BY goBack/goForward must not itself record
/// a new entry. Callers use the isReplaying() flag (or suppress
/// recording while invoking the return value) to avoid stack
/// corruption.
class NavigationHistory {
public:
    /// Record a page visit. If we're currently replaying (back/forward),
    /// this is a no-op so the history stays consistent. Truncates any
    /// forward history when recording after a back.
    void recordVisit(int pageIndex);

    /// Return the page to navigate to for "Previous View", or -1 if
    /// no history is available. Moves the cursor back one step.
    int goBack();

    /// Return the page to navigate to for "Next View", or -1 if no
    /// forward history is available. Moves the cursor forward one.
    int goForward();

    /// True when a back() or forward() is in progress. Callers
    /// should consult this to skip recordVisit during that window.
    [[nodiscard]] bool isReplaying() const { return replaying_; }

    /// Can the back action be invoked?
    [[nodiscard]] bool canGoBack() const {
        return cursor_ > 0;
    }

    /// Can the forward action be invoked?
    [[nodiscard]] bool canGoForward() const {
        return cursor_ + 1 < static_cast<int>(history_.size());
    }

    /// Clear all history (e.g., when a new document is opened).
    void clear() {
        history_.clear();
        cursor_ = -1;
        replaying_ = false;
    }

    /// Number of recorded entries (exposed for testing).
    [[nodiscard]] int size() const { return static_cast<int>(history_.size()); }

    /// Current cursor position (exposed for testing). -1 when empty.
    [[nodiscard]] int cursor() const { return cursor_; }

    /// Mark the start / end of a replay window explicitly. The
    /// goBack / goForward methods toggle this automatically around
    /// their return, but callers that need to suppress multiple
    /// nested recordings (for example when a goToPage slot fires
    /// downstream signals that would otherwise recurse) can guard
    /// with setReplaying(true) / setReplaying(false).
    void setReplaying(bool r) { replaying_ = r; }

private:
    std::vector<int> history_;
    int cursor_ = -1;  // index into history_ of the "current" view
    bool replaying_ = false;
};

} // namespace pdfe::ui
