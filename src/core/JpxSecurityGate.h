// -*- coding: utf-8 -*-
//
// JpxSecurityGate.h — PDFE #50 opt-in gate for JPEG2000 (JPX) decoding.
//
// Malformed JPX can heap-overflow the bundled OpenJPEG during decode
// (STATUS_HEAP_CORRUPTION 0xC0000374, potential RCE), and MuPDF's setjmp-based
// fz_try/fz_catch cannot trap heap corruption. PDFE therefore ships with JPX
// decoding DISABLED BY DEFAULT: a patched MuPDF (vcpkg-overlay-ports/libmupdf,
// 0001-pdfe-jpx-decode-gate.patch) checks a global flag at the entry of
// fz_load_jpx / fz_load_jpx_info and returns a 1x1 placeholder WITHOUT entering
// openjpeg while the flag is off. A user preference opts in (see the JPX
// preference wiring); enabling re-accepts the decode risk on untrusted files.
//
// These are the raw MuPDF-side C hooks defined by the patch. Prefer the thin
// C++ wrappers in namespace pdfe::core below.

#pragma once

#include <cstdint>

extern "C" {
// Defined in the patched MuPDF source/fitz/load-jpx.c (always linkable — the
// definitions live outside the FZ_ENABLE_JPX guard). Thread-safety: a plain int
// flag set from the UI thread at startup / on preference change; reads race
// benignly (a torn read of an int is not possible on the supported platforms and
// the only values are 0/1).
void fz_pdfe_set_jpx_enabled(int enabled);
int fz_pdfe_get_jpx_enabled(void);

// F39-c: the compiled-in decode-time pixel-count bound (FZ_PDFE_JPX_MAX_PIXELS).
// Exposed as an accessor rather than a header constant so a test reads the value
// the LINKED library actually enforces — a duplicated constant in a PDFE header
// would agree with itself even if the overlay patch had been dropped by a
// dependency bump. Also defined outside the FZ_ENABLE_JPX guard.
std::int64_t fz_pdfe_jpx_max_pixels(void);
}

namespace pdfe::core {

// Enable/disable JPEG2000 decoding process-wide. Default is DISABLED.
inline void setJpxDecodingEnabled(bool enabled)
{
    fz_pdfe_set_jpx_enabled(enabled ? 1 : 0);
}

// Whether JPEG2000 decoding is currently enabled (default false).
inline bool isJpxDecodingEnabled()
{
    return fz_pdfe_get_jpx_enabled() != 0;
}

// F39-c: the maximum pixel count (width * height) the patched MuPDF will decode
// on the JPX path. Enforced on the dimensions parsed from the codestream header,
// BEFORE OpenJPEG allocates component buffers, so an over-large declaration
// never produces the allocation. Reachable only while the gate above is on;
// while it is off, fz_load_jpx returns the 1x1 placeholder and never gets there.
//
// The value is 600,000,000 px, chosen as the largest legitimate large-format
// scan rounded up — ISO A0 at 600 dpi is 19866 x 28087 = 557,976,342 px, i.e.
// 93.0% of the bound. Full arithmetic in the patch's comment and in
// docs/audit/meta-audit-2026-07/followup/phase-47-dispositions.md.
inline std::int64_t jpxMaxDecodePixels()
{
    return fz_pdfe_jpx_max_pixels();
}

}  // namespace pdfe::core
