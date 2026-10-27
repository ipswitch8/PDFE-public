# Security & Compliance Feature Gaps

<!-- tracking-banner -->
> **STATUS: HISTORICAL EVIDENCE — this is not a work list.**
>
> **GitHub Issues (`ipswitch8/PDFE`) is the canonical tracker for anything still
> open.** This document records what an audit found on the day it ran and what
> was decided about it. Rows here may have been fixed, superseded, or
> deliberately declined since. Where a row and a GitHub issue disagree, **the
> issue wins**; where a row describes something still broken and no issue
> exists, file one.
>
> See [`docs/TRACKING.md`](../TRACKING.md) for why, and for the parts of this repo that are
> operationally live rather than historical.

---


**Discovered:** phase-8 (P1 Security and Compliance)
**Status:** open backlog — phase-8 proceeds with a narrowed criterion

## Tracked Gaps

### 1. Legacy encryption algorithms not offered

`src/ui/EncryptionDialog.cpp:27-28` populates the algorithm combo box
with only:

```cpp
algorithmCombo_->addItem("None");
algorithmCombo_->addItem("AES-256");
```

The original phase-0 matrix expected:

- RC4-40
- RC4-128
- AES-128
- AES-256

The underlying `EncryptionManager::saveEncrypted` API in
`src/encryption/EncryptionManager.h` supports `EncryptionAlgorithm`
as an enum; the UI just doesn't expose the legacy variants. 6 phase-8
matrix rows are blocked for this reason:

- `sec_encrypt_rc4_40_set` / `_reopen`
- `sec_encrypt_rc4_128_set` / `_reopen`
- `sec_encrypt_aes_128_set` / `_reopen`

**To unblock:** add `addItem("RC4-40")`, `addItem("RC4-128")`, and
`addItem("AES-128")` entries in EncryptionDialog and wire them through
`applyEncryptionSettings()`. Test rows will flip from blocked to
green automatically once the combo is populated (the test code already
supports any algorithm key via `ALGORITHMS` dict).

Alternatively, accept that AES-256 is the only modern algorithm worth
supporting and remove these matrix rows entirely as obsolete-by-spec.
AES-256 is what the PDF 2.0 standard mandates; RC4 is broken and
AES-128 is a transitional format.

### 2. PDF/A validation fixtures missing

No PDF/A fixtures exist in `tests/fixtures/`:

- `test_pdfa_1b.pdf`
- `test_pdfa_2b.pdf`
- `test_pdfa_2u.pdf`
- `test_pdfa_2a.pdf`
- `test_pdfa_3b.pdf`

5 phase-8 matrix rows (`sec_pdfa_*_validate`) are blocked on this.

**To unblock:** run the pdfe `compliance::PdfAConverter` on a simple
plain PDF for each level and save the results as fixtures. The convert
path is exercised in tests, so if Tier-1 generated outputs land we can
re-point the validate tests at them.

### 3. PDF/UA coverage not in UI

PDF/UA-1 is a tagged-PDF accessibility profile. `ComplianceDialog`
exposes it as a LevelCombo item but the conversion and validation
pipeline through the UI is not wired up beyond Analyze. 2 rows
blocked: `sec_pdfua_validate` and `sec_pdfua_convert`.

**To unblock:** either (a) implement the full PDF/UA pipeline in
ComplianceDialog's Convert button branch, or (b) create a pre-tagged
PDF fixture that pdfe can validate against.

### 4. Signature verify flows

`sec_sign_verify_valid`, `sec_sign_verify_invalid`,
`sec_sign_chain_validation`, `sec_sign_timestamp_present` need:

- A pre-signed fixture for verify_valid (phase-8 sign_visible now
  produces signed output at runtime, so this is sourceable from there
  once we persist a signed copy to the corpus)
- A tampered signed fixture for verify_invalid
- A CA-signed certificate chain for chain_validation (self-signed
  test_signer.pfx doesn't exercise chain walking)
- TSA (timestamp authority) integration in pdfe's SignDialog for
  timestamp_present

All 4 blocked pending corpus/feature work.

### 5. Permissions menu-state queries

`sec_encrypt_permissions_print_blocked` and `_copy_blocked` require a
multi-stage flow:

1. Encrypt with a specific permission flag disabled
2. Save, close, reopen, enter password
3. Query the state of `printAction_` / `copyAction_` to verify the
   menu action reflects the permission

The test harness has no helper for multi-relaunch sequences yet (the
fixture tears down between cases). This is a phase-10 polish item.

### 6. Password strength meter widget does not exist

The matrix row `sec_security_password_strength_meter` expected a
real-time meter in EncryptionDialog showing weak/medium/strong
as the user types. No such widget exists — `src/ui/EncryptionDialog.cpp`
shows no `setObjectName("passwordStrengthMeter")` or equivalent. The
dialog has a `validationLabel_` for error messages but nothing
strength-related.

**To unblock:** either add the widget to EncryptionDialog, or remove
this matrix row as phase-0 wishful thinking.
