// -*- coding: utf-8 -*-
// Phase-8 fuzz harness (R2-15): redaction apply.
//
// Drives redaction::RedactionManager — create a mark then applyPageRedactions
// / applyAllRedactions over a fuzzed page. This is the redaction content-
// removal + QPDF single-revision rewrite surface, a security-critical
// destructive write path.
//
// The per-input body lives in fuzz_targets.h as pdfe::fuzz::run_redaction_apply
// so the GoogleTest smoke test can drive the identical logic without libFuzzer.
#include "fuzz_targets.h"
#include "fuzz_common.h"

extern "C" void pdfe_fuzz_one_input(const std::uint8_t* data, std::size_t size)
{
    pdfe::fuzz::run_redaction_apply(data, size);
}
