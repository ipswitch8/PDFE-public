# Third-Party Notices

PDFE itself is licensed under the GNU Affero General Public License v3.0 — see
[`LICENSE`](LICENSE).

This file records third-party material **committed into this repository** whose
own licence requires attribution, or whose provenance would otherwise be
undocumented. It is a **record of facts, not a legal clearance**: nothing here
asserts that any particular use is permitted. Where the facts are incomplete or
a chain of rights is unclear, that is stated rather than smoothed over.

Scope note: this file does **not** yet enumerate the libraries PDFE links
against (Qt, MuPDF, QPDF, FreeType, HarfBuzz, Tesseract, ONNX Runtime, …). Those
are consumed as dependencies rather than committed here, and their notices are
not currently assembled. Consequently this file is **not installed by CPack** —
shipping a third-party notice that covers only a test fixture and omits every
shipped library would be more misleading than shipping none. **If an entry for a
shipped component is ever added, this file must be added to the install rules in
`CMakeLists.txt` alongside `LICENSE`.**

---

## `test_PDFs/crazyones-pdfa.pdf`

A 16,368-byte PDF/A-1b document used as the known-good external reference input
by four tests (see `docs/audit/meta-audit-2026-07/followup/phase-46-dispositions.md`).

| Field | Value |
|---|---|
| Path in this repo | `test_PDFs/crazyones-pdfa.pdf` |
| Size | 16,368 bytes |
| SHA-256 | `f05f2738a1fa8c1d2e1147881fe1a62516a7f8caaf784067790731f56df626c4` |
| Upstream project | **py-pdf/sample-files** |
| Upstream URL | https://github.com/py-pdf/sample-files |
| Upstream path | `021-pdfa/crazyones-pdfa.pdf` |
| Upstream licence | **CC-BY-SA-4.0** (Creative Commons Attribution-ShareAlike 4.0 International) |
| Licence URL | https://creativecommons.org/licenses/by-sa/4.0/ |

The upstream project states that every PDF in it is under CC-BY-SA-4.0, and
GitHub reports `spdx_id: CC-BY-SA-4.0` for the repository. **CC-BY-SA-4.0
requires attribution; this entry is that attribution.**

### How the identification was established

The file was committed with no fetch script, manifest or README, so its origin
was initially undocumented. It was subsequently matched to the upstream file by
SHA-256. The hash above was **re-computed locally against the committed file**
and matches the upstream artifact. Upstream's `files.json` records
`"producer": "GPL Ghostscript 10.00.0"` and
`"creation_date": "2023-04-23T17:59:04"`; both agree with metadata extracted
directly from the committed bytes (`/Producer(GPL Ghostscript 10.00.0)`,
`/CreationDate(D:20230423175904+08'00')`).

### Facts extracted from the file itself

- `%PDF-1.4`; declares PDF/A via `/S/GTS_PDFA1` and an XMP packet containing
  `pdfaid`.
- Producer `GPL Ghostscript 10.00.0`. The file preserves the exact Ghostscript
  command line in a `%%Invocation` comment:

  ```
  %%Invocation: gs -dQUIET -sstdout=? -dPDFA=1 -dBATCH -dNOPAUSE -dNOOUTERSAVE
    -dCompatibilityLevel=1.4 -dPDFACompatibilityPolicy=1 -dUseCIEColor
    -sProcessColorModel=DeviceRGB -sColorConversionStrategy=RGB
    -sOutputICCProfile=? -sDEVICE=pdfwrite -sOutputFile=?
  ```

  (The `?` values are Ghostscript's redaction of the local paths.)
- Fonts are Computer Modern subsets — `SFRM0900`, `SFTI1200`, `SFTI1440` —
  i.e. a LaTeX document distilled through Ghostscript.
- The document carries **no `/Author`, `/Title` or `/Creator`**, no `dc:rights`
  in its XMP, and no copyright string of its own.

### Two rights questions this entry does NOT resolve

1. **Embedded ICC profile.** The output intent is `AdobeRGB1998`, and the
   embedded profile stream carries the string
   `Copyright 2000 Adobe Systems Incorporated`. This is the AdobeRGB1998 profile
   as distributed with Ghostscript. Its terms are Adobe's, not the sample-files
   project's, and are not reproduced or assessed here.

2. **The text.** The document body is Apple's "Think Different" advertising
   copy — the "Here's to the crazy ones" text. The file titles itself
   *The Crazy Ones* and carries the dateline *October 14, 1998*. This is
   **third-party advertising copy**. The CC-BY-SA-4.0 grant is the sample-files
   project's grant over its own compilation and typesetting; it is **not** a
   grant from Apple over the underlying copy, and this entry makes no claim
   about the status of that text.

### Anti-rot control

`tools/gate/check_third_party_notices.py` (wired into `fast-checks.yml`) verifies
that this entry exists and that the SHA-256 recorded above still matches the
committed file. Replacing the fixture without updating this notice, or dropping
the notice while keeping the fixture, fails a fast check. This is the same
anti-rot shape phase-45 applied to risk acceptances: an attribution that can
silently drift away from the artifact it describes is not an attribution.


## Mozilla pdf.js test corpus (fuzz seeds)

- **Upstream:** [mozilla/pdf.js](https://github.com/mozilla/pdf.js)
- **Licence:** Apache-2.0
- **Pinned commit:** `1c8020a7d4e43668ac287a3ecf9a8dbea17e4c56` (tag `v6.3.289`)
- **Imported by:** `scripts/security/import_external_corpus.py`
- **Why:** `tests/security/fuzz/seeds/` was grown entirely from PDFs this project
  wrote. A fuzzer seeded only with its author's idea of a PDF explores the shapes
  that author already thought of; pdf.js's `test/pdfs` is years of real-world
  reader crashes, so it encodes bugs we did not think of (SEC-04 / GitHub #36).
- **Why only eight files (~3.7 KB):** seeds are *mutated*, so small and
  structurally diverse beats large — a 184-byte minimal document is a better seed
  than a 6 MB drawing, and a big corpus mostly costs campaign time re-deriving
  coverage it already had.
- **Selection:** these are genuinely tracked files in pdf.js, not `.link` stubs
  pointing at third-party downloads (which carry no licence guarantee).

Each file is pinned by SHA-256 in `tools/gate/check_third_party_notices.py`, so
it cannot be swapped without the guard noticing.

| file | bytes | sha256 | shape it seeds |
|---|---|---|---|
| `tests/security/corpus/pdfjs/bug1020226.pdf` | 184 | `50e91d373aa313537972519c65b050f8d4d5bd996859fa20756432c2029c3ff6` | **truncated mid-inline-image** -- no `startxref`, no `%%EOF`, 0 pages (corrected 2026-09-18, GitHub #193) |
| `tests/security/corpus/pdfjs/issue15590.pdf` | 191 | `c711bb3357eccec499968ac7b4e286ebd3f49910b458b0596d81731c890eedb6` | **trailer before the objects, no `startxref`**, `/OpenAction` -> JavaScript; xref reconstructed, 0 pages (corrected 2026-09-18, GitHub #193) |
| `tests/security/corpus/pdfjs/extractPages_null_in_array.pdf` | 360 | `37c087807b2a6a0a4545a077ee45ebb23c3d297e0307feebd1897391d23fa843` | a null inside an array -- object-parser edge |
| `tests/security/corpus/pdfjs/clippath.pdf` | 459 | `cbcbf53e8109ed56f9a3fbc696081455c8a5d32cc5cff4e741e85250788dcbff` | clipping-path operators in the content stream |
| `tests/security/corpus/pdfjs/issue4461.pdf` | 476 | `86f561b0b17a9487326fba41f717b7e085ff838bc101c07ebad929c29466f52e` | page-tree shape that regressed a real reader |
| `tests/security/corpus/pdfjs/GHOSTSCRIPT-698804-1-fuzzed.pdf` | 627 | `62e518ca7894e8b8b8ed9927da3007cdc5e9ccc5b111bbb22d6101d4a8d7490e` | already fuzzer-derived: a shape no hand-written seed produces |
| `tests/security/corpus/pdfjs/asciihexdecode.pdf` | 743 | `17cc92aa58ec48f75b01c6b6e7f8c8bade0a36f7295d0f833a16bafae854dc79` | ASCIIHexDecode filter chain |
| `tests/security/corpus/pdfjs/checkbox_no_appearance.pdf` | 694 | `31e82e933f4d4664c6ae1c4418cc44efce0c614c10c85c34b515b38e04e08bcd` | form field with no appearance stream -- synthesis path |

## Tesseract language models (OCR test fixtures)

- **Upstream:** [tesseract-ocr/tessdata_fast](https://github.com/tesseract-ocr/tessdata_fast)
- **Licence:** Apache-2.0
- **Pinned tag:** `4.1.0` (annotated tag object
  `a8ba5063ab8013372a20e300da0c97ee46b92b07`, dereferencing to commit
  `65727574dfcd264acbb0c3e07860e4e9e9b22185`)
- **Why committed rather than fetched:** they gate the OCR suites, which
  otherwise report themselves environmentally blocked. A network fetch at test
  time turns a deterministic suite into one whose result depends on whether a
  CDN answered.
- **Why the variant matters:** `tessdata`, `tessdata_best` and `tessdata_fast`
  publish **different files under identical names**. "`eng.traineddata` from
  Tesseract" is not a provenance statement, which is why the repository is named
  here and not merely the project.

**How the identification was established.** The git blob SHA-1 of each committed
file (`git hash-object`) was compared with the `.sha` the GitHub contents API
reports for all three upstream repositories at tag `4.1.0`, so the match was
made without transferring 16 MB. `eng` and `ara` match `tessdata_fast` and
neither of the other two. `osd` is byte-identical in `tessdata_best` and
`tessdata_fast`, so on its own it does not discriminate — it is `eng` and `ara`
that fix the variant. Full method, the discriminating table, and what the match
does **not** prove (the same bytes are on `tessdata_fast`'s current default
branch, so byte-equality with `4.1.0` does not establish *when* they were taken)
are recorded in `tests/fixtures/tessdata/README.md`.

Each file is pinned by SHA-256 in `tools/gate/check_third_party_notices.py`, so
it cannot be swapped without the guard noticing.

| file | bytes | sha256 | git blob sha1 |
|---|---|---|---|
| `tests/fixtures/tessdata/eng.traineddata` | 4113088 | `7d4322bd2a7749724879683fc3912cb542f19906c83bcc1a52132556427170b2` | `bbef4675053b5b468cdb477053e28b1c698ba08e` |
| `tests/fixtures/tessdata/ara.traineddata` | 1432056 | `e3206d3dc87fd50c24a0fb9f01838615911d25168f4e64415244b67d2bb3e729` | `c8d129c67821c1592cac46836686b057ecc203dc` |
| `tests/fixtures/tessdata/osd.traineddata` | 10562727 | `9cf5d576fcc47564f11265841e5ca839001e7e6f38ff7f7aacf46d15a96b00ff` | `527457ca8f8fe1fda7c2f88bce3c0e4be12be9d0` |

### What this entry does NOT resolve

Apache-2.0 requires the licence text and attribution to travel with a
redistribution. This entry is that attribution; whether PDFE's distribution
of these files additionally needs the upstream `LICENSE` file copied into the
installer payload is a packaging question and is **not** answered here. Nothing
in this document is a licence clearance.
