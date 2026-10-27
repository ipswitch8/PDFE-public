// -*- coding: utf-8 -*-
// Phase-8 fuzz harness (R2-15): MuJS execution.
//
// Drives forms::JavaScriptEngine — enable() (which runs document-level
// /OpenAction + /Names JavaScript) and the full form-field event set
// (keystroke/validate/calculate/format/focus/blur + document events) with
// fuzzed field values. This is the embedded-JavaScript execution surface
// (MuPDF's bundled MuJS interpreter) reached from a fuzzed PDF.
//
// The per-input body lives in fuzz_targets.h as pdfe::fuzz::run_mujs_exec
// so the GoogleTest smoke test can drive the identical logic without libFuzzer.
#include "fuzz_targets.h"
#include "fuzz_common.h"

extern "C" void pdfe_fuzz_one_input(const std::uint8_t* data, std::size_t size)
{
    pdfe::fuzz::run_mujs_exec(data, size);
}
