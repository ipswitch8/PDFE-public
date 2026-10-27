# Provenance of the committed Tesseract language models

<!-- tracking-banner: exempt -- a provenance record for three binary fixtures, not a work list; GitHub Issues remains canonical for anything still open -->

Meta-audit gap `SEC-90` (GitHub #69). Until this file existed, three
multi-megabyte binaries sat in the repository with **nothing stating where they
came from, which upstream revision they are, or under what licence they are
redistributed**. That is not a documentation nicety: a committed binary with no
provenance cannot be re-verified by anyone later, so a legitimate model bump and
a substitution look identical in review.

## What these files are

| File | Bytes | SHA-256 | Git blob SHA-1 |
|---|---|---|---|
| `eng.traineddata` | 4,113,088 | `7d4322bd2a7749724879683fc3912cb542f19906c83bcc1a52132556427170b2` | `bbef4675053b5b468cdb477053e28b1c698ba08e` |
| `ara.traineddata` | 1,432,056 | `e3206d3dc87fd50c24a0fb9f01838615911d25168f4e64415244b67d2bb3e729` | `c8d129c67821c1592cac46836686b057ecc203dc` |
| `osd.traineddata` | 10,562,727 | `9cf5d576fcc47564f11265841e5ca839001e7e6f38ff7f7aacf46d15a96b00ff` | `527457ca8f8fe1fda7c2f88bce3c0e4be12be9d0` |

## Upstream

**Repository:** `tesseract-ocr/tessdata_fast`
**Tag:** `4.1.0` (annotated tag object `a8ba5063ab8013372a20e300da0c97ee46b92b07`,
which dereferences to commit `65727574dfcd264acbb0c3e07860e4e9e9b22185`)
**Licence:** Apache-2.0 (the licence of the `tessdata_fast` repository; PDFE
redistributes the files unmodified)

Naming **which** variant matters more than it looks. `tessdata`,
`tessdata_best` and `tessdata_fast` are three different repositories publishing
**different files under identical names** — different sizes, different
accuracy, different runtime cost. "It is `eng.traineddata` from Tesseract" is
not a provenance statement.

## How the variant was determined — measured, not assumed

The git blob SHA-1 of a file is computable locally
(`git hash-object <file>`) and is exactly what the GitHub contents API returns
as `.sha`, so the three files can be matched against all three upstream
repositories **without downloading 16 MB of models**:

```bash
# locally
for f in ara eng osd; do git hash-object "$f.traineddata"; done

# upstream, for each of tessdata / tessdata_best / tessdata_fast
for repo in tessdata tessdata_best tessdata_fast; do
  for f in ara eng osd; do
    gh api "repos/tesseract-ocr/$repo/contents/$f.traineddata?ref=4.1.0" --jq .sha
  done
done
```

Result (run 2026-09-08):

| File | matches `tessdata` | matches `tessdata_best` | matches `tessdata_fast` |
|---|---|---|---|
| `eng.traineddata` | no | no | **yes** |
| `ara.traineddata` | no | no | **yes** |
| `osd.traineddata` | no | yes | **yes** |

`osd.traineddata` is **byte-identical in `tessdata_best` and `tessdata_fast`**,
so on its own it does not discriminate between the two. `eng` and `ara` do, and
both land on `tessdata_fast`. That is why all three rows are shown rather than a
summary: an "osd matches" line alone would have supported the wrong conclusion.

### What this does NOT prove

The three blobs are **also** the current contents of `tessdata_fast`'s default
branch — the files have not changed since the `4.1.0` tag. So the match
establishes *byte-equality with what `tessdata_fast` @ `4.1.0` holds*, and does
not establish that they were fetched at that tag rather than later. For the
purpose the hashes exist for — re-verifiability, and detecting substitution —
that is sufficient; for "which release were these taken from", the honest answer
is that the repository does not record it and the bytes cannot recover it.

## To re-verify

```bash
python - <<'PY'
# -*- coding: utf-8 -*-
import hashlib, pathlib
expected = {
    "eng.traineddata": "7d4322bd2a7749724879683fc3912cb542f19906c83bcc1a52132556427170b2",
    "ara.traineddata": "e3206d3dc87fd50c24a0fb9f01838615911d25168f4e64415244b67d2bb3e729",
    "osd.traineddata": "9cf5d576fcc47564f11265841e5ca839001e7e6f38ff7f7aacf46d15a96b00ff",
}
here = pathlib.Path("tests/fixtures/tessdata")   # run from the repository root
bad = 0
for name, want in expected.items():
    got = hashlib.sha256((here / name).read_bytes()).hexdigest()
    ok = got == want
    bad += not ok
    print(("OK   " if ok else "WRONG"), name, got)
raise SystemExit(1 if bad else 0)
PY
```

## Why they are committed rather than fetched

They gate OCR tests, which otherwise skip. `tests/CMakeLists.txt` stages all
three next to `pdfe_tests.exe` as a post-build step, and several suites
(`OcrPipeline*`, `OcrLanguage*`, `TessdataFontDetection*`, and the heavy
autosave-capture probes) check for `eng.traineddata` and report themselves as
environmentally blocked when it is missing. A network fetch at test time would
convert a deterministic suite into one that is green or skipped depending on
whether a GitHub CDN answered — see `docs/manual-verification.md` on
OCR-dependent rows.

## Attribution, and the gate that keeps it honest

The three files are now entries in `THIRD-PARTY-NOTICES.md` and in the
hand-maintained `ENTRIES` table of `tools/gate/check_third_party_notices.py`,
which re-hashes each committed artifact and requires the notice to still state
the upstream project, its URL, the licence id, the size and the SHA-256. So the
provenance above is **re-derived on every run of that gate**, not asserted once
in prose.

They were not there before. The gate reported *"9 committed third-party
artifact(s) attributed … OK"* while these three sat outside its table entirely —
a hand-maintained inventory reports completeness over its own contents, which is
the one thing it cannot check. Adding a committed third-party artifact means
adding a row there; that instruction is in the checker's own comment and had
been missed for these files.

## Known gap, stated rather than left to be found

These three files are **not in the SBOM**.
`tests/security/reports/deps/sbom.spdx.json` is generated from vcpkg's installed
package set, so it enumerates `tesseract` (the engine) and knows nothing about
the models the engine loads. `SupplyChainInventory.SbomEnumeratesEveryDirectVcpkgDependencyWithAVersion`
therefore passes while three redistributed third-party binaries sit outside the
dependency audit's scope entirely. Recorded here and in
`docs/security-testing.md` § (a); not fixed, because extending the SBOM
generator to non-vcpkg committed artifacts is a change to
`scripts/security/run_deps.ps1` and its fail-closed check, not a documentation
edit.
