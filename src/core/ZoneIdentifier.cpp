// -*- coding: utf-8 -*-
#include "core/ZoneIdentifier.h"

#include <fstream>
#include <sstream>

namespace pdfe::core {

std::optional<std::string> readZoneIdentifier(
    [[maybe_unused]] const std::filesystem::path& file)
{
#if defined(_WIN32)
    // The ADS is addressed by suffixing the stream name to the path. Opened in
    // binary so the CRLF the stream conventionally contains is preserved
    // byte-for-byte -- rewriting it as LF would change a file the shell parses.
    const std::wstring ads = file.wstring() + L":Zone.Identifier";
    std::ifstream in(ads, std::ios::binary);
    if (!in) {
        return std::nullopt;   // no stream, or no stream support here
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string content = ss.str();
    if (content.empty()) {
        return std::nullopt;   // an empty stream marks nothing
    }
    return content;
#else
    return std::nullopt;       // no such concept off Windows
#endif
}

ZonePropagation propagateZoneIdentifier(
    [[maybe_unused]] const std::filesystem::path& from,
    [[maybe_unused]] const std::filesystem::path& to)
{
#if defined(_WIN32)
    const auto mark = readZoneIdentifier(from);
    if (!mark.has_value()) {
        // The source is not internet-sourced. Deliberately NOT stamping the
        // destination: see the header for why marking everything would make
        // the mark meaningless.
        return ZonePropagation::NoSourceMark;
    }

    const std::wstring ads = to.wstring() + L":Zone.Identifier";
    std::ofstream out(ads, std::ios::binary | std::ios::trunc);
    if (!out) {
        return ZonePropagation::Failed;
    }
    out.write(mark->data(), static_cast<std::streamsize>(mark->size()));
    if (!out) {
        return ZonePropagation::Failed;
    }
    out.close();
    return out ? ZonePropagation::Copied : ZonePropagation::Failed;
#else
    return ZonePropagation::NoSourceMark;
#endif
}

}  // namespace pdfe::core
