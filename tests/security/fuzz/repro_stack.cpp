// -*- coding: utf-8 -*-
// Diagnostic repro (NOT a fuzz target): open a PDF on a thread with a small
// stack so a runaway recursion overflows SHALLOW, where ASan can still walk
// and print the repeating frames — pinpointing the unguarded recursion that
// fuzz_document_open found. Usage: repro_stack <file.pdf>
#include <windows.h>
#include <cstdio>
#include <memory>
#include <exception>

#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/PreflightOpen.h"
#include <cstring>

static const char* g_path;

static DWORD WINAPI worker(LPVOID)
{
    try {
        auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
        auto doc = pdfe::core::Document::open(ctx, g_path);
        std::printf("OPENED OK\n");
    } catch (const std::exception& e) {
        std::printf("REJECTED: %s\n", e.what());
    } catch (...) {
        std::printf("REJECTED: unknown\n");
    }
    return 0;
}

int main(int argc, char** argv)
{
    if (argc < 2) { std::printf("usage: repro_stack <file> [stack_MB|preflight]\n"); return 2; }
    g_path = argv[1];
    if (argc >= 3 && std::strcmp(argv[2], "preflight") == 0) {
        bool safe = pdfe::core::isDocumentOpenSafe(argv[1]);
        std::printf("PREFLIGHT: %s\n", safe ? "SAFE" : "UNSAFE");
        return 0;
    }
    SIZE_T stackBytes = (argc >= 3 ? (SIZE_T)atoll(argv[2]) : 1) * 1024 * 1024;
    std::printf("stack reserve: %llu MB\n",
                (unsigned long long)(stackBytes / (1024 * 1024)));
    HANDLE t = CreateThread(NULL, stackBytes, worker, NULL,
                            STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (!t) { std::printf("CreateThread failed\n"); return 3; }
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
    return 0;
}
