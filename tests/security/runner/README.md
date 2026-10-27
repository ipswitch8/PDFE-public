# Phase 3: Sandboxed PDFE Runner

Drives `PDFE.exe` through every payload in `manifest.json` while a
loopback listener and Windows Firewall rule watch for any callback
attempt.

**Primary success metric:** zero loopback connection attempts across
the entire corpus. Any single PDF that triggers a callback fails the
phase, regardless of crash status. The framing is binary because the
corpus is synthetic — the PDFs *are not* malware, so any callback
is anomalous by definition.

## Files

| File | Purpose |
|---|---|
| `listener.py` | Multi-protocol loopback listener (TCP / UDP / HTTP / DNS). Bound to 127.0.0.1 only. Records every connection. |
| `decision.py` | Pure decision logic: `decide(observation, expected) -> (final_decision, reason)`. Unit-tested. |
| `run_corpus.py` | Live corpus runner. Bails without admin elevation. Owns the firewall rule + firewall-log parser. |
| `test_decision.py` | Pytest unit tests for `decide()`. Run via `ctest -L security`. |
| `test_listener.py` | Pytest unit tests for the listener (loopback-only, no PDFE). |
| `test_firewall_log.py` | Pytest unit tests for the pfirewall.log parser (synthetic logs in tmp dir, no admin). |
| `verify_optin.ps1` | Phase 1 opt-in invariant check. |
| `bootstrap.ps1` | Sets up Python venv + tool inventory. |

## How to run the corpus (live)

**Prerequisites**:

1. **Build PDFE.exe** at `build/vs2022/bin/Debug/PDFE.exe` (or pass `--pdfe-exe` to override).
2. **Generate the corpus**: `.\scripts\security\gen_malicious_corpus.ps1`.
3. **Add Windows Defender exclusion** for `tests/security/corpus/malicious_pdf/` (else AV will eat payloads silently).
4. **Open an elevated PowerShell** (admin). The runner refuses to start without elevation because it needs `netsh advfirewall` to install a per-binary outbound block rule for PDFE.

Run:

```powershell
python tests\security\runner\run_corpus.py
```

Output:

```
tests/security/reports/corpus_run_<ts>.json    # per-PDF records + summary
tests/security/reports/corpus_run_<ts>.md      # human-readable
tests/security/reports/corpus_run_<ts>_loopback.log  # canonical exfil evidence
```

Exit code 0 = all 69 payloads pass; non-zero = at least one PDF
triggered a callback / crash / hang / memory blowup.

## Decision logic at a glance

```
ANY loopback connection attempt        -> fail / 'exfiltration_attempt'
Non-loopback outbound attempt blocked   -> fail / 'exfiltration_attempt_nonloopback'
Crash                                   -> fail / 'crash'
Hang past --timeout-s                   -> fail / 'hang'
Peak RSS exceeds --rss-ceiling-mb       -> fail / 'memory_blowup'
expected=refuse but file opened         -> fail / 'opened_when_should_refuse'
expected=open_clean but file didn't open -> fail / 'failed_to_open'
otherwise                                -> pass
```

The exfil-attempt check takes precedence over crash because data
exfiltration before a crash is still exfiltration.

## Architecture

`run_corpus.py` is the only piece that touches PDFE / pywinauto /
netsh. Everything else (`listener.py`, `decision.py`) is pure:

- The listener accepts connections and records events.
- `build_observation()` packs listener events + process metrics
  into a dict with the schema `decide()` expects.
- `decide()` returns `(final_decision, reason)` from observation +
  expected behavior. No I/O, no globals.
- `summarize()` aggregates per-PDF records into the phase verdict.

This split is what makes the unit tests possible without admin or
PDFE.exe.

## CTest registration

`tests/security/CMakeLists.txt` registers three tests under the
`security` label:

- `security_runner_decision_unit` — runs `pytest test_decision.py`
- `security_runner_listener_unit` — runs `pytest test_listener.py`
- `security_runner_firewall_log_unit` — runs `pytest test_firewall_log.py`

All three run via `ctest -L security`; none run under the default
`ctest` invocation per the opt-in posture.

The actual corpus run is **not** a CTest entry — it requires admin
elevation and 30+ minutes of wall-clock time, neither of which fits
the unattended-CI model. It's invoked manually by `run_corpus.py`.

## Why no live corpus baseline is committed

Phase 3 acceptance criterion 10 says "phase commits the runner code
and a baseline report showing zero loopback attempts." This commit
ships the runner code and unit tests. The baseline report itself
must be produced from an actual elevated run against a built
PDFE.exe — that's a separate hands-on step documented in
`docs/manual-verification.md` (and in this README) but not gated by
the C++ build.

When you run the corpus and get a clean baseline, copy
`tests/security/reports/corpus_run_<ts>.json` into
`tests/security/regression_baselines/corpus_run_baseline.json`.
Phase 9's regression check will then compare future runs against
that baseline.

## How `non_loopback_blocked` is populated

The runner enables Windows Firewall dropped-packet logging on
`currentprofile` at start (`netsh advfirewall set currentprofile
logging droppedconnections enable`). For each payload, it snapshots
`pfirewall.log`'s byte size before and after the launch+wait, then
parses the appended bytes for `DROP` lines whose destination is not
`127.0.0.1` / `::1` / `0.0.0.0` / `::`. Those entries become the
payload's `non_loopback_blocked` list.

Caveats:

- The Windows Firewall log doesn't record which process triggered the
  drop. Attribution to PDFE.exe relies on the per-binary block rule
  being the only thing producing drops on the test machine. For a
  shared host, run on a clean VM.
- The log file rotates on size cap (default 4 MB). The parser detects
  rotation (file shrunk below the saved start position) and rereads
  from the new EOF; on a single mid-run rotation the worst-case is
  double-counting one payload's drops, never silent loss.
- Logging is disabled at end-of-run (`droppedconnections disable`)
  unless the runner is killed mid-run.

## Limitations

- **ICMP echo (ping) is not listened for.** Raw ICMP socket needs
  admin and is platform-finicky. The phase-3 firewall rule blocks
  outbound ICMP, and any attempt shows up in the Windows Firewall
  log via the parser above (recorded as `non_loopback_blocked` with
  `protocol='icmp'`, ports 0).
- **DNS over UDP is heuristically classified.** A query whose flags
  byte looks DNS-shaped is tagged `udp_dns`. Other UDP datagrams are
  tagged `udp`. Both count equally as exfiltration attempts.
- **The listener binds well-known ports (80, 443, 8080, 8443, 9999,
  53).** If a payload uses a hardcoded port we don't bind, the
  outbound firewall rule still catches it and the firewall-log
  parser surfaces it as a `non_loopback_blocked` entry.
- **AV interference.** If your AV deletes payloads from
  `malicious_pdf/` between corpus generation and runner execution,
  those PDFs are reported as `payload not on disk (AV deletion?)`
  rather than treated as a pass.
- **Hang detection** triggers only when `app.kill()` fails to
  terminate the process within 3 s (`_KILL_GRACE_S`). A PDFE that
  opens a payload cleanly and idles to the wait-budget deadline is
  the *normal* path -- the runner closes it; this is not a hang.

---

## ETW process-creation sensor (GitHub #62) — `etw_process_sensor.py`

### The gap it closes

`tests/security_oracles.h` carries a job-object-scoped spawn monitor and states
its own irreducible limit:

> *"a spawn brokered ENTIRELY outside our job (created by a different process —
> e.g. an RPC/DCOM broker — so it never joins our job) cannot be attributed
> without reintroducing machine-wide false positives."*

That residual is real. A PDF exploit that reaches a broker (WMI, DCOM, the task
scheduler) has its process created **by the broker**, so the new process never
joins our job object and the job monitor cannot see it. Widening that monitor to
the whole machine would trade a false negative for constant false positives on a
developer or CI machine.

An ETW `Microsoft-Windows-Kernel-Process` session filtered on **parent image**
is the right instrument: the kernel reports every process creation machine-wide
together with its parent, so a brokered spawn is attributable by asking *which
image was the parent* rather than *did it land in my job*.

### Why it is opt-in rather than a GoogleTest

**Enabling the kernel-process provider requires administrator rights.** The
default suite must stay runnable unelevated and broker-agnostic, so this is its
own lane.

> **PRECISION CORRECTION, measured 2026-09-14 (p85 phase 2).** This line used to
> say *"starting a kernel ETW **session** requires administrator rights"*. That
> is not where the privilege boundary sits, and the imprecision concealed a
> false-green path. Unelevated, with the account in `Performance Log Users`:
>
> ```
> logman create trace PDFE-ProbeSensor -ets \
>     -p Microsoft-Windows-Kernel-Process 0x10 0x5 -o probe.etl -nb 16 16 -bs 1024
>
>   Error: Access is denied.  Try running this command as an administrator.
>   CREATE_EXIT=-2147024891   (0x80070005)
> ```
>
> …**yet `logman query PDFE-ProbeSensor -ets` then reports `Status: Running`**,
> with buffers written and a 1 MiB `.etl` on disk. The *session* starts fine
> unelevated; it is the *provider enable* that is denied, and it fails while
> **leaving a running session with no provider attached**.
>
> The consequence was confirmed directly: spawning `whoami.exe` during such a
> session and decoding it gave a 3-line CSV with **0 `ParentImageName` events**,
> and **`tracerpt` exited 0**. A blind sensor decodes cleanly, attributes
> nothing, and looks exactly like a clean run.
>
> Three fixes followed, all in `etw_process_sensor.py`: a failed `logman create`
> now stops the session it orphans; the elevation message names the real
> boundary; and — the important one — `capture_verdict()` treats a decode
> containing **zero process events of any kind** as exit **2**, never 0. Over a
> multi-second machine-wide kernel-process trace, a live Windows box always
> observes some process creation, so zero means blind, not quiet. That rule has
> its own self-tests and was mutation-verified (collapsing it to `return 0` was
> caught by the `EMPTY decode is 'did not run' (2), not 'clean' (0)` case).

```
# decision logic only — needs no privilege, runs anywhere
python tests/security/runner/etw_process_sensor.py --self-test

# real capture — REQUIRES AN ELEVATED SHELL
python tests/security/runner/etw_process_sensor.py --capture \
    --parent-image pdfe.exe --seconds 30
```

Exit codes: `0` clean, `1` a spawn was attributed, **`2` the sensor could not
run**. 2 is deliberately distinct from 0 — *"the sensor did not run"* must never
be reported as *"nothing was spawned"*, which is the false-green this lane
exists to avoid.

### Status, measured 2026-09-06 — read this before citing it

The development account is **not elevated** (`IsInRole(Administrator)` →
`False`), so **the end-to-end capture has NOT been executed here.** That remains
true as of 2026-09-14, and the measurement above now explains *why* at the right
level of detail: the account **is** in `Performance Log Users`, which is enough
to create the session but not to enable the kernel provider on it.

What has been verified is the decision core, via `--self-test` (**9/9** as of
2026-09-14; 5/5 before), which covers:

- a hostile child (`cmd.exe`) brokered under `pdfe.exe` **is** attributed
- the legitimate `pdfe_preflight.exe` child **is not**
- another parent's child is not attributed to us
- an empty capture attributes nothing — the sensor can say *no*, not only *yes*
- **an empty decode is exit 2 (did not run), not exit 0 (clean)** — the
  false-green closed on 2026-09-14
- a *populated* decode with nothing attributable is still a real exit 0, so the
  rule above cannot be satisfied by always returning 2
- a populated decode with an attributed spawn is still exit 1

### How this complements — and does not replace — the job-scoped guard

Both guards are live, and they answer different questions:

| | job-scoped guard | ETW lane |
|---|---|---|
| Where | `tests/security_oracles.h` (`ProcessSpawnGuard` / `SpawnEventMonitor`) | `tests/security/runner/etw_process_sensor.py` |
| Scope | processes joining **our job object** | **machine-wide**, filtered on parent image |
| Privilege | none | provider enable needs admin |
| Runs | default GoogleTest suite, every run | opt-in lane only |
| Blind to | a spawn brokered entirely outside our job (RPC/DCOM/task scheduler) | nothing structurally — but blind in practice without elevation, which it now reports as 2 |

The ETW lane exists precisely to address the job guard's stated irreducible
residual. It is **not** a replacement: it needs privilege the default suite must
not require, so removing the job guard would leave the common
in-process-spawn case uncovered on every ordinary run.

Both are exercised:

- **job-scoped guard** — by the GoogleTest suite, including dedicated
  positive/negative cases: `ProcessSpawnGuardScoping.DetectsInTreeSpawn`
  (proves it still says *yes* to a real `cmd.exe` in our tree),
  `ProcessSpawnGuardScoping.ObservesTransientInTreeSpawn` (a fast-exiting
  spawn), plus ~10 suites asserting `spawnGuard.newlySpawned() == ""` across
  the malicious-PDF, link-action, form-action and CVE-replay corpora.
- **ETW lane** — by `python tests/security/runner/etw_process_sensor.py
  --self-test`, run 2026-09-14: **PASS, 9/9**.

That self-test earned its place immediately: it caught a real defect in the
parser, where `"C:\Program Files\PDFE\pdfe.exe"` was truncated at the space to
`C:\Program`, so **every attribution against an installed build would have
silently missed**. A capture-only lane would have shipped that.

### Consequence for the triage

The elevation requirement means **#62 is not "individually workable" in the
sense the 2026-09-06 triage assumed** — verifying it end-to-end needs a
privilege none of the triage's four factors model. Recorded rather than papered
over; the same class as the `#94` caveat already flagged in
`docs/audit/triage-2026-09/stage6-plan-input.md`.
