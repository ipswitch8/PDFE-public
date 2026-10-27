// -*- coding: utf-8 -*-
#include "NavigationHistory.h"

namespace pdfe::ui {

void NavigationHistory::recordVisit(int pageIndex)
{
    if (replaying_) return;
    if (pageIndex < 0) return;

    // Coalesce consecutive duplicates — rapidly re-selecting the
    // same page (e.g. clicking the same thumbnail twice) shouldn't
    // spam the back stack.
    if (cursor_ >= 0 && cursor_ < static_cast<int>(history_.size())
        && history_[cursor_] == pageIndex) {
        return;
    }

    // Truncate any forward history beyond the cursor: recording a
    // fresh visit while we have forward entries discards them,
    // matching browser behavior.
    if (cursor_ + 1 < static_cast<int>(history_.size())) {
        history_.erase(history_.begin() + cursor_ + 1, history_.end());
    }

    history_.push_back(pageIndex);
    cursor_ = static_cast<int>(history_.size()) - 1;
}

int NavigationHistory::goBack()
{
    if (!canGoBack()) return -1;
    --cursor_;
    return history_[cursor_];
}

int NavigationHistory::goForward()
{
    if (!canGoForward()) return -1;
    ++cursor_;
    return history_[cursor_];
}

} // namespace pdfe::ui
