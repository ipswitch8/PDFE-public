// -*- coding: utf-8 -*-
// Out-of-process document pre-flight opener.
//
// The main PDFE application spawns this tiny helper as a CHILD PROCESS to
// validate that opening a given PDF will not crash. A hostile document can
// drive MuPDF's parser/repair into unbounded (effectively cyclic) recursion
// that exhausts the stack — an unrecoverable crash that cannot be safely
// trapped in-process (fz_try/fz_catch is setjmp-based and a stack overflow
// corrupts both the stack and the allocator state). By attempting the open
// HERE, in a disposable process, such a crash terminates only this helper;
// the parent observes the abnormal exit and refuses the document, staying
// healthy. This is the same isolation strategy browsers use for untrusted
// content.
//
// Exit codes (normal termination — the document is SAFE to open in-process):
//   0  opened and the page tree walked cleanly
//   1  MuPDF rejected it with an ordinary error (malformed but not a crash)
//   2  usage / context-creation error
// Abnormal termination (access violation 0xC0000005, stack overflow
// 0xC00000FD, etc.) or a timeout enforced by the parent => UNSAFE.
//
// Deliberately links only MuPDF (no Qt) so it starts fast enough to gate
// every File>Open without a perceptible delay.

#include <mupdf/fitz.h>

#include <cstdio>
#include <cwchar>
#include <string>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

#if defined(_WIN32)
namespace {

// The one thing this helper must get right before it can judge anything:
// the PATH IT WAS HANDED.
//
// fz_open_document() takes a UTF-8 filename. On Windows the CRT hands a
// plain `main(int, char**)` its argv already converted to the process ANSI
// code page, and any character the ACP cannot represent is replaced with
// '?'. The parent passes the path as UTF-16 (CreateProcessW, see
// src/core/PreflightOpen.cpp), so the corruption happened entirely on this
// side of the boundary. The result was a FALSE ACCUSATION: a perfectly good
// PDF whose path contained any non-ACP character (a CJK folder, a Cyrillic
// name, an emoji) was opened by MuPDF under a corrupted filename, failed,
// exited 1, and the parent — which reads exit-code-0-means-safe — refused
// it as "malformed or crafted to exhaust resources".
//
// wmain() receives the command line as WIDE characters straight from the
// CRT, with no code-page conversion anywhere in the path, and we convert to
// UTF-8 ourselves. The ACP is out of the picture entirely.
//
// The exit-code contract with the parent (0 = clean open, anything else or
// an abnormal exit = refuse) is unchanged; only the fidelity of the
// filename is. A conversion failure returns the usage code 2, which the
// parent already treats as "not safe" — this fails closed like everything
// else here.
std::string toUtf8(const wchar_t* w)
{
    if (!w || w[0] == L'\0')
        return {};
    const int need =
        ::WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (need <= 1)
        return {};
    std::string utf8(static_cast<size_t>(need), '\0');
    if (::WideCharToMultiByte(CP_UTF8, 0, w, -1, utf8.data(), need,
                              nullptr, nullptr) <= 0)
        return {};
    utf8.resize(static_cast<size_t>(need) - 1); // drop the terminating NUL
    return utf8;
}

} // namespace

#if defined(_WIN32)
namespace {

// ── SEC-60: ACG + CIG, on this helper only ─────────────────────────────────
//
// src/app/ProcessMitigations.cpp explains, in its "INTENTIONALLY NOT APPLIED"
// section, why the main application cannot take these two: ProhibitDynamicCode
// (ACG) breaks Qt's QML/JS and MuJS, and MicrosoftSignedOnly (CIG) would block
// PDFE's own non-Microsoft-signed binaries from loading.
//
// NEITHER OBJECTION APPLIES HERE. This helper links MuPDF and nothing else --
// no Qt, no MuJS, no plugins -- and it is the process that deliberately
// touches the most hostile input in the product. It is also the process whose
// death is already an expected, handled outcome: the parent reads an abnormal
// exit as "unsafe" and refuses the document. So it is the one place where the
// strongest available mitigations cost nothing and protect the most.
//
// ACG means an attacker who achieves a write primitive in the MuPDF parser
// cannot then allocate executable memory or make existing pages executable --
// it removes the standard route from memory corruption to code execution.
// CIG means the same attacker cannot load an unsigned DLL into this process.
//
// Deliberately NOT linking ProcessMitigations.cpp: that file pulls in QDebug,
// and the whole point of this helper is that it starts fast enough to gate
// every File > Open. Fifty duplicated lines are the right trade against a Qt
// dependency here; the duplication is bounded and this comment is why.
//
// Failures are silent and non-fatal by design. An older Windows without these
// policies must still preflight documents -- refusing to run would turn a
// missing hardening into a total loss of the crash-isolation gate, which is
// the stronger control of the two. tests/test_process_mitigations.cpp asserts
// they ARE on where they can be, so "silent" does not mean "unverified".
void applyPreflightHardening() noexcept
{
    using SetPolicy_t = BOOL (WINAPI*)(PROCESS_MITIGATION_POLICY, PVOID, SIZE_T);
    HMODULE k32 = ::GetModuleHandleW(L"kernel32.dll");
    if (!k32) return;
    auto setFn = reinterpret_cast<SetPolicy_t>(
        reinterpret_cast<void*>(
            ::GetProcAddress(k32, "SetProcessMitigationPolicy")));
    if (!setFn) return;

    // ACG. ProhibitDynamicCode without AllowThreadOptOut / AllowRemoteDowngrade:
    // an opt-out a thread can take is an opt-out an exploit can take.
    {
        PROCESS_MITIGATION_DYNAMIC_CODE_POLICY p{};
        p.ProhibitDynamicCode = 1;
        setFn(ProcessDynamicCodePolicy, &p, sizeof(p));
    }

    // CIG.
    {
        PROCESS_MITIGATION_BINARY_SIGNATURE_POLICY p{};
        p.MicrosoftSignedOnly = 1;
        setFn(ProcessSignaturePolicy, &p, sizeof(p));
    }
}

// `--report-mitigations`: query the two policies and print them, one per line.
//
// The read-back HAS to happen in this process. A mitigation applied here is
// invisible from pdfe_tests.exe, which cannot set ACG on itself (it is a Qt
// program) and cannot query another process's dynamic-code policy without a
// handle and the right access. So the child reports on itself and the test
// spawns it -- following the queryOne pattern in ProcessMitigations.cpp, one
// process further out.
int reportMitigations()
{
    using GetPolicy_t = BOOL (WINAPI*)(HANDLE, PROCESS_MITIGATION_POLICY,
                                       PVOID, SIZE_T);
    HMODULE k32 = ::GetModuleHandleW(L"kernel32.dll");
    auto getFn = k32 ? reinterpret_cast<GetPolicy_t>(
                           reinterpret_cast<void*>(
                               ::GetProcAddress(k32, "GetProcessMitigationPolicy")))
                     : nullptr;
    if (!getFn) {
        std::printf("GetProcessMitigationPolicy=unavailable\n");
        return 2;
    }

    PROCESS_MITIGATION_DYNAMIC_CODE_POLICY dyn{};
    const BOOL dynOk = getFn(::GetCurrentProcess(), ProcessDynamicCodePolicy,
                             &dyn, sizeof(dyn));
    std::printf("ProhibitDynamicCode=%u\n",
                dynOk ? dyn.ProhibitDynamicCode : 0u);
    std::printf("AllowThreadOptOut=%u\n",
                dynOk ? dyn.AllowThreadOptOut : 0u);

    PROCESS_MITIGATION_BINARY_SIGNATURE_POLICY sig{};
    const BOOL sigOk = getFn(::GetCurrentProcess(), ProcessSignaturePolicy,
                             &sig, sizeof(sig));
    std::printf("MicrosoftSignedOnly=%u\n",
                sigOk ? sig.MicrosoftSignedOnly : 0u);

    return 0;
}

}  // namespace
#endif  // _WIN32

// MSVC's linker selects wmainCRTStartup automatically when wmain is defined,
// so no explicit /ENTRY and no extra library are needed.
int wmain(int argc, wchar_t** argv)
#else
int main(int argc, char** argv)
#endif
{
#if defined(_WIN32)
    // SEC-60: applied FIRST, before any argument is looked at and long before
    // MuPDF sees a byte of the document. A mitigation applied after the parse
    // has begun protects nothing that matters.
    applyPreflightHardening();

    if (argc < 2)
        return 2;
    if (std::wcscmp(argv[1], L"--report-mitigations") == 0)
        return reportMitigations();
    const std::string targetUtf8 = toUtf8(argv[1]);
    if (targetUtf8.empty())
        return 2;
    const char* target = targetUtf8.c_str();
#else
    // POSIX argv is already a byte string in the locale/filesystem encoding,
    // which is what fz_open_document() and open(2) both want — no conversion.
    if (argc < 2)
        return 2;
    const char* target = argv[1];
#endif

    fz_context* ctx = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
    if (!ctx)
        return 2;

    int rc = 1;
    fz_try(ctx)
    {
        fz_register_document_handlers(ctx);
        fz_document* doc = fz_open_document(ctx, target);
        if (doc)
        {
            // Exercise the page-tree walk / page count — this is the path a
            // deeply or cyclically nested page graph drives into overflow.
            (void) fz_count_pages(ctx, doc);
            fz_drop_document(ctx, doc);
            rc = 0;
        }
    }
    fz_catch(ctx)
    {
        rc = 1; // ordinary MuPDF error: malformed, but it did NOT crash us
    }

    fz_drop_context(ctx);
    return rc;
}
