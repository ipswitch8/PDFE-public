// -*- coding: utf-8 -*-
//
// VersionInfo implementation — see VersionInfo.h for the R3-16 rationale and
// the version source-of-truth contract.

#include "app/VersionInfo.h"

#include <cctype>
#include <cstddef>
#include <vector>

// Version source of truth: PROJECT_VERSION, threaded from the top-level
// project(pdfe VERSION 0.3.0 ...) via a target_compile_definitions on
// pdfe_app. The literal fallback MUST equal that project() version; the unit
// test cross-checks currentVersion() against the PDFE_VERSION_STRING macro so
// any drift is caught at test time.
#ifndef PDFE_VERSION_STRING
#define PDFE_VERSION_STRING "0.3.0"
#endif

namespace pdfe::app {

namespace {

// Parse a single dotted segment into a non-negative integer, guarding against
// non-numeric input. Uses the leading run of decimal digits; a digit-less
// segment yields 0. Leading zeros are absorbed naturally by base-10 accumulation.
long long segmentToNumber(const std::string& seg) {
    long long value = 0;
    bool sawDigit = false;
    for (char c : seg) {
        if (c >= '0' && c <= '9') {
            sawDigit = true;
            value = value * 10 + static_cast<long long>(c - '0');
            // Clamp to avoid overflow on absurd input; real versions are tiny.
            if (value > 1000000000LL) {
                value = 1000000000LL;
            }
        } else {
            // First non-digit ends the numeric prefix of this segment.
            break;
        }
    }
    (void)sawDigit;
    return value;
}

// Split a version string into its core numeric segments. An optional leading
// 'v'/'V' is skipped; a '-' or '+' terminates the core version (pre-release /
// build metadata is ignored). Empty input yields a single 0 segment.
std::vector<long long> parseSegments(const std::string& v) {
    std::vector<long long> out;
    std::size_t i = 0;
    const std::size_t n = v.size();
    if (n > 0 && (v[0] == 'v' || v[0] == 'V')) {
        i = 1;
    }
    std::string cur;
    for (; i < n; ++i) {
        const char c = v[i];
        if (c == '-' || c == '+') {
            break;  // pre-release / build-metadata suffix: not part of core version
        }
        if (c == '.') {
            out.push_back(segmentToNumber(cur));
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    out.push_back(segmentToNumber(cur));
    return out;
}

}  // namespace

std::string currentVersion() {
    return std::string(PDFE_VERSION_STRING);
}

UpdateStatus compareVersions(const std::string& current, const std::string& latest) {
    const std::vector<long long> a = parseSegments(current);
    const std::vector<long long> b = parseSegments(latest);
    const std::size_t count = a.size() > b.size() ? a.size() : b.size();
    for (std::size_t i = 0; i < count; ++i) {
        const long long av = i < a.size() ? a[i] : 0;  // zero-pad shorter
        const long long bv = i < b.size() ? b[i] : 0;
        if (av < bv) {
            return UpdateStatus::UpdateAvailable;  // current older than latest
        }
        if (av > bv) {
            return UpdateStatus::Newer;  // current ahead of latest
        }
    }
    return UpdateStatus::UpToDate;
}

UpdateStatus checkForUpdate(const std::string& latestFromChannel) {
    return compareVersions(currentVersion(), latestFromChannel);
}

}  // namespace pdfe::app
