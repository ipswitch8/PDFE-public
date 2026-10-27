# vcpkg overlay source verification

<!-- tracking-banner: exempt -- a dated verification artifact; GitHub Issues remains canonical for anything still open -->

**GENERATED FILE — do not hand-edit.** Regenerate with:

```
python tools/gate/verify_vcpkg_overlays.py \
    --out docs/security/vcpkg-overlay-source-verification.md \
    --date <YYYY-MM-DD> --reevaluate <YYYY-MM-DD>
```

- **Verified:** 2026-09-07
- **Re-evaluate by:** 2027-03-07

Meta-audit **SEC-88** (GitHub #67). `tools/vcpkg-overlays/{mujs,gumbo}` re-pin upstream vcpkg's SHA512 because Codeberg regenerated the source archives. Re-pinning a hash is a decision to trust re-served bytes, and until now the justification lived only in a prose comment that a later reviewer could not re-run. This is that justification, re-derived and committed.

**The original mujs check is no longer reproducible by anyone.** It compared the new Codeberg archive against the OLD Codeberg archive, and those bytes no longer exist — Codeberg serves only the regenerated form. So the comparison below is deliberately a DIFFERENT and stronger one: against the upstream author's own release tarball at mujs.com. Two hosts, two packagings, one source tree.

**These overlays are temporary.** They exist only because upstream vcpkg's pins are stale. Remove them once upstream repins; the re-evaluation date above is the prompt to check.

---

## mujs 1.3.8

- Codeberg archive: `https://codeberg.org/ccxvii/mujs/archive/1.3.8.tar.gz`
  - SHA512 `528cd8a8a60fdef64fe5624739b773277990ea6c531f15b0219043076b0c8242d63ccb6fe2b0dd77420b9d74fe16b3f16259489667db37436d21ed8c745c15a6`
  - overlay pin `528cd8a8a60fdef64fe5624739b773277990ea6c531f15b0219043076b0c8242d63ccb6fe2b0dd77420b9d74fe16b3f16259489667db37436d21ed8c745c15a6`
  - **MATCHES the overlay pin**

- Upstream author's release: `https://mujs.com/downloads/mujs-1.3.8.tar.gz`
  - SHA512 `8156326b7cd2734242ce91ac2e80ae7a3cae30448c8e1a6601e6a3b747db34c713eec630a3f873d5abb1d19a3e8a84845964c8ff03de595ea9acd371278f2623`
  - archive bytes differ from Codeberg's: **True** (expected — different packaging)

- Tree comparison (Codeberg vs upstream author):
  - files: 53 vs 53, 53 in common
  - content-differing common files: **0**
  - only in Codeberg: 0
  - only in upstream: 0

- `mujs.h` SHA512 identical across sources: **True**
  - `c2c2183f0380b1226ff4e39c631f2ffb24380de6c11b912b8b8ec4675f2d94b5f437679f3a7c30686b006e4246f7173cebebf1d89f895addadced76485d37759`

## gumbo-parser 0.12.3

- Codeberg archive: `https://codeberg.org/gumbo-parser/gumbo-parser/archive/0.12.3.tar.gz`
  - SHA512 `4e0bc0e8c387bad18cde5f9998930afedbe4e38d2e984dd14c87486f4b86e5eda6089b8c11ceda0827be0a18a075bfeeafedb068694f30fe3f2b3a8be0238b07`
  - overlay pin `4e0bc0e8c387bad18cde5f9998930afedbe4e38d2e984dd14c87486f4b86e5eda6089b8c11ceda0827be0a18a075bfeeafedb068694f30fe3f2b3a8be0238b07`
  - **MATCHES the overlay pin**

- **No independent source was reachable.** Both the GitHub mirrors tried return 404, so gumbo gets a STRUCTURAL check only — strictly weaker than mujs's cross-source tree comparison, and recorded as such rather than presented as equivalent.

  - PASS — configure.ac declares gumbo 0.12.3 (AC_INIT version = 0.12.3)
  - PASS — src/gumbo.h present (src\gumbo.h)
  - PASS — COPYING present (doc/COPYING)

