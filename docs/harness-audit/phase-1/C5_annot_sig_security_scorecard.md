# C5 — Annotations / Signatures / Security-Crypto — Adversarial Oracle Scorecard

**Cluster:** `C5_annot_sig_security` · **62 files · 657 tests** · harness treated as defendant.
Every file was read in full (5 parallel batches). Each file classified by dominant oracle; 7 smell patterns counted with `file:line`.

## Headline

Positive content-forgery **is** caught — real `tamper -> INVALID` metamorphic oracles exist
(`test_signature_roundtrip.cpp:146/204`, `test_signature_security_phase2.cpp:235`). The security **rot is in the trust / policy / attack-neutralization layers**:

1. **21 named-CVE replays** whose sole oracle is a wall-clock timing bound around a catch-all `try/catch`.
2. **Certificate trust-chain never positively validated** — every fixture is self-signed, so the cert-trust assertions are tautological ORs that any self-signed cert satisfies.
3. **DocMDP enforcement only grep-checked** for byte presence; no "disallowed edit is flagged" test.
4. **OCSP/CRL freshness "rejection" tests masked** by a permanently-`UntrustedChain` fixture (two tests' own comments concede it) — the FIX-OCSP-VALIDATOR guards don't guard the fix.
5. **Hostile-JS / SSRF / redaction tests** assert neutralization in their **names** but only prove no-crash.

## Counts by oracle

| Oracle | Files | Tests | Reading |
|---|---:|---:|---|
| true | 21 | 249 | Asserts the actual result |
| metamorphic | 5 | 44 | Relation (sign→valid / tamper→invalid; scale ~2x) |
| heuristic | 3 | 19 | Plausible property / token-blocklist |
| proxy | 30 | 333 | Correlated-not-equal (no-throw, exists, reopens, flag echo) |
| no-oracle-smoke | 3 | 12 | Asserts nothing meaningful |
| pseudo | 0 | 0 | — |

**~53% of tests (345/657) ride proxy or no-oracle-smoke oracles.**

## Counts by smell (occurrences)

| Smell | Count |
|---|---:|
| widget-exists-only | 40 |
| tautological | 39 |
| EXPECT_NO_THROW-only | 27 |
| over-tolerant-threshold | 21 |
| extraction-only-for-visual | 16 |
| reopens-without-error-only | 12 |
| assertion-free | 11 |

## Top weakest oracles (security theatre called out explicitly)

| # | File:line | Test | Oracle | Failure that ships GREEN | Sev |
|---|---|---|---|---|---|
| 1 | `test_cve_replay.cpp:62` | verifySafeOpen (all 21 CVE replays) | proxy | **SECURITY THEATRE.** Re-introducing NTLM leak (CVE-2018-4993), embedded /AA JS exec (CVE-2021-30560) or font-PostScript RCE (CVE-2024-4367) passes as long as open returns in 5–10s and doesn't throw. Catch-all `try/catch` (57-59) even hides a throw. No test asserts the attack was neutralized. | **Critical** |
| 2 | `test_malicious_pdf_advanced.cpp:651` | ProcessSpawnAttemptBlocked | proxy | **SECURITY THEATRE.** If MuJS ever got a launch transport, `app.launchURL(...calc.exe)` would spawn a process and this still passes — oracle is only "fireKeystroke didn't throw + pageCount survives". Name promises a block it never verifies. | **Critical** |
| 3 | `test_prohibited_feature_scanner.cpp:39` | No{Encryption,JavaScript,ExternalRefs}_FreshDoc + ScanAll | heuristic | **Negative-only.** Entire suite asserts only that a clean doc reports nothing. A scanner hard-wired to detect nothing (always false/empty) passes all 9 → PDF/A compliance silently passes non-compliant docs. | High |
| 4 | `test_signature_roundtrip.cpp:425` | SelfSigned/Expired/IntermediateCA cert tests | proxy | **Trust-chain never positively validated.** All fixtures self-signed + no trust store → certStatus always non-Okay; asserts "non-Okay OR !certValid OR !isFullyValid" which a self-signed cert satisfies unconditionally. A signature from an **attacker-controlled issuer is indistinguishable from a trusted one.** Plus SUCCEED() escapes + GTEST_SKIPs. | High |
| 5 | `test_certification_signatures.cpp:175` | P1/P2/P3 DocMDP + Preserves tests | proxy | **DocMDP enforcement never negatively tested.** /DocMDP + /P N only grep-checked; only ALLOWED changes are shown to keep the digest valid. No test that a **disallowed** edit under P1 NoChanges / P2 FormFillOnly is flagged. Enforcement could silently permit forbidden edits, all tests green. | High |

### Further High findings (security)

- `test_ocsp_crl_validation.cpp:279` + `test_encrypt_ocsp_phase3.cpp:349` + `test_ltv_responder_validation.cpp:179` — every staleness/expiry/future/unknown "rejection" is `EXPECT_NE(LtvValidated)` on a permanently-untrusted chain; **freshness enforcement could be deleted and the FIX-OCSP-VALIDATOR guards stay green** (own comments concede "both end up as UntrustedChain").
- `test_signature_security_phase2.cpp:310` — TamperedPkcs7NotFullyValid has an **operator-precedence defect** (`&b == 0` is never null → always writes 0x00); if the target byte is already 0x00 the "tamper" is a no-op and it asserts `!isFullyValid` on an unmodified signed doc. **A tamper test that may not tamper.**
- `test_tsa_real_endpoint.cpp:146` — timestamp-token OID is **printed, never asserted** ("bonus best-effort"); reachability gate only checks a local file exists. TSA timestamping could be wholly non-functional and this "real endpoint" test passes.
- `test_ltv_validator.cpp:342` — **forged-GOOD-responder gap:** verifyWith "doesn't gate on OCSP signer trust, only the certStatus value" (code comment). An attacker injecting a self-signed GOOD OCSP into /DSS is reported as validly revocation-checked. No such negative exists.
- `test_malicious_pdf_security.cpp:179` — RedactionClearsUndoEvidence greps for `/UndoStack`, **never for the redacted secret** → a redaction leaving recoverable glyphs passes.
- `test_malicious_pdf_security.cpp:211` — GoToR/DataUri/JsSubmitForm all NO_THROW-only; names promise "Rejected/NotExecuted/NotAvailable".
- `test_encryption_roundtrip.cpp:410` — **no test anywhere proves a permission-restricted op is actually blocked.** Every "permission" assertion is a `getPermissions()` bitmask read-back; one test is literally named `...Enforced` while only reading the flag.
- `test_watermark_extended.cpp:186` — ImageWatermarkRespectsPageRange asserts **nothing about the range** (only `open!=null` + `pageCount==3`); a logo could be stamped on pages meant to be excluded.
- `test_annotation_tools.cpp:129` — HighlightFromSelection's only assertion is inside `if(annotationWasCreated)`; with empty selection nothing is created → a totally broken highlight tool passes with **zero assertions**.
- `test_annotations_phase4.cpp:346` — FreeText DA persistence asserts `daFontName` non-empty + `daFontSize>0`; set Helv/14.0/red, **daColor never checked** — wrong font/size/lost-color ships green.

### Notable Medium findings

- `test_pkcs7_fuzz.cpp:140` — 3 assertion-free bodies; "DoesNotLeak" measures no allocation.
- `test_pkcs7_bridge.cpp:115` — verifier constructed + vtable-non-null, `check_digest`/`check_certificate` **never invoked** on content.
- `test_invisible_and_certification_signatures.cpp:85` — all 8 are no-throw + grep-for-marker; **not one `verifySignature` call** (invisible sigs are a prime forgery surface).
- `test_security_phase8.cpp:145` — "locks signature byte-range" but only checks a non-empty file was written.
- `test_annotation_roundtrip.cpp:69` — **/Rect geometry never asserted** after any roundtrip; a note could reopen in the wrong place, all 12 green.
- `test_tsa_fixture.cpp:76` — DocTimeStamp token "valid ASN.1" = first byte `0x30` only; never parsed/verified/imprint-matched.
- `test_tsa_and_taborder.cpp:82` — TSA response-parse tests round-trip raw OpenSSL i2d/d2i **without ever calling PDFE's TsaClient** (validates OpenSSL, not PDFE).
- `test_stamp_and_watermark.cpp:105` — stamp persistence = bare annotation count; wrong rect / lost contents / blank AP would pass.
- `test_comment_panel_context_menu.cpp:52` — Reply/Delete asserted by objectName/label existence only; a dead menu ships green.

## Strongest files (earn their keep)

| File | Why it's a real oracle |
|---|---|
| `test_path_security.cpp` | Lexical UNC/file/scheme classifier, concrete TRUE/FALSE per input, mutation-killable. |
| `test_permission_enforcement.cpp` | **Real enforcement:** password-strip THROWS + no file; exporter returns false + no file; clipboard sentinel unchanged on copy-deny; print refuses before render. Positive controls + toggle-cleared negatives. |
| `test_image_stamp.cpp` | **Pixel oracle** — renders page, counts red-dominant pixels, re-renders after reopen. |
| `test_watermark_state_leak.cpp` / `test_hf_watermark_adversarial.cpp` | Device-space geometry via `fitz` stext quads against adversarial ambient state; neighbor glyph argb/x/y/advance fenced. |
| `test_fidelity_r3_20_annotation_ap.cpp` | Reopens with a fresh raw MuPDF ctx, asserts each annot carries a real /AP/N XObject with non-degenerate /BBox overlapping /Rect — kills "round-trips as data, paints as nothing". |
| `test_ltv_validator.cpp` | `EXPECT_EQ(Revoked)` + trust-anchor-gated `LtvValidated` with empty-store control. |
| `test_sign_dialog.cpp` | Asserts digestValid + Okay + !modifiedAfterSigning; wrong-password cannot yield a verifying /Sig. |
| `test_encryption_manager.cpp` / `test_encryption_roundtrip.cpp` | Real wrong-password negatives + AES-256 version/rev/method oracle (permission axis is proxy though). |
| `test_measure.cpp` | 3-4-5 distance, shoelace area, winding invariance, area-scale-SQUARED. |
| `test_attachments.cpp` / `test_file_attachment_annotation.cpp` | Byte-exact extract roundtrip incl. NUL; **path-traversal actually asserted** (`../` stripped, extract confined via weakly_canonical prefix). |
| `test_comment_threading.cpp` / `test_comment_panel_threading.cpp` | /IRT chain re-resolved to parent index on reopen; reply nesting depth asserted on the real tree. |
| `test_annotation_commands.cpp` | Undo/redo reverts AND re-applies contents/color through the manager. |

## Notes / mis-filings

- `test_content_stream_tm_sign.cpp` is **misfiled for a crypto audit** — "Tm sign" means the numeric sign of the text-matrix `Tm.d` (upside-down-text bug), not digital signatures. Strong algebraic oracles, zero security relevance.
- `test_encryption_settings.cpp` and the AlgorithmMapping tests in `test_encryption_dialog.cpp` are tautological struct/echo tests — high assert density, ~zero power to catch a crypto defect (risk = mis-attributed coverage).
- List-file banners are occasionally stale (e.g. `test_encryption_manager.cpp` header says "skip=2" but the file has zero `GTEST_SKIP`).
