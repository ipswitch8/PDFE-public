#include "ProcessMitigations.h"

#ifdef Q_OS_WIN
#include <QtGlobal>
#endif

#include <QDebug>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <processthreadsapi.h>

// SetProcessMitigationPolicy / GetProcessMitigationPolicy are exposed
// via processthreadsapi.h (Windows 8+). The struct definitions live in
// winnt.h via windows.h. We dynamically resolve the entry points so a
// stripped-down Win 7 build (if anyone ever runs PDFE that far back)
// gracefully no-ops instead of failing to start.
using SetProcessMitigationPolicy_t = BOOL (WINAPI*)(
    PROCESS_MITIGATION_POLICY, PVOID, SIZE_T);
using GetProcessMitigationPolicy_t = BOOL (WINAPI*)(
    HANDLE, PROCESS_MITIGATION_POLICY, PVOID, SIZE_T);
#endif

namespace pdfe::app {

#ifdef _WIN32
namespace {

template <typename PolicyT>
void applyOne(SetProcessMitigationPolicy_t setFn,
              PROCESS_MITIGATION_POLICY policyKind,
              const PolicyT& policy,
              const char* name) noexcept {
    if (!setFn) return;
    BOOL ok = setFn(policyKind,
                    const_cast<PolicyT*>(&policy),
                    sizeof(policy));
    if (!ok) {
        DWORD err = GetLastError();
        // ERROR_ACCESS_DENIED (5): "already irrevocably set with
        // stronger flags than we're requesting" — SUCCESS for our
        // purposes.
        // ERROR_NOT_SUPPORTED (50): on 64-bit Windows, DEP cannot be
        // set via SetProcessMitigationPolicy because DEP is
        // permanently on for x64 binaries. The kernel rejects the
        // call but the policy is still in effect at full strength.
        // Same logic for any policy the OS deems always-on for this
        // bitness — treat as success.
        if (err == ERROR_ACCESS_DENIED || err == ERROR_NOT_SUPPORTED) {
            qInfo() << "ProcessMitigations:" << name
                    << "already enforced by OS (GLE=" << err
                    << "). OK.";
            return;
        }
        qWarning() << "ProcessMitigations:" << name
                   << "failed, GetLastError=" << err;
    } else {
        qInfo() << "ProcessMitigations:" << name << "applied.";
    }
}

template <typename PolicyT>
void queryOne(GetProcessMitigationPolicy_t getFn,
              PROCESS_MITIGATION_POLICY policyKind,
              PolicyT& policy,
              const char* name) noexcept {
    if (!getFn) return;
    BOOL ok = getFn(GetCurrentProcess(), policyKind,
                    &policy, sizeof(policy));
    if (!ok) {
        qWarning() << "ProcessMitigations: query" << name
                   << "failed, GetLastError=" << GetLastError();
    }
}

} // namespace
#endif

void applyProcessMitigations() noexcept {
#ifdef _WIN32
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    if (!k32) {
        qWarning() << "ProcessMitigations: kernel32.dll handle null. "
                   << "Skipping all mitigations.";
        return;
    }

    auto setFn = reinterpret_cast<SetProcessMitigationPolicy_t>(
        GetProcAddress(k32, "SetProcessMitigationPolicy"));
    auto getFn = reinterpret_cast<GetProcessMitigationPolicy_t>(
        GetProcAddress(k32, "GetProcessMitigationPolicy"));

    if (!setFn) {
        qInfo() << "ProcessMitigations: SetProcessMitigationPolicy not "
                << "available (pre-Win8). Skipping.";
        return;
    }

    // ── DEP (DataExecutionPrevention) ─────────────────────────────────
    // x64 has DEP on by default but pinning makes it explicit + means
    // a future ROP-via-VirtualProtect attack can't disable it.
    {
        PROCESS_MITIGATION_DEP_POLICY p{};
        p.Enable = 1;
        p.DisableAtlThunkEmulation = 1;
        p.Permanent = 1;
        applyOne(setFn, ProcessDEPPolicy, p, "DEP");
    }

    // ── ASLR ─────────────────────────────────────────────────────────
    // Mandatory + BottomUp + HighEntropy. We don't disable export-
    // address-filtering or import-address-filtering (those require the
    // related EAF/IAF policies which can break legitimate Qt plugin
    // loading on some configurations).
    {
        PROCESS_MITIGATION_ASLR_POLICY p{};
        p.EnableForceRelocateImages = 1;
        p.EnableBottomUpRandomization = 1;
        p.EnableHighEntropy = 1;
        p.DisallowStrippedImages = 1;
        applyOne(setFn, ProcessASLRPolicy, p, "ASLR");
    }

    // ── Image load policy ────────────────────────────────────────────
    // Refuse DLLs from remote shares and from a low-IL location.
    // PreferSystem32 means an unqualified load picks the System32 copy
    // first, defeating the classic "drop a malicious comdlg32.dll next
    // to the .exe" hijack pattern.
    {
        PROCESS_MITIGATION_IMAGE_LOAD_POLICY p{};
        p.NoRemoteImages = 1;
        p.NoLowMandatoryLabelImages = 1;
        p.PreferSystem32Images = 1;
        applyOne(setFn, ProcessImageLoadPolicy, p, "ImageLoadPolicy");
    }

    // ── Strict handle check ──────────────────────────────────────────
    // Convert invalid-handle use into an immediate crash so QA / fuzz
    // runs surface the bug instead of silently corrupting state.
    {
        PROCESS_MITIGATION_STRICT_HANDLE_CHECK_POLICY p{};
        p.RaiseExceptionOnInvalidHandleReference = 1;
        p.HandleExceptionsPermanentlyEnabled = 1;
        applyOne(setFn, ProcessStrictHandleCheckPolicy, p,
                 "StrictHandleCheck");
    }

    // ── Query CFG status (does not enable; just records evidence) ────
    if (getFn) {
        PROCESS_MITIGATION_CONTROL_FLOW_GUARD_POLICY cfg{};
        queryOne(getFn, ProcessControlFlowGuardPolicy, cfg, "CFG");
        qInfo() << "ProcessMitigations: CFG"
                << "EnableControlFlowGuard=" << cfg.EnableControlFlowGuard
                << "EnableExportSuppression=" << cfg.EnableExportSuppression
                << "StrictMode=" << cfg.StrictMode;
    }

    // ── INTENTIONALLY NOT APPLIED ────────────────────────────────────
    //
    // ProcessSignaturePolicy (MicrosoftSignedOnly) — would block
    //   PDFE's own non-Microsoft-signed plugin DLLs from loading.
    //
    // ProcessDynamicCodePolicy (ProhibitDynamicCode) — Qt6 QML/JS uses
    //   dynamic code generation; MuJS in MuPDF does too when PDF form
    //   JavaScript is enabled. Setting this would make legitimate
    //   features stop working.
    //
    // ProcessExtensionPointDisablePolicy — would block AppInit DLLs
    //   and SetWindowsHookEx-based extensions, which is mostly safe,
    //   but breaks accessibility tools (NVDA, JAWS, etc.) in some
    //   configurations. The accessibility win > the security win for
    //   PDFE's threat model.
    //
    // ProcessFontDisablePolicy — would block GDI font loading, which
    //   would break Qt's font fallback for PDF rendering.
#endif // _WIN32
}

} // namespace pdfe::app
