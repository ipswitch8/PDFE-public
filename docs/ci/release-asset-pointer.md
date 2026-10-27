<!-- tracking-banner -->
> **STATUS: LIVE OPERATIONAL POINTER — maintained, not historical.**
>
> This says where release assets are **right now**. If an asset moves, change
> this file. **GitHub Issues is the canonical tracker for anything still
> open** — see [`docs/TRACKING.md`](../TRACKING.md).

# Where the release assets live after the 2026-10 migration

This repository was recreated in October 2026 to leave behind 287 enumerated
pre-rewrite commits that GitHub was still serving by SHA. Reasoning:
[`docs/legal/publication-route-evaluation-2026-10.md`](../legal/publication-route-evaluation-2026-10.md).
Release **assets** did not all come with it, and that is deliberate.

## What is here

| release | asset | SHA-256 | why it moved |
|---|---|---|---|
| `assets-v1` | `font_embedder.onnx` | `476a7432…d7d32b46` | **The build needs it.** `CMakeLists.txt` and `windows-validation.yml` resolve it **by name** from this release — by name rather than by asset id, precisely so it survives a re-upload. Verified byte-identical to the pre-migration copy. |
| `assets-v1` | the SHA-512-named CI blob | `10e85480…118291d9` | Same CI-assets release; small, unsigned. Verified byte-identical. |
| `v0.3.0` | `PDFE-0.3.0-sbom.spdx.json` | `17865a6e…777a2a5a` | Provenance for the v0.3.0 installer. **On `v0.3.0`, not on `assets-v1`** — see below. |

### The SBOM is on `v0.3.0` rather than `assets-v1`

The migration plan originally placed it on `assets-v1`. It lives on **`v0.3.0`**
instead, which is where it sat on the pre-migration repository, so it stays
**co-located with the artifact it describes**. An SBOM filed among CI assets
reads as describing those assets; this one describes an installer.

The consequence to keep in mind: that installer is **not here** (see below), so
the SBOM on this repository documents a binary that lives on the retired
repository. If you are correlating the two, that is where to look.

## What deliberately did NOT move

| asset | still at | why |
|---|---|---|
| `PDFE-0.3.0-win64.exe` | the retired pre-migration repository | **Code-signed.** The standing instruction for this migration was not to re-upload or re-sign it. The signature covers the file rather than the URL, so re-uploading the same bytes is arguably not re-signing — but that was not a call to make unilaterally, so the artifact stays where it was signed and published. |
| `PDFE-0.1.0-win64.exe` | the retired pre-migration repository | Same reasoning, and nothing depends on it. |

**So `v0.3.0` here is a release with no installer.** If you need the signed
installer, it is on the retired repository; a new one must come from a fresh run
of [`scripts/sign/Sign-Artifacts.ps1`](../../scripts/sign/Sign-Artifacts.ps1),
not a copy.

## Why `v0.3.0` exists here at all, with no binary on it

Because omitting it would have silently broken the shipped update check, and
the mechanism is worth stating precisely — it is not obvious.

`src/app/UpdateChecker.cpp` reads this repository's releases feed and picks a
"latest" tag. With only `assets-v1` present it does **not** degrade gracefully:

* `normalizeTag("assets-v1")` strips a leading `v`/`V` only, so it returns the
  tag **unchanged and non-empty** — the loop's `if (tag.isEmpty()) continue;`
  does not skip it.
* `parseSegments` breaks at the first `-`, leaving the segment `"assets"`, and
  `segmentToNumber` stops at the first non-digit and returns **0**.
* So the tag parses as version **0**, and `compareVersions("0.3.0", "assets-v1")`
  returns **`Newer`** — the client concludes it is *ahead of the latest release*.

The result is not a visible error. It is a client that can **never offer an
update again**, for any future version, because the only release it can see
parses lower than anything real. A tag with no binary restores the version
signal without re-publishing a signed artifact.

Measured both ways rather than reasoned about —
[`scripts/migration/check_update_feed.py`](../../scripts/migration/check_update_feed.py)
reimplements the C++ selection and parsing and runs it against the live feed:

```
feed containing only assets-v1   -> selected 'assets-v1' -> parsed [0] -> Newer   (INERT)
feed with the binary-less v0.3.0 -> selected '0.3.0'                   -> UpToDate (healthy)
```

Both repositories currently report `UpToDate`, i.e. the migrated repo behaves
identically to the pre-migration one.

## Verifying

```sh
python scripts/migration/check_update_feed.py

gh release view assets-v1 --repo ipswitch8/PDFE \
  --json assets --jq '.assets[] | select(.name=="font_embedder.onnx") | {name,size,digest}'
```

The ONNX lookup must return **exactly one** match, and its digest must equal the
value pinned in the table above — carried from the source repository, not
re-derived.

> **If a future release adds the installer here**, update this file in the same
> change. A pointer that has drifted is worse than no pointer, because it reads
> as current.

## The README release badge is static while this repository is private

`README.md` line 9 carries a **static** `release-0.3.0` badge, not a dynamic
`shields.io/github/v/release` shield. That is deliberate and it is a
consequence of the repository being private, not of the migration.

Measured, with a positive control, 2026-10-22:

```
img.shields.io/github/v/release/ipswitch8/PDFE.json
  -> {"message":"repo not found","color":"red"}
img.shields.io/github/v/release/jqlang/jq.json        (public control)
  -> {"message":"jq-1.8.2","color":"blue"}
```

shields.io fetches anonymously and has no credentials, so against a private
repository it renders **red, and not a version**. The control proves the
endpoint itself works and that the failure is specific to visibility.

> **The `message` text varies; do not key a check on it.** Two observations of
> the same private repository returned `"repo not found"` and
> `"no releases or repo not found"`. Quoting one variant as *the* response was
> a mistake on my part — a third party's error wording is not a contract. The
> stable assertions are `color` is red and `message` is not a version string;
> the public control is what makes a red reading meaningful rather than
> ambiguous.

This was found only because a rewritten acceptance criterion required the badge's
*rendered* output to be verified rather than asserted. Before that, three
separate parties — including me, twice — had reasoned from "the GitHub API
reports v0.3.0, so the badge shows v0.3.0". The API and shields are not the same
reader, and the badge is rendered by the one without access.

> **At publication, when this repository becomes public**, restore the dynamic
> shield:
>
> ```
> [![Release](https://img.shields.io/github/v/release/ipswitch8/PDFE?include_prereleases&label=release)](https://github.com/ipswitch8/PDFE/releases/latest)
> ```
>
> and drop the static one, which cannot track releases and will go stale at the
> next tag. Re-check the rendered JSON rather than assuming it works — that is
> the mistake this note exists to record.
