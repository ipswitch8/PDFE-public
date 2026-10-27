// -*- coding: utf-8 -*-
// Phase-8 fuzz harness (R2-15): content-stream text edit.
//
// Drives ContentStreamEditor::applyEdits + the /Contents snapshot-restore
// undo path over a fuzzed page. This is the mirror / state-continuity bug
// surface (op_Tz / canonical Tm emission + pdf_filter_page_contents rewrite)
// that held PDFE's real historical text-edit defects. Seeded from the
// adversarial-ambient corpus (tests/fixtures/ambient_*.pdf).
//
// The per-input body lives in fuzz_targets.h as pdfe::fuzz::run_content_stream_edit
// so the GoogleTest smoke test can drive the identical logic without libFuzzer.
#include "fuzz_targets.h"
#include "fuzz_common.h"

extern "C" void pdfe_fuzz_one_input(const std::uint8_t* data, std::size_t size)
{
    pdfe::fuzz::run_content_stream_edit(data, size);
}
