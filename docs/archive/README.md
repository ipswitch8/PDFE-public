# Archived docs

<!-- tracking-banner -->
> **STATUS: HISTORICAL EVIDENCE — this is not a work list.**
>
> **GitHub Issues (`ipswitch8/PDFE`) is the canonical tracker for anything still
> open.** This document records what was found or decided on the day it was
> written. Items here may have been fixed, superseded, or deliberately declined
> since. Where this document and a GitHub issue disagree, **the issue wins**.
>
> See [`docs/TRACKING.md`](../TRACKING.md).

---


Documents here are **historical**. They captured gaps, bugs, or
architecture snapshots that were current at the time of writing and
have since been addressed or superseded.

Each file keeps its original body. If you're looking for the
*current* status of a feature, start from
[`../acrobat-pro-parity-status.md`](../acrobat-pro-parity-status.md).

## Index

| File | Archived when | Superseded by |
|---|---|---|
| `security-feature-gaps.md` | Phase 13 (2026-04-22) | `acrobat-pro-parity-status.md` — **PARTIALLY** resolved by Phases 8a/8b/8c core + the fixture corpus. See the correction note below; do not read this row as a blanket "resolved". |
| `text-formatting-feature-gaps.md` | Phase 13 (2026-04-22) | `acrobat-pro-parity-status.md` + `text-reflow-engine-architecture.md` — alignment persistence is addressed in Phase 9 (engine) and will be wired into ContentStreamEditor in the next atomic phase. |
| `undo-panel-refresh-bug.md` | Phase 13 (2026-04-22) | Fixed in the annotation-commands phases; the symptoms listed no longer reproduce against the current UndoManager + CommentPanel wiring. |

Kept as-is rather than deleted so the history is visible when
grep-ing the repo for why a decision was made.

## Correction — `security-feature-gaps.md` was not fully resolved

*Meta-audit follow-up F19, gap A5-79, re-verified 2026-08-03.*

The index row above named **four** gaps and called them resolved.
`security-feature-gaps.md` actually tracks **six**, and the blanket claim
over-reached on four counts:

| # | gap in `security-feature-gaps.md` | real state |
|---|---|---|
| 1 | Legacy encryption algorithms not offered | **Half.** `src/ui/EncryptionDialog.cpp:30-34` offers None / RC4-40 / RC4-128 / AES-128 / AES-256 and the `_set` rows are green, but the three `sec_encrypt_*_reopen` rows are **`wont-fix`** (password dialog not enumerable via Desktop UIA on Server-2019), so the round-trip is unverified. |
| 2 | PDF/A validation fixtures missing | **Resolved.** `test_pdfa_{1b,2b,2u,2a,3b}.pdf` + sidecars present; 5 rows green. |
| 3 | PDF/UA coverage not in UI | **NOT resolved.** Rows are green, but `sec_pdfua_validate`/`_convert` carry notes admitting no tagged fixture exists, and `src/compliance/PdfUAConverter.cpp:113` marks the conversion path out of scope. Green over an acknowledged out-of-scope. |
| 4 | Signature verify flows | **Three of four.** `sec_sign_chain_validation` is **`wont-fix`** on the rationale that no CA-signed cert exists — a rationale the fixture corpus has since invalidated (`tests/fixtures/certs/test_root_ca.pem`, `test_intermediate_ca.pem`, `test_signer_intermediate.pfx` are all present). Worth reopening. |
| 5 | Permissions menu-state queries | **Resolved** — and never named in the index row. |
| 6 | Password strength meter | **Resolved** (`passwordStrengthBar_`, `EncryptionDialog.cpp:82-92`) — also never named. |

So the row was simultaneously **over**-inclusive (claimed resolution for 3 and
for part of 1 and 4) and **under**-inclusive (silently omitted 5 and 6). An
archive index that says "resolved" is a claim like any other and gets checked
like one.
