# The Cloudflare R2 build store

How a runner that never built gets the binaries, when some runners are on the
LAN and some are not.

## Why R2 and not a share

The runners are a **mix of local and remote machines**. A LAN share serves only
the local ones, so the remote ones would need a second transport — and two
restore paths are two things to drift apart. R2 serves both with one code path.

The numbers, from Cloudflare's pricing page rather than memory:

| | |
|---|---|
| Storage | **10 GB-month free**, then $0.015/GB-month |
| Class A ops (writes) | 1M/month free |
| Class B ops (reads) | 10M/month free |
| **Egress to the internet** | **free, at every tier** |

Egress is the property that decides it. The runtime closure is ~2.5 GB raw and
~1 GB compressed, and every consumer pulls it. A transport that charged for
egress would make cross-machine reuse cost more than the build it saves — which
is exactly the calculation that rejected GitHub artifacts for the single-machine
case originally.

GitHub Actions artifacts remain as a **fallback leg**, not a replacement. A
machine with no R2 configuration behaves exactly as it did before R2 existed.

## Order of sources

`ensure-build-tree` tries, in order:

```
workspace  ->  local per-SHA store  ->  Cloudflare R2  ->  GitHub artifact  ->  fail loudly
```

R2 sits third: after the two free same-machine paths, and **before** the
artifact leg, because artifacts are bounded by a quota this account has already
exhausted while R2 is not.

Whatever the source, the restored store is fed to the **same verifier**, so all
five refusals apply unchanged — no marker, wrong-commit marker, incomplete
restore, nothing published, and the cold-machine case. R2 does not get a
verifier of its own; a second one would be a second thing to keep correct.

## Configuring a runner

Four **machine-level** environment variables. Set them on the machine, for the
account the runner service runs as — not in a workflow `secrets:` block:

```powershell
[Environment]::SetEnvironmentVariable('PDFE_R2_ENDPOINT',          'https://<accountid>.r2.cloudflarestorage.com', 'Machine')
[Environment]::SetEnvironmentVariable('PDFE_R2_BUCKET',            'pdfe-build-store', 'Machine')
[Environment]::SetEnvironmentVariable('PDFE_R2_ACCESS_KEY_ID',     '<token id>',       'Machine')
[Environment]::SetEnvironmentVariable('PDFE_R2_SECRET_ACCESS_KEY', '<token secret>',   'Machine')
```

Restart the runner afterwards — it reads the environment at process start.

The `aws` CLI must be present; R2 is S3-compatible and the action shells out to
it. If R2 is configured and `aws` is missing, the action **says so and fails**
rather than quietly falling through.

### Why machine-level and not workflow secrets

On a self-hosted runner the **machine is the trust boundary**. Workflow secrets
are readable by every job in the repository, including ones with no business
writing to the build store, and they follow the repo — which matters
particularly because this repository is being prepared to go public. Machine
environment keeps the credential with the machine that was deliberately
provisioned to hold it.

### Consume-only machines MUST get a read-only token

Not "should", not "ideally". This is the control the store's integrity actually
rests on, and the phase-3 `security-audit` gate failed this phase for stating it
as a recommendation.

Issue **bucket-scoped** tokens, and give every machine that does not build a
**read-only** one. Only `pdfe-builder` machines get write.

The reasoning is in the next section: with a single shared write credential,
nothing inside the object can authenticate it, because the party you are trying
to authenticate against holds the same credential. Read-only consumers reduce
"attacker with bucket write" to "attacker who compromised a builder" — and an
attacker who controls a builder already controls the build output, so no new
capability is created. That reduction *is* the security property.

This is an operational control. Nothing in the repository can verify which token
a given machine holds, so no guard enforces it — which is exactly why it is
written here as a requirement rather than left implicit.

## Failure behaviour, and why each case behaves as it does

| State | Behaviour |
|---|---|
| No R2 variables at all | Falls through to the artifact leg, silently. A machine never given R2 settings is not misconfigured. |
| **Some** variables set | **Fails, loudly.** Partial configuration is never intentional — somebody meant this machine to use R2 and it would not, and they would conclude R2 "does not work" rather than "was never finished". |
| Configured, `aws` absent | **Fails, loudly.** Same reasoning. |
| Configured, object not found | Falls through. Nothing published for this commit yet is the ordinary first-run case, not an error. |
| Configured, auth/endpoint failure | **Fails, loudly.** A configured store that does not answer is broken, not absent. |

The miss-vs-broken distinction is the one that matters most: collapse it and
either every first run fails, or every real failure is swallowed.

## Provenance — what R2 does not give you

`actions/download-artifact` supplied a fork check, a `head_sha` match and
producing-workflow identity for free. An object store supplies none of it. What
stands in:

- the object key is **SHA-scoped**, so a wrong-commit object cannot be addressed
  by accident;
- the downloaded store passes through the shared verifier, whose commit-marker
  check compares `published-sha.txt` against `GITHUB_SHA`;
- write access is held only by machines carrying credentials, and this workflow
  is `workflow_dispatch`-only, so a fork cannot publish;
- **bucket-scoped tokens, read-only for every consume-only machine** — see
  above, and note this is the load-bearing control, not a nice-to-have.

### CORRECTION: the marker is not an integrity control against a hostile writer

This section previously called the `published-sha.txt` check "the real integrity
control … an attacker who can write to the bucket still cannot make a store
claim to be a commit it is not". **That was wrong, and the phase-3
`security-audit` gate failed the phase for it.**

The marker travels **inside the same zip the writer controls**. The publish step
writes it into the store and *then* compresses the whole directory; the restore
step extracts that archive and checks the file it just unpacked. So an attacker
with bucket write simply sets the marker to whatever SHA they are impersonating.
The check confirms the writer labelled their own payload consistently. It
authenticates nothing about *who* wrote it.

What the marker genuinely does, and all it does:

- catches **accidental** mis-keying — a store published under the wrong object
  name, or a stale object served for a different commit;
- catches **corruption** — a truncated or partially-written archive.

Both are real and worth having. Neither is a defence against a hostile writer.

The actual defence is credential scoping: consumers hold read-only tokens, so
the only parties who can write are the builders — and a compromised builder
already controls the binaries it produces, marker or no marker. A stronger
scheme would need a **second trust root** (a signing key held only by the build
job and verified by consumers), which is a materially larger change than this
transport swap and is recorded here as the upgrade path rather than pretended
into existence.

## What is actually in the object

The zip is the whole per-SHA store directory, which is the runtime closure —
`bin/`, the two vcpkg DLL directories, and the CTest manifests.

**It also contains PDBs.** `bin\Debug` is copied wholesale and `pdfe.pdb` lives
there (360 MB uncompressed), so every machine with read access to the bucket
gets debug symbols: internal symbol names and build paths. The publish step
describes itself as shipping the "runtime closure only", and debug info is not
strictly runtime — this is recorded because the phase-3 `security-audit` gate
asked what the object really contains and the honest answer is "slightly more
than the label suggests".

It is also *useful*: a PDB in the store is what makes a crash dump from that
commit symbolisable at all (see
`docs/audit/crash-evidence/2026-08-13-pdfe-uaf.md`, where its absence cost the
only diagnosis of a live use-after-free). So this is a stated property, not a
defect to remove — but a private bucket is a precondition, not a detail.

`crash-pin.txt` does **not** reach the object. It is written by the `report`
job, into the local store only, after the R2 upload has already happened, and
nothing re-uploads afterwards.

## The bucket also holds crash dumps

Under `pdfe-crashdumps/<commit>/run-<run id>/`, keyed by **commit and run** so
two runs of the same commit cannot overwrite each other's evidence — which a
SHA-only key would do silently, and exactly when a crash is intermittent and you
want both captures.

They are here rather than in a GitHub artifact for a security reason, not a cost
one. A minidump of this application can contain document passwords, decrypted
content from copy-restricted PDFs, OCSP/CRL tokens and absolute host paths (see
`docs/audit/crash-evidence/2026-08-13-pdfe-uaf.md`). Artifacts of a **public**
repository are downloadable by anyone with read access — which is everyone — and
this repository is being prepared to go public.

**If R2 is not configured, dumps are published nowhere.** Not to an artifact,
not anywhere; they stay on the runner's disk and the step says so loudly. That
is the deliberate fail-*safe* direction: a fallback to an artifact would restore
the exact exposure this arrangement removes, and a fallback that reintroduces
the hazard is not a fallback.

`tools/gate/check_crashdump_artifact.py` enforces it — no artifact may name the
dump staging directory, the old `windows-validation-crashdumps` artifact may not
return, the logs artifact must keep excluding `ui-out/crashdumps/**`, and the
unconfigured-R2 path must stay loud. Mutation-verified against the live
workflow.

Give crash dumps their own, longer lifecycle rule than the build closures: a
build store is worthless after its commit passes, but a dump is evidence.

## Lifecycle

Set a bucket lifecycle rule to expire `pdfe-build/` objects after a few days.
The store is only reusable for its own commit and the restore refuses any other,
so nothing has value past the run that follows it — and 10 GB holds roughly ten
compressed closures, which a short expiry keeps comfortable.

## Guard

`tools/gate/check_ensure_build_tree.py` asserts the R2 leg is ordered before the
artifact leg, refuses partial configuration, distinguishes a miss from a broken
store, takes no credentials from workflow secrets, and that **no credential is
ever committed** — that last rule applies to the guard's own source too, which
is why its test sample is assembled at runtime rather than written as a literal.
