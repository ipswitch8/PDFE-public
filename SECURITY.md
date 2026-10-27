# Security Policy

## Reporting a security issue

If you find a way to make PDFE leak data, run an attacker-supplied program, or
render unprompted UI from an attacker-controlled PDF:

1. **Open a private security advisory** at
   <https://github.com/ipswitch8/PDFE/security/advisories/new>.
   Please do **not** file a public issue for a security report.
2. Include the PDF that reproduces it, the expected behaviour, the observed
   behaviour, and your PDFE version (Help → About).
3. We acknowledge within **7 days** and aim to fix critical-severity issues
   within **30 days**.

Don't worry about the reproducer being malware. PDFE's phase-3 corpus runner
exists specifically to handle malicious-PDF reports safely, in a network
sandbox.

The full, authoritative text of this process — including what is in and out of
scope — is
[`docs/security-policy.md` § "Reporting a security issue"](docs/security-policy.md#reporting-a-security-issue).

## Supported versions

PDFE is pre-1.0. Security fixes land on `master` and ship in the next release;
there is no long-term-support branch to back-port to.

## Where the rest of the security documentation lives

| Document | What it covers |
|---|---|
| [`docs/security-policy.md`](docs/security-policy.md) | The **user-facing** posture: what PDFE refuses by default, what requires opt-in (JavaScript, external actions, JPEG 2000 rendering), what it prompts before doing, resource limits, and the dependency-bump runbook. |
| [`docs/security-testing.md`](docs/security-testing.md) | The **threat model** (§(a) — the single canonical one) and the internal testing pipeline: corpus runner, SAST, dependency CVE audit, sanitizers, fuzzing, secrets scan. |
| [`tests/docs/security_workflows.md`](tests/docs/security_workflows.md) | Which security CI lanes fire automatically, which are dispatch-only, and which are retired. |
| [`docs/security/unsigned-binary-risk-acceptance.md`](docs/security/unsigned-binary-risk-acceptance.md) | Code-signing and update-channel posture, and the risk formally accepted. |
| [`docs/audit/dca/`](docs/audit/dca/) | Defect-cause analyses for security defects that escaped, including DCA 0011 (the JPEG 2000 heap overflow behind the default-deny JPX gate). |

---

*This file exists so GitHub's Security tab and the "Report a vulnerability"
affordance resolve to a real process (meta-audit gap `SEC-91`). It is a
front door, not a second source of truth — `docs/security-policy.md` is
authoritative and `SecurityDocIndex` in `tests/test_doc_drift_guards.cpp`
asserts this file keeps pointing at it.*
