#pragma once
#include <vector>
#include <cstdint>

namespace pdfe::compliance {

/// Returns a minimal valid sRGB IEC61966-2.1 ICC v2 profile.
/// This is generated programmatically to avoid storing binary blobs in headers.
/// The profile includes all required tags for a basic RGB monitor profile:
/// - desc, wtpt, rXYZ, gXYZ, bXYZ, rTRC, gTRC, bTRC, cprt
std::vector<uint8_t> generateSrgbIccProfile();

} // namespace pdfe::compliance
