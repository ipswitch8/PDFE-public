// -*- coding: utf-8 -*-
// Where a test finds a system font, on every platform (GitHub #390).
//
// The suite names its fonts the way Windows does: "arial.ttf", "times.ttf",
// "msgothic.ttc", read from C:\Windows\Fonts. On Linux there is no such
// directory, so every one of those tests failed or skipped there. This maps each
// Windows font file to the font a Linux desktop actually ships for it, and asks
// fontconfig where that font is.
//
//   * Windows: unchanged. The same C:\Windows\Fonts\<file> path as before.
//   * POSIX: a fontconfig match for the font's metric-compatible substitute
//     (Arial -> Liberation Sans, Times New Roman -> Liberation Serif, ...). The
//     match is ACCEPTED only if fontconfig returns one of the families listed
//     for it. fontconfig always returns *something*, usually DejaVu Sans, and a
//     test that asked for Arial's metrics must not silently get DejaVu's. A
//     font with no acceptable substitute yields a path that does not exist, so
//     the test fails or skips exactly as it did when the Windows file was
//     missing, rather than passing against the wrong font.
#pragma once

#include <string>

namespace pdfe_test {

/// Path of the system font that Windows calls `windowsFile` (case-insensitive,
/// e.g. "arial.ttf"). On POSIX the result may be a .ttc/.otf of a substitute
/// family. When no acceptable font is installed, returns a path that does not
/// exist (callers already test std::filesystem::exists()).
std::string systemFontPath(const std::string& windowsFile);

/// The family name of the font `systemFontPath(windowsFile)` resolves to, or
/// "" if none. Lets a test that asserts on names compare against the font it
/// actually got.
std::string systemFontFamily(const std::string& windowsFile);

/// True when `path` (any case) names the Windows core font whose file stem is
/// `windowsStem` ("arial", "times", "cour"), or, off Windows, one of the
/// metric-compatible faces fontconfig's alias rules name for it (Liberation,
/// croscore, URW base-35). A generic fallback such as DejaVu or Noto is never
/// accepted, so "the known pair / metric substitute was taken" stays testable.
bool isMetricCompatibleFont(const std::string& path, const std::string& windowsStem);

}  // namespace pdfe_test
