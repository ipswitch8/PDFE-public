// -*- coding: utf-8 -*-
//
// Meta-audit 2026-07 follow-up, phase F13 (SEC-46): the NETWORK / SOCKET
// observation seam.
//
// WHY THIS EXISTS
// ---------------
// Phase-4 of the meta audit established that PDFE's default posture is NO
// outbound network connections: signature-validation fetching is opt-in
// (ValidationCoordinator / ValidationFetcher), form submission is not wired
// into any UI dispatcher, and nothing else in src/ opens a socket during
// open → render → save. It also established that NOTHING ASSERTED IT —
// `grep -c "socket\|UNC" tests/security_oracles.h` returned 0. An unasserted
// default is a claim, not a control: a future feature that adds a telemetry
// ping, a font CDN lookup or an OCSP fetch on the open path would break the
// posture and no test would go red.
//
// This header is the missing observer, modelled on the existing
// ProcessSpawnGuard in security_oracles.h (snapshot → operate → diff).
//
// HOW IT OBSERVES — and what that honestly does and does not catch
// ----------------------------------------------------------------
// Mechanism: the Windows TCP/UDP connection tables, scoped to OUR process id.
//   * GetExtendedTcpTable(TCP_TABLE_OWNER_PID_ALL) for AF_INET and AF_INET6
//   * GetExtendedUdpTable(UDP_TABLE_OWNER_PID)     for AF_INET and AF_INET6
// A baseline snapshot is taken at construction; a background sampler thread
// re-snapshots every few milliseconds while the guard is armed, and the union
// of everything seen minus the baseline is the observed set.
//
// WHY THIS ACTUALLY CATCHES A REAL CONNECTION, not just a theoretical one:
//   * A completed TCP connection does not vanish when the socket closes — the
//     4-tuple lingers in TIME_WAIT (Windows default 2×MSL, minutes) and stays
//     visible in the table long after the operation under test finished. So
//     even a connect/send/close that happens entirely between two samples is
//     still observed by the FINAL snapshot.
//   * A connection that is merely ATTEMPTED and refused appears in SYN_SENT
//     for the duration of the attempt, which is orders of magnitude longer
//     than the sampling interval.
//
// WHAT IT DOES NOT CATCH — stated plainly rather than glossed over:
//   * DNS resolution. On Windows the DNS Client service (dnscache, in a shared
//     svchost) issues the query, so a name lookup by our process produces no
//     socket owned by our pid. A leak that consisted ONLY of encoding data in
//     a hostname would be invisible here.
//   * A raw UDP sendto from a wildcard-bound (0.0.0.0) socket that is opened
//     and closed between two samples. UDP has no connection state to linger.
//     Non-wildcard-bound UDP endpoints ARE classified as egress.
//   * Egress performed by a CHILD process. That is ProcessSpawnGuard's job
//     (tests/security_oracles.h), and the two guards are complementary.
//   * Named-pipe / SMB traffic, which is handled by the separate UNC / path
//     security controls (src/core/PathSecurity.cpp).
//
// A spy that cannot observe is worse than no spy, so the guard is required to
// PROVE it is live: tests/test_default_egress_posture.cpp opens a loopback
// listener, connects to it from the same process, and asserts the guard SAW
// that connection. The loopback-vs-external classification is a pure function
// (isExternalEndpoint) asserted separately against known addresses. Together
// those two facts establish that a real outbound connection would be caught,
// without the test itself ever touching the network.
//
// Header-only, like security_oracles.h, so no CMake source wiring is needed.
// The one build requirement is iphlpapi (linked by tests/CMakeLists.txt).
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <winsock2.h>
#  include <ws2ipdef.h>
#  include <windows.h>
#  include <iphlpapi.h>
#endif

namespace pdfe_test {

/// One observed endpoint owned by this process.
struct NetEndpoint {
    std::string protocol;    ///< "tcp" / "udp"
    std::string localAddr;   ///< dotted-quad or bracketed IPv6 text
    std::uint16_t localPort = 0;
    std::string remoteAddr;  ///< empty for UDP (connectionless)
    std::uint16_t remotePort = 0;
    bool external = false;   ///< true when this counts as OUTBOUND EGRESS

    std::string key() const {
        return protocol + "|" + localAddr + ":" + std::to_string(localPort) +
               "|" + remoteAddr + ":" + std::to_string(remotePort);
    }

    bool operator<(const NetEndpoint& o) const { return key() < o.key(); }
};

/// True when `addr` is a routable peer address, i.e. an address that a packet
/// would actually leave the machine for. Loopback, unspecified/wildcard and
/// the empty string are NOT external.
///
/// Pure function, deliberately separated from the table plumbing so the
/// classification half of the oracle can be asserted directly.
inline bool isExternalEndpoint(const std::string& addr)
{
    if (addr.empty()) return false;
    if (addr == "0.0.0.0" || addr == "::" || addr == "[::]") return false;
    if (addr == "::1" || addr == "[::1]") return false;
    // 127.0.0.0/8
    if (addr.rfind("127.", 0) == 0) return false;
    return true;
}

#if defined(_WIN32)

namespace detail {

inline std::string ipv4Text(DWORD netOrderAddr)
{
    const auto* b = reinterpret_cast<const unsigned char*>(&netOrderAddr);
    return std::to_string(b[0]) + "." + std::to_string(b[1]) + "." +
           std::to_string(b[2]) + "." + std::to_string(b[3]);
}

inline std::string ipv6Text(const UCHAR addr[16])
{
    static const char* kHex = "0123456789abcdef";
    std::string out = "[";
    for (int i = 0; i < 16; i += 2) {
        if (i) out += ':';
        out += kHex[(addr[i] >> 4) & 0xF];
        out += kHex[addr[i] & 0xF];
        out += kHex[(addr[i + 1] >> 4) & 0xF];
        out += kHex[addr[i + 1] & 0xF];
    }
    out += "]";
    // Normalise the two forms the classifier cares about.
    if (out == "[0000:0000:0000:0000:0000:0000:0000:0001]") return "[::1]";
    if (out == "[0000:0000:0000:0000:0000:0000:0000:0000]") return "[::]";
    return out;
}

inline std::uint16_t portFromDword(DWORD p)
{
    // The MIB tables store the port in network byte order in the low word.
    return static_cast<std::uint16_t>(((p & 0xFF) << 8) | ((p >> 8) & 0xFF));
}

/// Snapshot every TCP/UDP endpoint owned by this process, both address
/// families. Returns an empty vector if the tables cannot be read (the guard
/// then reports unsupported rather than silently reporting "clean").
inline bool snapshotEndpoints(std::vector<NetEndpoint>& out)
{
    const DWORD myPid = ::GetCurrentProcessId();
    bool anyTableRead = false;
    std::vector<char> buf;

    auto readTable = [&](bool tcp, ULONG family) {
        ULONG size = 0;
        DWORD rc = tcp
            ? ::GetExtendedTcpTable(nullptr, &size, FALSE, family,
                                    TCP_TABLE_OWNER_PID_ALL, 0)
            : ::GetExtendedUdpTable(nullptr, &size, FALSE, family,
                                    UDP_TABLE_OWNER_PID, 0);
        if (rc != ERROR_INSUFFICIENT_BUFFER && rc != NO_ERROR) return;
        if (size == 0) return;
        buf.assign(size, 0);
        rc = tcp
            ? ::GetExtendedTcpTable(buf.data(), &size, FALSE, family,
                                    TCP_TABLE_OWNER_PID_ALL, 0)
            : ::GetExtendedUdpTable(buf.data(), &size, FALSE, family,
                                    UDP_TABLE_OWNER_PID, 0);
        if (rc != NO_ERROR) return;
        anyTableRead = true;

        if (tcp && family == AF_INET) {
            auto* t = reinterpret_cast<MIB_TCPTABLE_OWNER_PID*>(buf.data());
            for (DWORD i = 0; i < t->dwNumEntries; ++i) {
                const auto& r = t->table[i];
                if (r.dwOwningPid != myPid) continue;
                if (r.dwState == MIB_TCP_STATE_LISTEN) continue;  // inbound
                NetEndpoint e;
                e.protocol   = "tcp";
                e.localAddr  = ipv4Text(r.dwLocalAddr);
                e.localPort  = portFromDword(r.dwLocalPort);
                e.remoteAddr = ipv4Text(r.dwRemoteAddr);
                e.remotePort = portFromDword(r.dwRemotePort);
                e.external   = isExternalEndpoint(e.remoteAddr);
                out.push_back(std::move(e));
            }
        } else if (tcp && family == AF_INET6) {
            auto* t = reinterpret_cast<MIB_TCP6TABLE_OWNER_PID*>(buf.data());
            for (DWORD i = 0; i < t->dwNumEntries; ++i) {
                const auto& r = t->table[i];
                if (r.dwOwningPid != myPid) continue;
                if (r.dwState == MIB_TCP_STATE_LISTEN) continue;
                NetEndpoint e;
                e.protocol   = "tcp";
                e.localAddr  = ipv6Text(r.ucLocalAddr);
                e.localPort  = portFromDword(r.dwLocalPort);
                e.remoteAddr = ipv6Text(r.ucRemoteAddr);
                e.remotePort = portFromDword(r.dwRemotePort);
                e.external   = isExternalEndpoint(e.remoteAddr);
                out.push_back(std::move(e));
            }
        } else if (!tcp && family == AF_INET) {
            auto* t = reinterpret_cast<MIB_UDPTABLE_OWNER_PID*>(buf.data());
            for (DWORD i = 0; i < t->dwNumEntries; ++i) {
                const auto& r = t->table[i];
                if (r.dwOwningPid != myPid) continue;
                NetEndpoint e;
                e.protocol  = "udp";
                e.localAddr = ipv4Text(r.dwLocalAddr);
                e.localPort = portFromDword(r.dwLocalPort);
                // UDP is connectionless: a socket bound to a SPECIFIC routable
                // local address is egress-capable in a way a loopback- or
                // wildcard-bound one is not. See the header's limitations note.
                e.external  = isExternalEndpoint(e.localAddr);
                out.push_back(std::move(e));
            }
        } else {
            auto* t = reinterpret_cast<MIB_UDP6TABLE_OWNER_PID*>(buf.data());
            for (DWORD i = 0; i < t->dwNumEntries; ++i) {
                const auto& r = t->table[i];
                if (r.dwOwningPid != myPid) continue;
                NetEndpoint e;
                e.protocol  = "udp";
                e.localAddr = ipv6Text(r.ucLocalAddr);
                e.localPort = portFromDword(r.dwLocalPort);
                e.external  = isExternalEndpoint(e.localAddr);
                out.push_back(std::move(e));
            }
        }
    };

    readTable(true,  AF_INET);
    readTable(true,  AF_INET6);
    readTable(false, AF_INET);
    readTable(false, AF_INET6);
    return anyTableRead;
}

} // namespace detail

/// Snapshot-diff observer for outbound connections owned by THIS process.
///
/// Usage:
///     pdfe_test::NetworkEgressGuard guard;
///     ... open / render / save ...
///     guard.stop();
///     EXPECT_EQ(guard.externalConnectionCount(), 0) << guard.report();
class NetworkEgressGuard {
public:
    explicit NetworkEgressGuard(std::chrono::milliseconds sampleInterval =
                                    std::chrono::milliseconds(5))
        : interval_(sampleInterval)
    {
        std::vector<NetEndpoint> base;
        supported_ = detail::snapshotEndpoints(base);
        for (auto& e : base) baseline_.insert(e.key());
        if (!supported_) return;
        running_.store(true, std::memory_order_release);
        sampler_ = std::thread([this] { sampleLoop(); });
    }

    ~NetworkEgressGuard() { stop(); }

    NetworkEgressGuard(const NetworkEgressGuard&) = delete;
    NetworkEgressGuard& operator=(const NetworkEgressGuard&) = delete;

    /// Stop sampling and take one FINAL snapshot. Idempotent. The final
    /// snapshot is what catches a short-lived connection that has already
    /// closed: its 4-tuple is still in TIME_WAIT.
    void stop()
    {
        if (!running_.exchange(false, std::memory_order_acq_rel)) {
            if (sampler_.joinable()) sampler_.join();
            return;
        }
        if (sampler_.joinable()) sampler_.join();
        sampleOnce();
    }

    /// True when the OS tables could be read at all. False means the oracle
    /// observed NOTHING and its counts must not be read as "clean".
    bool supported() const { return supported_; }

    /// Every endpoint that appeared after construction (loopback included).
    std::vector<NetEndpoint> newEndpoints() const
    {
        std::lock_guard<std::mutex> lock(mu_);
        return observed_;
    }

    /// Count of new endpoints of any kind. Used by the POSITIVE CONTROL to
    /// prove the observer is live.
    std::size_t newConnectionCount() const
    {
        std::lock_guard<std::mutex> lock(mu_);
        return observed_.size();
    }

    /// Count of new endpoints that constitute OUTBOUND EGRESS. This is the
    /// number the default-posture assertion requires to be zero.
    std::size_t externalConnectionCount() const
    {
        std::lock_guard<std::mutex> lock(mu_);
        std::size_t n = 0;
        for (const auto& e : observed_) if (e.external) ++n;
        return n;
    }

    /// Human-readable dump for the failure message.
    std::string report() const
    {
        std::lock_guard<std::mutex> lock(mu_);
        std::string s = "NetworkEgressGuard: supported=";
        s += supported_ ? "yes" : "NO";
        s += ", new endpoints=" + std::to_string(observed_.size());
        for (const auto& e : observed_) {
            s += "\n  " + e.key() + (e.external ? "  [EXTERNAL]" : "  [local]");
        }
        return s;
    }

private:
    void sampleLoop()
    {
        while (running_.load(std::memory_order_acquire)) {
            sampleOnce();
            std::this_thread::sleep_for(interval_);
        }
    }

    void sampleOnce()
    {
        std::vector<NetEndpoint> now;
        if (!detail::snapshotEndpoints(now)) return;
        std::lock_guard<std::mutex> lock(mu_);
        for (auto& e : now) {
            const std::string k = e.key();
            if (baseline_.count(k)) continue;
            if (!seen_.insert(k).second) continue;
            observed_.push_back(std::move(e));
        }
    }

    mutable std::mutex mu_;
    std::set<std::string> baseline_;
    std::set<std::string> seen_;
    std::vector<NetEndpoint> observed_;
    std::atomic<bool> running_{false};
    std::thread sampler_;
    std::chrono::milliseconds interval_;
    bool supported_ = false;
};

#else  // !_WIN32

/// Non-Windows placeholder. `supported()` is false, so a test must treat its
/// counts as "not observed" rather than "clean" — the guard never manufactures
/// a passing result on a platform where it cannot see.
class NetworkEgressGuard {
public:
    explicit NetworkEgressGuard(std::chrono::milliseconds = std::chrono::milliseconds(5)) {}
    void stop() {}
    bool supported() const { return false; }
    std::vector<NetEndpoint> newEndpoints() const { return {}; }
    std::size_t newConnectionCount() const { return 0; }
    std::size_t externalConnectionCount() const { return 0; }
    std::string report() const {
        return "NetworkEgressGuard: unsupported on this platform (no observation)";
    }
};

#endif // _WIN32

} // namespace pdfe_test
