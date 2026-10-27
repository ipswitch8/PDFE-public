#include "HostAllowlist.h"
#include <algorithm>
#include <cctype>

namespace pdfe::formsubmit {

std::string HostAllowlist::normalize(const std::string& host)
{
    std::string out = host;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    // PORT STRIPPING, AND WHY IT IS NOT `resize(find(':'))`.
    //
    // The old code truncated at the FIRST colon. For an IPv6 literal that is
    // catastrophic rather than merely wrong: `QUrl::host()` returns the address
    // WITHOUT brackets — `::1`, `fe80::1`, `2001:db8::42` — so the first colon
    // is at index 0 or 1 and every IPv6 host normalised to the SAME short
    // prefix, most of them to the empty string. Allowlisting any one IPv6 host
    // therefore allowlisted all of them, and `contains("")` on a list built
    // from a bracketed literal could match a host that was never approved.
    //
    // Three shapes have to be told apart, and a colon count does it:
    //   [::1]:8080  bracketed, optional port   -> strip brackets, drop the rest
    //   ::1         bare IPv6, NEVER has a port (it would be ambiguous)
    //   host:8080   exactly one colon          -> strip the port
    if (!out.empty() && out.front() == '[') {
        const auto close = out.find(']');
        if (close != std::string::npos) {
            out = out.substr(1, close - 1);          // brackets and any :port
        } else {
            out.erase(out.begin());                  // malformed; keep the rest
        }
    } else {
        const auto first = out.find(':');
        if (first != std::string::npos &&
            out.find(':', first + 1) == std::string::npos) {
            out.resize(first);                       // exactly one colon = port
        }
        // Two or more colons and no brackets: a bare IPv6 literal. There is no
        // port to strip, and truncating is what caused the collision above.
    }

    if (!out.empty() && out.back() == '.') out.pop_back();   // trailing dot (IDNA)
    return out;
}

HostAllowlist::HostAllowlist(const std::vector<std::string>& initialHosts)
{
    for (const auto& h : initialHosts) hosts_.insert(normalize(h));
}

bool HostAllowlist::contains(const std::string& host) const
{
    return hosts_.count(normalize(host)) > 0;
}

void HostAllowlist::allow(const std::string& host)
{
    hosts_.insert(normalize(host));
}

void HostAllowlist::revoke(const std::string& host)
{
    hosts_.erase(normalize(host));
}

std::vector<std::string> HostAllowlist::hosts() const
{
    std::vector<std::string> out(hosts_.begin(), hosts_.end());
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace pdfe::formsubmit
