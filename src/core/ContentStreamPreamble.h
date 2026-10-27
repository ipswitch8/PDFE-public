#pragma once

// Shared "reset-to-known-basis" preamble for PDF page-content fragments.
//
// Injectors that APPEND a content-stream fragment to a page's existing
// /Contents (AlignmentWriter, HeaderFooterEngine, BatchEngine watermark, TRE
// writeTextBlock) run in a stream that PDF concatenates onto the preceding page
// content with NO graphics-state reset. Ambient text/colour state left by the
// page therefore leaks INTO the fragment: a non-black ambient fill hides the
// text, an ambient "-100 Tz" mirrors it, an ambient Tc/Tw/Ts distorts spacing
// and baseline. (The audit's per-site details are in
// docs/content-stream-state-continuity-audit.md, Register H1.)
//
// This is the single place that defines the known basis. Emit it immediately
// after the fragment's `q` (which bounds the OUTBOUND direction) so the text is
// drawn against predictable state regardless of the page's ambient graphics
// state.
//
// NOTE on CTM: this preamble intentionally does NOT emit a `cm`. A `cm`
// multiplies the current transform rather than replacing it, so it cannot
// neutralise an arbitrary ambient CTM. Callers that require CTM isolation
// (absolute image placement) must handle that separately.
//
// The filter-based textedit/ContentStreamEditor establishes the same basis by
// calling the equivalent pdf_processor ops (op_Tz/op_Tc/op_Tw/op_Ts/op_rg)
// directly on its chain; keep the two in sync.

#include <cstdint>
#include <cstdio>
#include <string>

namespace pdfe::core {

/// Content-stream operators that reset fill colour, horizontal text scale,
/// character/word spacing, text rise and render mode to a known basis.
/// `rgb` is the fill colour to establish (default black 0x000000).
inline std::string knownTextStateBasis(std::uint32_t rgb = 0x000000u)
{
    const double r = ((rgb >> 16) & 0xFFu) / 255.0;
    const double g = ((rgb >> 8) & 0xFFu) / 255.0;
    const double b = (rgb & 0xFFu) / 255.0;
    char buf[96];
    std::snprintf(buf, sizeof(buf),
                  "%.6g %.6g %.6g rg\n100 Tz\n0 Tc\n0 Tw\n0 Ts\n0 Tr\n",
                  r, g, b);
    return std::string(buf);
}

} // namespace pdfe::core
