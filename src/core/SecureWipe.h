#pragma once

// Optimizer-resistant memory wipe — ONE definition, shared.
//
// WHY THIS HEADER EXISTS (phase-11). The wipe was a `static inline secureWipe`
// buried in src/encryption/EncryptionManager.cpp, reachable only from that one
// translation unit. The 2026-08-10 reverification found the password path had
// no wipe at all while noting, correctly, that "the project *has* an
// optimizer-resistant wipe ... applied to encryption intermediates only".
// A security primitive that is private to the file that first needed it gets
// re-derived — slightly differently, and usually wrongly — by the next caller.
// That is the sibling-drift shape DCA 0015 records against InPlaceWriteBlocker.
//
// The correctness point, which is the whole reason this is not `std::fill`:
// a plain memset/std::fill over a buffer that is never read again is a DEAD
// STORE, and the compiler is entitled to delete it under /O2. SecureZeroMemory
// is documented by Microsoft to survive that. The non-Windows fallback writes
// through a `volatile` pointer, which the standard requires be emitted.

#include <cstddef>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

namespace pdfe::core {

/// Overwrite `len` bytes at `data` with zeroes in a way the optimizer may not
/// elide. Safe on (nullptr, 0). Never throws.
inline void secureWipe(void* data, std::size_t len) noexcept
{
    if (data == nullptr || len == 0) return;
#ifdef _WIN32
    ::SecureZeroMemory(data, len);
#else
    volatile unsigned char* p = static_cast<volatile unsigned char*>(data);
    while (len--) { *p++ = 0; }
#endif
}

}  // namespace pdfe::core
