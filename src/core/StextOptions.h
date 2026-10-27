// -*- coding: utf-8 -*-
#pragma once

#include <mupdf/fitz.h>

namespace pdfe {
namespace core {

// Shared structured-text (stext) extraction options for PDFE's text pipeline.
//
// PDFE ALWAYS sets FZ_STEXT_IGNORE_ACTUALTEXT on stext extraction, for two
// reasons:
//
//  1. Correct editor semantics. Extraction feeds search / Replace All / in-place
//     text editing / hasText probes, all of which must operate on the DISPLAYED
//     glyphs the user sees and edits — NOT the /ActualText accessibility
//     override. A span that shows "ABC" but carries /ActualText (A) must extract
//     as "ABC"; extracting "A" would make search/replace/edit target the wrong
//     characters.
//
//  2. Crash avoidance (bugfix-3e91a7c4 #6). MuPDF 1.26.10
//     do_extract_within_actualtext (source/fitz/stext-device.c:1142) underflows
//     its unsigned running actualtext length `z` when /ActualText is a proper
//     prefix of the displayed span: `z` reaches 0 while the span still has
//     unconsumed items, tripping assert(z != 0) (Debug abort()) and then
//     rune_index(actualtext, z - 1) with z - 1 == SIZE_MAX (Release
//     out-of-bounds read => "Access Violation: read address 0x0"). Ignoring
//     ActualText bypasses that code path entirely. See
//     tests/docs/bugfix-3e91a7c4-phase1-findings.md and
//     tests/test_actualtext_extraction_crash.cpp.
//
// Pass any additional FZ_STEXT_* flags (e.g. FZ_STEXT_PRESERVE_IMAGES) via
// extraFlags; FZ_STEXT_IGNORE_ACTUALTEXT is always OR-ed in. Prefer this helper
// over constructing fz_stext_options directly so no new extraction site can
// silently reintroduce the crashing ActualText path.
inline fz_stext_options pdfeStextOptions(int extraFlags = 0) {
    fz_stext_options opts{};
    opts.flags = extraFlags | FZ_STEXT_IGNORE_ACTUALTEXT;
    return opts;
}

}  // namespace core
}  // namespace pdfe
