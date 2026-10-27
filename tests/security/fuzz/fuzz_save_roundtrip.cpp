// -*- coding: utf-8 -*-
// Phase-8 fuzz harness (R2-15): save / incremental round-trip.
//
// Drives Document::writeRecoveryCopy + saveAs (full rewrite) + saveIncremental
// (append) then reopens the written bytes. This is the save-integrity surface
// (full-rewrite GC routing, incremental xref append, file-lock/round-trip) that
// held PDFE's real historical data-loss defects.
//
// The per-input body lives in fuzz_targets.h as pdfe::fuzz::run_save_roundtrip
// so the GoogleTest smoke test can drive the identical logic without libFuzzer.
#include "fuzz_targets.h"
#include "fuzz_common.h"

extern "C" void pdfe_fuzz_one_input(const std::uint8_t* data, std::size_t size)
{
    pdfe::fuzz::run_save_roundtrip(data, size);
}
