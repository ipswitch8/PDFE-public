# Pinning artifacts whose bytes legitimately move — GitHub #298 and #177

<!-- tracking-banner: exempt -- a decision and evidence record; GitHub Issues remains canonical for anything still open -->

**Status:** mechanism DELIVERED and under test; coverage PARTIAL and the gaps are
enumerated below rather than rounded off.
**Date:** 2026-10-14, pipeline `p96-open-issues` phase-11.
**Tool:** [`tools/gate/pin_pe_normalised.py`](../../tools/gate/pin_pe_normalised.py)
**Pins:** [`pe-normalised-pins.json`](pe-normalised-pins.json)
**Tests:** `tests/test_pe_normalised_pin.py` and
`tests/test_pe_rebuild_determinism.py`, wired as the CTest steps
`pin_pe_normalised` and `pin_pe_rebuild_determinism` (label `supply-chain`; the
second also `slow`). **Neither runs in CI** — see the end of §1 for why, and why
the `security` label they first carried had to come off.

---

## 1. GitHub #298 — PDFE's own binary

### The issue's premise did not hold, and that changed the work

#298 calls a from-source pin tractable because "PDFE's own linked output is
already demonstrably reproducible on this triplet", citing 24/24 byte-identical
artifacts. **Measured 2026-10-14, two links of `pdfe.exe` from identical
objects:**

| build | differing bytes |
|---|---|
| as configured before this phase | **6,164 of 70,291,968** |
| with the determinism flags below | **69 of 57,795,072** |

The 24/24 result is about artifacts **vcpkg** built under
`tools/vcpkg-triplets/x64-windows-repro.cmake`, which sets `VCPKG_LINKER_FLAGS`
for *ports*. It never reached this executable's link. So the flags were the
first deliverable, not the manifest.

### The flags, and which are conditional

`src/app/CMakeLists.txt`, for MSVC, on the `pdfe` target:

* `/Brepro` and `/PDBALTPATH:%_PDB%` — **unconditional**. The first makes the
  COFF timestamp and CodeView GUID content-derived; the second records only the
  PDB basename, so the absolute build path — which on a CI runner contains the
  account name — is not embedded. Both cost nothing.
* `/INCREMENTAL:NO` — **gated behind `PDFE_REPRODUCIBLE_LINK` (default OFF)**.
  An incrementally linked image cannot carry a reproducible layout, so it
  defeats `/Brepro` outright; that triplet records the same discovery (setting
  its linker flags alone did not fix two debug DLLs). It is gated because it
  turns every incremental Debug relink into a full one, which is a real cost to
  anyone iterating. Release is already `/INCREMENTAL:NO` by default, so the
  **shipped** artifact is deterministic without the option; the option exists so
  the property can be measured in Debug.

### The residual 69 bytes are all debug identity, and that is why the pin masks

Seven runs, every one a debug-identity field — the PE header COFF timestamp, the
same timestamp in four debug-directory entries, the `RSDS` CodeView GUID, and
the linker's own content hash in the `IMAGE_DEBUG_TYPE_REPRO` entry. `/Brepro`
derives the timestamp from content, so once the GUID moves the timestamp
follows; five timestamp slots differ for one underlying cause. The GUID pairs
the image with its PDB, and a PDB is not reproducible — the same exclusion list
records 142 `msvc-pdb` files differing between two otherwise byte-identical
builds, noting that `/Brepro` "makes the IMAGE deterministic, not the symbol
file".

So the pin is a SHA-256 over the image with exactly those fields masked, **each
located by parsing the PE structures**. With them masked, the two independent
links above produce the identical pin:

```
repro1.exe  625bc81df8e1111aadd432f3d1151892d6bf2ad68ef5a15f3c26c2324ffa5aa0
repro2.exe  625bc81df8e1111aadd432f3d1151892d6bf2ad68ef5a15f3c26c2324ffa5aa0
masked 8 regions, 80 bytes
```

80 bytes masked of 57.8 MB, covering the 69 that move. Everything else — code,
data, resources, imports, the embedded manifest — is hashed, which is what makes
this a control rather than a formality, and that is asserted in **both**
directions: a forged timestamp must not change the pin, and a byte flipped in
the code must.

### How the property is guarded — and a correction, because the first attempt was the wrong shape

**What this record claimed first, and why it was wrong.** It offered
`test_committed_manifest_matches_what_is_installed` as #298's "test that rebuilds
from source and confirms byte-identical output across repeated builds". That test
rebuilds nothing: it hashes whatever `pdfe.exe` is in the build tree and compares
it against a snapshot taken by hand. The two-link agreement above was a one-time
manual measurement that **no committed test reproduced**.

The `karen` gate failed the phase for it, and added the consequence I had missed:
`pdfe.exe`'s normalised hash changes on **every legitimate change to linked
code** — most commits. A committed snapshot of it would therefore go red for
ordinary work, with no way to tell that apart from a nondeterministic build. That
is `noisy_guard_masks_everything`: a guard that reds routinely gets silenced,
excluded or rubber-stamped, and then it is not protecting anything.

**Both halves of the fix:**

* **`pdfe.exe` is no longer pinned in the manifest at all.** The manifest carries
  a `_pdfe_exe_deliberately_absent` field saying why, so the omission cannot be
  mistaken for an oversight. What remains are the five cache-sourced DLL paths,
  whose bytes are stable until a cache entry changes.
* **`tests/test_pe_rebuild_determinism.py` does what the criterion words**: it
  links `pdfe.exe` twice and compares the two images **to each other**. There is
  no snapshot, so nothing can go stale and a feature commit cannot redden it. Run
  2026-10-14:

  ```
  [i298] two links agreed after masking: 625bc81df8e1111a | raw diff 69 bytes,
         all inside 8 masked region(s)
  1 passed
  ```

  It carries a control against its own vacuity: the two RAW images **must still
  differ**, because the CodeView GUID is minted per link. If they were identical,
  masking would not be what made the hashes agree, and the test would no longer
  be measuring what it claims.

Both are CTest steps — `pin_pe_normalised` and `pin_pe_rebuild_determinism`
(label `supply-chain`; the second also `slow`). The rebuild step is
gated on `PDFE_RUN_REBUILD_DETERMINISM=1` — the convention
`PDFE_RUN_SLOW_SAVE_TESTS` already uses for the XFA benchmarks — and **names that
condition when it skips**, so the reason appears in the run rather than as a
silent absence. It also REFUSES rather than fails when the tree lacks
`-DPDFE_REPRODUCIBLE_LINK=ON`, since reporting a configuration's absence as a code
failure blames the wrong thing.

**Neither runs in CI at all, and an earlier version of this record implied
otherwise.** It said only that they are off the fast-checks lane (true: that lane
does not build). The `claim-check` gate found the part that mattered:
`windows-validation.yml` configures `-DPDFE_ENABLE_SECURITY_TESTS=ON` and runs
`ctest -C Debug -L security`, selecting BY LABEL — and both steps carried
`security`. So that nightly lane would have selected
`pin_pe_rebuild_determinism`, whose `PDFE_RUN_REBUILD_DETERMINISM` gate is set in
**zero** workflow files and whose `-DPDFE_REPRODUCIBLE_LINK=ON` is configured
nowhere in CI: it would skip everything, exit 0, and report a security-labelled
test PASSING having rebuilt nothing. `pin_pe_normalised` would meanwhile verify
workstation-generated pins against a CI tree, which is exactly the mismatch the
manifest's `toolchain` field exists to report.

The `security` label is removed from both. **The honest position: these two
properties are verified ON DEMAND, not continuously.** Wiring them needs the pins
regenerated on the CI toolchain (already a stated prerequisite on GitHub #298)
plus a lane willing to pay for two full links of a 57 MB executable, and that is
tracked rather than faked. A guard labelled for a lane it cannot exercise is
worse than an unlabelled one, because the label reads as coverage.

### What this does NOT claim

**Cross-machine reproducibility.** `__FILE__` still embeds the absolute source
path, so two builds under different roots differ even with all of the above. The
triplet's `/d1trimfile` treatment is the known remedy and is deliberately not
applied to our own compile here. The pin is therefore valid for *the same tree,
the same toolchain, repeated builds* — which is the property #298's criterion
names ("byte-identical output across repeated builds") and no more. The manifest
records the toolchain so a mismatch is reported as a mismatch rather than as
tampering.

**The static-archive hole is narrowed, not closed.** #298's argument is that a
poisoned `.lib` out of the cache alters `pdfe.exe` rather than any pinned file.
That is now detectable: the link's output is pinned, so a changed archive
changes the pin. It is detectable **after a rebuild on the pinning machine** —
it is not an independent check of the archive itself, which remains unpinnable
for the reason the exclusion list gives.

---

## 2. GitHub #177 — the self-timestamping shipped binaries

### First, a correction to how this set is described

The phase criterion, and #298's summary of #177, both say "the six
self-timestamping shipped DLLs". Measured against
`docs/security/vcpkg-cache-unpinned.json`, the `self-timestamping-port` category
is larger and differently shaped:

| | count |
|---|---|
| paths in the category | **36** |
| of those, `.dll` | 21 |
| of those, `.exe` | 15 |
| distinct `.dll` filenames | 8 (`dbus-1-3`, `icudt78`, `icudtd78`, `jpeg62`, `leptonica-1.87.0`, `leptonica-1.87.0d`, `libcrypto-3-x64`, `turbojpeg`) |
| distinct `.exe` filenames | 8 (the dbus tools, `syncqt`) |

"Six" is recoverable as six distinct *libraries* among the DLLs once the debug
variants are folded in, so the phrase is not wrong — but it silently drops **15
executables**, which ship in the same tree for the same reason. Anyone closing
this issue against "the six" would leave those unaddressed without noticing.

### What is installed on this host, and what each one actually contains

| path | installed | `built on:` strings | normalised pin |
|---|---|---|---|
| `bin/libcrypto-3-x64.dll` | yes | **1** | pinned |
| `debug/bin/libcrypto-3-x64.dll` | yes | **1** | pinned |
| `bin/jpeg62.dll` | yes | 0 | pinned |
| `bin/turbojpeg.dll` | yes | 0 | pinned |
| `debug/bin/jpeg62.dll` | yes | 0 | — |
| `debug/bin/turbojpeg.dll` | yes | 0 | — |
| the other **30** paths | **no** | — | — |

Only **6 of 36** paths exist in this tree: it is a minimal install (25 DLLs in
`bin`), not the fuller tree the 7,364-file manifest describes. The 30 absent
paths cannot be measured or pinned here at all, which is the concrete form of
the risk noted when this phase was planned.

### So which remain genuinely unpinnable, and why

* **`libcrypto-3-x64.dll` — now pinnable.** Its residual is one ASCII string,
  `built on: Sat Oct  3 15:44:52 2026 UTC`, and `--mask-build-strings` masks it
  by a fixed-width pattern, so the pin survives a rebuild that changes only the
  date. This is the case #177's first branch asks for and it is delivered.
* **`jpeg62.dll`, `turbojpeg.dll` — pinned, but their nondeterminism is NOT
  explained.** Neither carries a date-like string anywhere. They are in the
  category because two from-source builds differed in bytes, and a string search
  cannot tell me where. The pin covers them (they come from the binary cache, so
  their bytes are fixed until the cache entry changes) but **it is not
  established that a rebuild would reproduce them**, and this record does not
  claim it. Settling it needs two from-source builds of those ports with a byte
  diff, which is hours of build time and was not done.
* **The 30 absent paths — unmeasured, therefore unpinned.** Not a judgement
  about their pinnability; they are simply not here.

### The deployed copies are not the installed ones

Found by the test, not by inspection: `jpeg62.dll` and `libcrypto-3-x64.dll`
exist BOTH in the installed tree and deployed next to the application, with
**different normalised hashes**:

```
vcpkg_installed/.../bin/jpeg62.dll            691688225872601e…
build/vs2022/bin/Debug/jpeg62.dll             57cc9ab6a9b6f26f…
vcpkg_installed/.../bin/libcrypto-3-x64.dll   ef0dd8da58f8eea4…
build/vs2022/bin/Debug/libcrypto-3-x64.dll    65296396fd35d197…
```

Two different builds of the same library ship in the same tree. That is why the
manifest keys entries by **path, not filename** — a first version keyed by
basename and its own test reported both DLLs as CHANGED minutes after pinning
them, because the verifier found the deployed copy and compared it against the
installed one's pin. `vcpkg-cache-unpinned.json` had already said as much by
being "EXPLICIT AND LITERAL — no globs" over 36 paths. A pin that fails in the
direction that looks like tampering is worse than no pin.

---

## 3. What is still open

* **#177 stays open.** Its two actual asks are a scope decision for binary-cache
  integrity (already recorded in
  [`supply-chain-scope-decision.md`](supply-chain-scope-decision.md)) and the
  tessdata SBOM extension, which that record says is unblocked since #181 was
  fixed and "remains a separate, still-open piece of work". Neither is what the
  phase criterion describes, and this mechanism does not close either.
  Separately, that record's **status line is stale**: it still reads "cache
  contents integrity remains ACCEPTED, not fixed" while its own body (and
  `vcpkg_cache_manifest.py`) record 7,364 files pinned.
* **#298 stays open** until the pin is generated and verified on the CI
  toolchain rather than this workstation, and until the `jpeg62`/`turbojpeg`
  question above is settled by measurement.
* The 15 `.exe` members of the category have no pin and no decision recorded.
