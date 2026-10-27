# Signature profile support — what PDFE actually emits

**Status:** authoritative. This is the **single place** in the repository where
PDFE's digital-signature *profile* support is stated. Every other document that
describes PDFE's capabilities points here and does not restate a profile level.

**Last re-derived from the tree:** 2026-08-03 (meta-audit follow-up **F20**,
gap ids `PDF-34`, `PDF-35` — see
`docs/audit/meta-audit-2026-07/followup/phase-20-dispositions.md`).

---

## The short answer

> **PDFE does not implement PAdES.** It produces ISO 32000-1 §12.8
> `adbe.pkcs7.detached` signatures. It has never emitted
> `ETSI.CAdES.detached`, and the PKCS#7 container it builds does not satisfy
> CAdES-BES. **No PAdES baseline level — B-B, B-T, B-LT or B-LTA — is
> supported, and none is claimed.**

This corrects a claim that stood in five source comments, the public `README`
and three capability documents. The claim was never true: `grep -rn 'SubFilter'`
over `src/` returns zero, so MuPDF's default `adbe.pkcs7.detached` has always
been what landed in the file.

## What *is* implemented

| capability | status | evidence |
|---|---|---|
| ISO 32000-1 approval signatures (PKCS#7 detached, `adbe.pkcs7.detached`) | **implemented** | `src/signatures/SignatureManager.cpp` `signField`; `tests/test_signature_roundtrip.cpp` |
| Certification signatures (`/Perms /DocMDP`, levels P1/P2/P3) | **implemented** (write) + enforced on read | `src/signatures/DocMdpEnforcer.cpp`; `tests/test_docmdp_enforcement.cpp` |
| RFC-3161 signature timestamp, embedded as a PKCS#7 **unauthenticated** attribute (`id-aa-timeStampToken`) | **implemented** | `src/signatures/Pkcs7OpenSslBridge.cpp` `embedTimestampToken`; `tests/test_tsa_timestamp_verification.cpp` |
| Long-term-validation **data** — `/DSS` with `/Certs`, `/OCSPs`, `/CRLs`, `/VRI` | **implemented** | `src/signatures/LtvDssWriter.cpp`; `tests/test_ltv_dss_writer.cpp` |
| Document-level `/DocTimeStamp` (`/SubFilter /ETSI.RFC3161`) | **not implemented** | `grep -rn 'DocTimeStamp' src/` → 0 |

Embedding a `/DSS` dictionary is a perfectly real ISO 32000-2 §12.8.4.3 feature
and it is what PDFE does. **It is not PAdES-B-LT.** B-LT is defined as *a
CAdES-conformant signature* plus validation data; PDFE supplies the second half
over a signature that fails the first half, so the composite does not reach the
level. Calling it "PAdES-B-LT" overstated the product.

## Why the container is not CAdES

`src/signatures/Pkcs7OpenSslBridge.cpp` builds the container with

```c
int flags = PKCS7_DETACHED | PKCS7_BINARY | PKCS7_NOSMIMECAP;
PKCS7* p7 = PKCS7_sign(cert, pkey, chain, bio_in, flags);
```

That produces signed attributes `content-type`, `message-digest` and
`signing-time`. Measured against ETSI EN 319 122-1 (CAdES) and EN 319 142-1
(PAdES) there are **two** conformance failures, in opposite directions:

1. **Missing, and mandatory:** the ESS `signing-certificate-v2` attribute
   (RFC 5035, OID `1.2.840.113549.1.9.16.2.47`). CAdES-BES requires the signer
   to bind its own certificate into the signed attributes. PDFE emits neither
   `signing-certificate-v2` nor its v1 predecessor.
2. **Present, and prohibited:** the PKCS#9 `signing-time` attribute. PAdES
   requires the claimed signing time to be carried by the signature
   dictionary's `/M` entry and forbids the CMS attribute.

Both facts are asserted directly against the bytes of a freshly signed file by
`tests/test_pades_profiles.cpp` — they are measurements, not assertions of
belief, and they turn red the moment either changes.

## Why this was disclaimed rather than implemented

Recorded here because "not supported" is a decision, not an omission:

* Flipping `/SubFilter` to `ETSI.CAdES.detached` over the container described
  above would produce a document that **announces** CAdES conformance while
  failing two mandatory CAdES rules. That is a strictly worse false claim than
  the one being removed, and it would be believed by relying parties rather
  than by readers of a comment.
* A conformance claim needs a conformance *oracle*. There is no PAdES/ETSI
  validator in this tree or in its dependency set, so a self-asserted "we are
  B-B now" could not be substantiated by anything — the exact defect class this
  audit programme exists to remove.
* B-LTA is not reachable at all without a document-timestamp writer, which does
  not exist (see scope below).

## Scope a future PAdES implementation would need

Recorded so the gap has a size rather than a shrug.

**B-B** — build the SignedData with `PKCS7_sign(..., PKCS7_PARTIAL)` (or the
CMS API), add ESS `signing-certificate-v2` computed over the signer cert,
suppress `signing-time`, and override `/SubFilter` on the signature value
dictionary to `ETSI.CAdES.detached` before `pdf_save_document` plans the
byte-range layout. Note the hazard already documented at
`src/signatures/SignatureManager.cpp` — changing `/V` after
`pdf_sign_signature` has planned the layout has previously produced negative
byte-range lengths.

**B-T** — the RFC-3161 token PDFE already embeds *is* the CAdES
`signature-time-stamp` unauthenticated attribute, so B-T is mostly reachable
once B-B is, but it needs an **offline TSA seam**: today `TsaClient` talks to a
network endpoint, and a profile claim cannot rest on a test that needs the
internet.

**B-LT** — `LtvDssWriter` already writes the required `/DSS` material; B-LT
then follows from B-T plus a rule that the DSS covers every certificate in the
validation path.

**B-LTA** — needs a document-timestamp writer that does not exist: an
incremental update appending a signature dictionary with `/Type /DocTimeStamp`,
`/Filter /Adobe.PPKLite`, `/SubFilter /ETSI.RFC3161`, a `/ByteRange` covering
the whole file except the `/Contents` hole, and an RFC-3161 token as
`/Contents`. MuPDF exposes no API for this; the practical route is a
`pdf_pkcs7_signer` whose `create_digest` returns a bare TimeStampToken instead
of a SignedData, with `/Type` and `/SubFilter` patched on the field, plus the
same offline-TSA seam B-T needs. `tests/fixtures/generate_timestamped_pdf.py`
already mints such a token offline and is the obvious starting point for the
seam.

---

## Machine-readable declarations

The block below is **parsed by `tests/test_pades_profiles.cpp`**, which
re-derives each fact from a freshly signed PDF and from the source tree and
fails if this document and the product disagree — **in either direction**. Do
not edit these lines to match a wish; they are checked against behaviour.

<!-- BEGIN MACHINE-READABLE (parsed by tests/test_pades_profiles.cpp) -->
    emitted-subfilter: adbe.pkcs7.detached
    cades-signing-certificate-v2-attribute: absent
    cms-signing-time-attribute: present
    pades-b-b: not-supported
    pades-b-t: not-supported
    pades-b-lt: not-supported
    pades-b-lta: not-supported
    document-timestamp: not-implemented
<!-- END MACHINE-READABLE -->

### Capability-document set

These are the documents that state what PDFE can do. The guard
`PadesDocClaims.CapabilityDocsMakeNoProfileLevelClaim` reads this list and
fails if any of them names a PAdES baseline level (`PAdES-B-…` / `PAdES B-…`).
Only *this* document may name the levels, because only this document says they
are unsupported. The list is read from here, so shrinking it to dodge the guard
changes this file and is visible in review.

<!-- BEGIN CAPABILITY-DOCS (parsed by tests/test_pades_profiles.cpp) -->
    capability-doc: README.md
    capability-doc: docs/pdfe-vs-acrobat-pro-comparison.md
    capability-doc: docs/acrobat-pro-parity-status.md
    capability-doc: docs/audit/rtm-coverage.md
<!-- END CAPABILITY-DOCS -->

Documents **outside** that set that mention PAdES levels do so about *other*
software or as background, not as a PDFE capability:
`docs/security-forms-annotations-architecture.md` and
`docs/agpl-library-architecture-reevaluation.md` (both evaluate iText /
BouncyCastle and define the levels), `docs/project-analysis-consolidated.md`
(library-selection notes), `docs/harness-audit/**` and `docs/audit/**` (audit
records, including the ones that found this gap). Those are enumerated in the
phase-20 dispositions with the grep that places each one.
