// -*- coding: utf-8 -*-
//
// Jbig2SecurityGate.h — PDFE SEC-56 (GitHub #56) opt-in gate for JBIG2 decoding.
//
// jbig2dec has a long history of memory-safety CVEs in its generic-region and
// symbol-dictionary decoders (CVE-2016-9601, CVE-2017-7885 / -7975 / -9216,
// CVE-2020-12268, CVE-2023-46361 and others), and — exactly as for JPEG2000
// (#50, JpxSecurityGate.h) — MuPDF's setjmp-based fz_try/fz_catch cannot trap a
// heap corruption once it has occurred. PDFE therefore ships with JBIG2
// decoding DISABLED BY DEFAULT: a patched MuPDF
// (vcpkg-overlay-ports/libmupdf, 0002-pdfe-jbig2-decode-gate.patch) checks a
// global flag and, while it is off,
//
//   * substitutes the PDF /JBIG2Decode stream filter with a blank (all-1 bits,
//     i.e. ink-free) placeholder stream WITHOUT constructing a jbig2dec context,
//   * refuses to parse the attacker-controlled /JBIG2Globals stream, and
//   * returns a 1x1 gray placeholder from the standalone JBIG2 image loader.
//
// A user preference opts in (Preferences ▸ Security, warn-once); enabling
// re-accepts the decode risk on untrusted files.
//
// These are the raw MuPDF-side C hooks defined by the patch. Prefer the thin
// C++ wrappers in namespace pdfe::core below.

#pragma once

extern "C" {
// Defined in the patched MuPDF source/fitz/filter-jbig2.c (always linkable —
// neither jbig2 source file sits behind a feature guard). Thread-safety: a
// plain int flag set from the UI thread at startup / on preference change;
// reads race benignly (the only values are 0/1).
void fz_pdfe_set_jbig2_enabled(int enabled);
int fz_pdfe_get_jbig2_enabled(void);

// SEC-56 test oracle. Incremented once per placeholder substitution — by the
// gated /JBIG2Decode filter and by the gated standalone loader alike.
//
// This counter exists because "rendering did not crash" CANNOT distinguish a
// held gate from a regressed one: the corruption jbig2dec is gated against is
// probabilistic and input-dependent, so a real decode of a benign fixture is
// indistinguishable from a refused one if all you assert is that the call
// returned. Reading the counter proves the placeholder — not jbig2dec —
// produced the pixels. It is a monotonically increasing process-wide int;
// tests snapshot it, act, and compare (or reset first).
int fz_pdfe_jbig2_placeholder_count(void);
void fz_pdfe_reset_jbig2_placeholder_count(void);
}

namespace pdfe::core {

// Enable/disable JBIG2 decoding process-wide. Default is DISABLED.
inline void setJbig2DecodingEnabled(bool enabled)
{
    fz_pdfe_set_jbig2_enabled(enabled ? 1 : 0);
}

// Whether JBIG2 decoding is currently enabled (default false).
inline bool isJbig2DecodingEnabled()
{
    return fz_pdfe_get_jbig2_enabled() != 0;
}

// How many times the patched MuPDF has substituted a JBIG2 placeholder for a
// real decode in this process. See the C declaration above for why this, and
// not a "did not crash" assertion, is the oracle for the gate.
inline int jbig2PlaceholderCount()
{
    return fz_pdfe_jbig2_placeholder_count();
}

inline void resetJbig2PlaceholderCount()
{
    fz_pdfe_reset_jbig2_placeholder_count();
}

}  // namespace pdfe::core
