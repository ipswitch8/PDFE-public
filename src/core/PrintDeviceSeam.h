// -*- coding: utf-8 -*-
// Pipeline p84-remaining-open phase-5 (GitHub #78 / gap-register WIN-28):
// test-only injection seam for Windows printer ENUMERATION.
//
// WHY A SEAM AND NOT A STOPPED SPOOLER
// ------------------------------------
// WIN-28's proposed work reads "on a VM with the print spooler stopped, invoke
// print and assert a clean user-facing error [manual]". Stopping the Windows
// Print Spooler service is machine-wide and requires elevation, so it is not
// something a test run may do to a developer's or a CI runner's host — and the
// audit's own evidence line is about coverage, not about a VM: "no test in
// tests/ references printer enumeration or an empty-printer-list condition".
//
// The OBSERVABLE that a stopped spooler produces is precise and small. With the
// Spooler service stopped, winspool's EnumPrinters returns no devices, so
// QPrinterInfo::availablePrinters() comes back EMPTY and QPrinter's native
// back-end has nothing to bind. That empty list is the whole of what PDFE can
// see, and it is identical to the observable on a machine that simply has no
// printers installed. This seam injects exactly that observable — an empty
// available-printer list — so the failure path is exercised deterministically,
// unelevated, on any host, whether or not it happens to have printers.
//
// POLARITY / SHIPPING POSTURE — same convention as src/ui/TestDialogSeam.h.
// `PDFE_TESTING` is `option(... ON)` in the top-level CMakeLists, so
// kPrintDeviceSeamCompiledIn is TRUE by default and FALSE only in a tree
// configured with -DPDFE_TESTING=OFF (the `windows-vs2022-package` preset,
// which is what scripts/sign/Build-Release-Signed.ps1 packages). The override
// is read behind `if constexpr`, so in an OFF-configured tree the override
// storage is never consulted and availablePrinterCount() is unconditionally the
// real QPrinterInfo enumeration. Nothing new ships in a packaged build.
#pragma once

#include <QPrinterInfo>

namespace pdfe::core::testseam {

inline constexpr bool kPrintDeviceSeamCompiledIn =
#ifdef PDFE_TESTING
    true;
#else
    false;
#endif

/// Process-wide override for the installed-printer count.
/// -1 (the default) means "no override — ask the real Windows spooler".
/// >= 0 means "pretend the enumeration returned exactly this many devices";
/// 0 is the stopped-spooler / no-printers-installed observable.
inline int& printerCountOverride()
{
    static int value = -1;
    return value;
}

/// Force the next enumeration to report @p count devices. No-op in an
/// OFF-configured (packaged) tree.
inline void setPrinterCountOverride(int count)
{
    if constexpr (kPrintDeviceSeamCompiledIn) {
        printerCountOverride() = count;
    } else {
        (void)count;
    }
}

/// Drop any override so enumeration returns to the real spooler.
inline void clearPrinterCountOverride()
{
    if constexpr (kPrintDeviceSeamCompiledIn) {
        printerCountOverride() = -1;
    }
}

/// The number of print devices PDFE can see. This is the single call site the
/// engine's no-printer guard consults.
inline int availablePrinterCount()
{
    if constexpr (kPrintDeviceSeamCompiledIn) {
        const int override = printerCountOverride();
        if (override >= 0) {
            return override;
        }
    }
    return static_cast<int>(QPrinterInfo::availablePrinters().size());
}

/// RAII helper for tests: install an override for a scope and restore the
/// previous value (usually "no override") on the way out, even on a failed
/// assertion that returns early.
class ScopedPrinterCount {
public:
    explicit ScopedPrinterCount(int count) : previous_(printerCountOverride())
    {
        setPrinterCountOverride(count);
    }
    ~ScopedPrinterCount() { printerCountOverride() = previous_; }

    ScopedPrinterCount(const ScopedPrinterCount&) = delete;
    ScopedPrinterCount& operator=(const ScopedPrinterCount&) = delete;

private:
    int previous_;
};

} // namespace pdfe::core::testseam
