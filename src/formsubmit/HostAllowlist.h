#pragma once

#include <string>
#include <unordered_set>
#include <vector>

namespace pdfe::formsubmit {

/// Per-AppSettings host allowlist for /SubmitForm actions. The policy
/// is "confirm on first use, allow afterwards" — this class supplies
/// the raw containment check; the UI layer handles the confirmation
/// prompt and calls allow() on approval.
class HostAllowlist {
public:
    HostAllowlist() = default;
    explicit HostAllowlist(const std::vector<std::string>& initialHosts);

    /// Lowercase the input (hosts are case-insensitive) and check
    /// membership.
    bool contains(const std::string& host) const;

    /// Add `host` to the allowlist.
    void allow(const std::string& host);

    /// Remove `host`.
    void revoke(const std::string& host);

    std::vector<std::string> hosts() const;

    static std::string normalize(const std::string& host);

private:
    std::unordered_set<std::string> hosts_;
};

} // namespace pdfe::formsubmit
