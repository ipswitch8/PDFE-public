// -*- coding: utf-8 -*-
// Phase-6/8 fuzz harness (R2-16): malformed /Encrypt dictionary.
//
// Targets the MuPDF standard security handler exercised when opening
// password-protected PDFs: pdf_read_encrypt -> pdf_new_crypt ->
// pdf_compute_encryption_key -> pdf_authenticate_password. Malformed
// /Encrypt material (oversize /Length, bogus /V or /R, truncated /O /U,
// mismatched /CF) has historically caused integer overflows and buffer
// over-reads in PDF crypto parsers.
//
// R2-16 FIX: the previous harness wrapped raw fuzz bytes as the ENTIRE
// /Encrypt dict body, which MuPDF rejects at the dict/trailer stage — the
// crypt parser was almost never reached. The core now builds a structurally
// valid encrypted PDF (valid /Filter /Standard dict + /ID + trailer /Encrypt)
// whose security-relevant fields are fuzz-driven, so MuPDF recognizes the
// document as encrypted and runs the real key-derivation + auth path against
// the malformed material. See pdfe::fuzz::build_encrypted_pdf.
//
// The per-input body lives in fuzz_targets.h as pdfe::fuzz::run_encryption_dict
// so the GoogleTest smoke test can drive the identical logic without libFuzzer.
#include "fuzz_targets.h"
#include "fuzz_common.h"

extern "C" void pdfe_fuzz_one_input(const std::uint8_t* data, std::size_t size)
{
    pdfe::fuzz::run_encryption_dict(data, size);
}
