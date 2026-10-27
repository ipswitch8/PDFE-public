#pragma once

namespace pdfe::tre {

/// Atlas rendering mode for scanned/image PDF text editing.
/// Controls how glyphs are sourced for both overlay display and save.
enum class AtlasMode {
    ScanCropped,     ///< Glyphs cropped from scan image + synthesis + FreeType fallback
    FreeTypeStyled   ///< All glyphs from FreeType, post-processed with ScanStyleTransfer
};

} // namespace pdfe::tre
