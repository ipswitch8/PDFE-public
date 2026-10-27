#pragma once

// Internal (non-public) helper for tile rasterization, factored out of
// TileRenderer::RenderTask::run() so the exact production raster sequence can
// be exercised directly by an allocation-balance leak oracle
// (tests/test_tile_renderer_leak.cpp, remediation R2-05).
//
// MuPDF uses setjmp/longjmp for error handling — this function is PURE C
// MuPDF (no C++ objects with non-trivial destructors on the stack inside
// fz_try/fz_catch).
#include <mupdf/fitz.h>

namespace pdfe::rendering::detail {

/// Render a display list into a fresh TILE_SIZE x TILE_SIZE RGBA pixmap.
///
/// Exception-safety (R2-05): if MuPDF `fz_throw`s during rasterization — e.g.
/// a corrupt embedded image whose decode fails inside `fz_run_display_list` —
/// BOTH the draw device AND the half-built pixmap are freed before the error
/// is rethrown, so no allocation leaks. Tiles are re-requested on every
/// scroll/zoom, so a per-throw device+pixmap leak (~1 MB/tile) is unbounded.
///
/// On success returns the pixmap; the caller owns it and must `fz_drop_pixmap`.
/// On an fz_throw the function frees everything and rethrows (never returns a
/// leaked or partial pixmap).
fz_pixmap* renderTileToPixmap(fz_context* ctx, fz_display_list* list, fz_matrix ctm);

} // namespace pdfe::rendering::detail
