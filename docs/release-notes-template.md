<!-- tracking-banner: exempt -- a fill-in template for future release notes, not a record of work or a defect list -->
# PDFE v<MAJOR>.<MINOR>.<PATCH>

<one-paragraph summary of the release>

## Changes

- <change>
- <change>

## Checksums

| asset | sha256 |
|---|---|
| `PDFE-<version>-win64.exe` | `<sha256>` |
| `PDFE-<version>-sbom.spdx.json` | `<sha256>` |

## Corresponding source

PDFE is licensed under AGPL-3.0-only. The complete corresponding source for
this release is the tag `v<MAJOR>.<MINOR>.<PATCH>` of
https://github.com/ipswitch8/PDFE — also attached to this release as GitHub's
automatic "Source code" archives. Third-party attributions are in
`THIRD-PARTY-NOTICES.md` at that tag.

<!--
RELEASE CHECKLIST (delete this comment before publishing; the gate half is
mechanical, not advisory):

1. BEFORE uploading any asset, scan the built artifacts:
       python tools/gate/check_release_asset_hygiene.py --paths <installer> <sbom> ...
   This extracts installer payloads (NSIS/LZMA) and scans every byte, both
   string encodings, against the full needle inventory. Exit 0 required.
2. Publish the release WITH this template's "Corresponding source" section --
   the phrase and the repository URL are both machine-checked.
3. AFTER publishing, the release-asset-hygiene workflow re-scans the published
   assets and verifies the corresponding-source section
   (tools/gate/check_release_asset_hygiene.py --release <tag>). It also runs
   on workflow_dispatch for any tag.
4. Do not re-sign or re-upload a previously published signed asset; fix
   forward with a new version instead.
-->
