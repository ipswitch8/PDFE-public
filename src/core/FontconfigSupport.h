// -*- coding: utf-8 -*-
// fontconfig-backed system font discovery for Linux and other non-Windows,
// non-Apple platforms (GitHub #390). Windows uses DirectWrite
// (SystemFontDiscovery.cpp); this is the POSIX counterpart it never had.
#pragma once

#if !defined(_WIN32)

#include "FontMetrics.h"

#include <string>
#include <vector>

namespace pdfe::core::fontconfig {

/// Every scalable face fontconfig knows about, as FontMetrics. Metric fields
/// that fontconfig does not expose (ascender, xHeight, ...) are left at their
/// defaults; callers needing them read the file.
std::vector<FontMetrics> listFonts();

/// The file for `family` in the requested weight/slant, or "" if the system has
/// neither that family nor a configured substitute for it.
///
/// fontconfig ALWAYS returns a match, falling back to a generic face (usually
/// DejaVu Sans) for a family it has never heard of. Accepting that would make
/// "is this font available?" answer yes for every name, so a match is accepted
/// only when its family is the requested one or one of the substitutes the
/// configuration binds to it, such as the metric aliases that map Arial to
/// Liberation Sans. Those are the families fontconfig places ahead of the
/// generic sans-serif/serif/monospace fallback in the substituted pattern.
std::string matchFamilyFile(const std::string& family, bool bold, bool italic);

}  // namespace pdfe::core::fontconfig

#endif  // !_WIN32
