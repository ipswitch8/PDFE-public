# Linux parity status

Status of the cross-platform core parity suite on Linux (Defect-42, the Linux
port; not GitHub #42). Measured 2026-11-14 on the GitHub #384 branch, which
was merged to `master` as `d00b8d5e`. Like every count in this repo, these numbers are
a timestamp, not a fact — re-derive them with the commands below.

Environment: Ubuntu 24.10, GCC 14.2, CMake 3.30, Ninja Multi-Config, preset
`linux-gcc`, vcpkg triplet `x64-linux` (static, including static Qt 6.10),
Release configuration.

## How it was run

The filter is the shared generator every validation lane uses, so the selection
is the same one Windows runs:

```
F="$(python3 tools/gate/parity_filter.py)"           # 123 suites from tools/gate/parity_suite.txt
export QT_QPA_PLATFORM=offscreen PDFE_RUN_SLOW_SAVE_TESTS=1
build/bin/Release/pdfe_tests --gtest_filter="$F" --gtest_output=xml:parity.xml
```

All 123 suite patterns match at least one registered test on Linux; none is
missing from `pdfe_tests --gtest_list_tests`. `PDFE_RUN_SLOW_SAVE_TESTS=1` is set
so the two opt-in large-XFA timing tests run rather than skip.

## Results

| Run | Tests | Passed | Skipped | Failed |
|---|---|---|---|---|
| Full parity filter, unfiltered, no exclusions (2026-11-14, after GitHub #386/#387) | 1053 | 1033 | 20 | 0 |

History, oldest first:

| Run | Tests | Passed | Skipped | Failed |
|---|---|---|---|---|
| Before the GitHub #384 phase-2 fixes | 1042 | 1013 | 23 | 6 |
| GitHub #384 as merged, unfiltered | 1042 | 1019 | 20 | 3 |
| GitHub #384 as merged, minus the 3 tests below | 1039 | 1019 | 20 | 0 |

The 11 tests added since GitHub #384 are
`SaveErrorPaths.NoPublishEntryPointReplacesAnUnsavableDestination` (DCA 0118),
the 8 `FixtureImmutabilityGuard.*` self-tests and the 2
`FixtureImmutabilityGuardInstall.*` tests. No exclusion filter is
needed any more.

## Tracking

Open work lives on GitHub, not in this file:

- [GitHub #390](https://github.com/ipswitch8/PDFE/issues/390): Linux port
  umbrella.
- [GitHub #385](https://github.com/ipswitch8/PDFE/issues/385): the save-path
  fixes below, with DCA record
  [0118](audit/dca/0118-posix-publish-replaced-destinations-windows-would-refuse.md).
- [GitHub #387](https://github.com/ipswitch8/PDFE/issues/387): the three
  formerly excluded tests, which now have platform-accurate expectations.
- [GitHub #386](https://github.com/ipswitch8/PDFE/issues/386): the fixture
  rewrite.
- [GitHub #388](https://github.com/ipswitch8/PDFE/issues/388): the macOS half of
  the old deferred item.
- [GitHub #389](https://github.com/ipswitch8/PDFE/issues/389): `Defect #nn`
  autolinking.

## Fixed on Linux in this phase

| Test | Root cause | Fix |
|---|---|---|
| `SaveErrorPaths.TargetIsADirectoryThrows` | MuPDF's `fz_new_output_with_path` calls `remove()` on the target before `fopen`. On POSIX `remove()` deletes an empty directory, so Save As onto an empty directory deleted it and wrote a PDF in its place. | `Document::saveAs` refuses an existing non-regular destination on POSIX before anything touches the filesystem (`src/core/Document.cpp`, POSIX-only block). |
| `SaveErrorPaths.ReadOnlyTargetThrows` | POSIX `rename()` ignores the destination's own mode, so a read-only file was replaced by any stage-and-rename save. | `existingTargetIsWritable` (`faccessat(W_OK, AT_EACCESS)`) is checked in both POSIX publish arms, `atomicReplaceFile` and `StagingFile::commitOver` (`src/core/AtomicFileReplace.cpp`). |
| `OptimizeEngineTest.LinearizeOptionProducesLinearizedOutput` | The POSIX arm of `StagingFile::unpredictableSiblingPath` was a not-supported stub. | It now draws its 128 bits from `csprngHex128` and fails closed like Windows. |
| `UndoStackStressTest.MemoryGrowthPerNInterval` (was skipped) | The test's POSIX memory probe returned 0. | It now reads resident set size from `/proc/self/statm` (`tests/test_stress_investigation.cpp`, Linux arm of the helper; the assertion is unchanged). |

## Formerly excluded — Windows file-locking semantics (GitHub #387, resolved)

These three failed on Linux because each forced its failure with a Windows
kernel behaviour that POSIX does not have: **Windows share-mode / file-lock
semantics with no POSIX analogue**. Until GitHub #387 they were excluded from the
gate run by exact name. They now run, with no exclusion, and pass on Linux.
`HeldPageLocksFullRewriteSave`'s Windows arm is verbatim, now under
`#ifdef _WIN32`. In the two redaction tests, the Windows deny-write pin
(`openDenyWriteReadStream`, then `fclose`) has moved unchanged into
`PublishBlocker`. None of the three has been run on Windows as part of this
change. They are in the shared parity selection, so the Windows lane runs them
when it is dispatched.

- `DocumentSaveFileLockTest.HeldPageLocksFullRewriteSave` has a POSIX arm that
  asserts the contract POSIX can keep. The full-rewrite save SUCCEEDS, the path
  gets a new inode, the held Page still reads the old inode, and the saved file
  re-opens with 3 pages. The test name is kept so both platforms report the
  same test id.
- The two redaction tests force the publish failure through a `PublishBlocker`
  in `tests/test_redaction_apply.cpp`:
  - on Windows it is the same `openDenyWriteReadStream` pin as before;
  - on POSIX it makes the target read-only, which every POSIX publish arm
    refuses (`existingTargetIsWritable`, GitHub #385 / DCA 0118).

  Their vacuity guards (`documentRewritten` must be false) are unchanged, so
  if the blocker stopped blocking, the tests would fail rather than pass on the
  success path.

The original diagnosis, kept for the record:

| Test | What it relies on | Evidence | Follow-up |
|---|---|---|---|
| `DocumentSaveFileLockTest.HeldPageLocksFullRewriteSave` | It expects a full-rewrite `save()` to throw while a `Page` is held, because on Windows the held `fz_document` keeps the source file handle open and `remove()` of the original fails. | `tests/test_document_save_file_lock.cpp:6-13` describes the mechanism as Windows-specific ("A full-rewrite Document::save() on Windows ..."). Linux log: `test_document_save_file_lock.cpp:147: Expected: doc->save(tempPdf) throws an exception. Actual: it doesn't.` On POSIX the rename succeeds and the held Page keeps the old inode, so no changes are lost. `Document::save` has no live-page contract to implement, and `DroppingPageBeforeSaveUnlocksIt` describes dropping the page as the remedy. | Give the test a platform-accurate expectation (on POSIX the save succeeds and the file has the new bytes), decided by the test owner. |
| `RedactionApplyTest.RewriteExceptionPathRemovesBothSideFiles` | It "pins" the target with `openDenyWriteReadStream` so the publish fails. On Windows a handle this process holds blocks a later replace-existing rename. | `tests/test_redaction_apply.cpp:436-449`; `src/core/AtomicFileReplace.h:503-509` (measured, DCA 0021). The POSIX arm of `openDenyWriteReadStream` returns a plain `fopen` and says the deny-write half does not exist. Linux log: `test_redaction_apply.cpp:457: result.documentRewritten Actual: true Expected: false` — the publish was expected to FAIL (the target is pinned). | A POSIX failure-injection seam for the publish step (for example a read-only target, which `existingTargetIsWritable` now refuses), decided by the test owner. |
| `RedactionApplyTest.NoFilesystemResidueFromAnyApplyPath` | Its `publish_blocked` row uses the same `openDenyWriteReadStream` pin. | `tests/test_redaction_apply.cpp:683-701`. Linux log: `test_redaction_apply.cpp:701: r.documentRewritten Actual: true Expected: false [publish_blocked] the publish was expected to fail`. | Same as above. |

## Parity-subset skips at PR #384 (20), pre-existing, none added by the port

Since then: the system-font and font-decode rows run and pass on Linux
(fontconfig, GitHub #390). Only the Windows path-form row still skips, and it
has nothing on POSIX to assert. The table is kept as the record of #384.

| Area | Tests | Reason |
|---|---|---|
| Windows path forms (ADS, MAX_PATH, `\\?\`, UNC) | `SaveTest.SaveAs_RejectsAdsSelectorInFilename`, `SavePathSecurityTest.{WindowsADSRejected, LongPathIsRejectedWithALengthNamingError, ExplicitExtendedLengthLongPathIsAccepted, UNCPathInSaveTarget}`, `PathRoundTrips.{ExtendedLengthPrefixRoundTrip, UncSaveRoundTripViaLoopbackAdminShare}` | Win32 path forms that do not exist on POSIX. |
| Windows system fonts (out-of-scope fontconfig discovery stub area) | `EmbeddedFontAnalyzerTest.AnalyzeFromFileHasPanose`, `PdfFontEmbedderTest.{EmbedArialReturnsResourceName, EmbedMultipleFontsGetsDifferentNames, NullPageReturnsNullopt, NullDocReturnsNullopt}`, `PdfContentWriterTest.{WriteSimpleTextBlockReturnsOperators, WriteTextBlockWithMultiLineHocrData}`, `CidFontEncodingTest.{CidFontPath_ViaEmbeddedFont, EmbeddedCidFont_HasMultiByteEncoding}`, `EmbeddedFontMultiscriptTest.{RealArial_SupportedScriptsDerivedFromGlyphRanges, RealTimes_UncoveredScript_Penalized}`, `VectorSubsetDetectorTest.FindMissingGlyphsReportsUncoveredCodepoints` | Each looks for `arial.ttf`, `times.ttf` or `segoeui.ttf`, mostly at the hard-coded `C:\Windows\Fonts\...` path. Follow-up: fontconfig-based discovery or a POSIX font path in the test helpers. |
| Font-decode text extraction (fontconfig stub area) | `RedactionRoundTripTest.RedactedTextIsNotExtractableAfterApply` | A Linux-only `GTEST_SKIP` that predates the port: the fixture's ToUnicode path relies on Windows-side font resolution. |

## The fixture-rewriting test (GitHub #386, resolved)

`MaliciousPdfTest.RedactionClearsUndoEvidence` used to open
`tests/fixtures/test_single.pdf` directly and apply redactions with the default
`rewriteDocument = true`, which rewrote the tracked fixture in place. Every
later suite reading that fixture then saw empty text. It now redacts a private
copy in its temp directory.

The class-level control is the fixture immutability guard
(`tests/fixture_immutability_guard.cpp`, installed by `tests/test_main.cpp`).
It stamps the size and modification time of every file under `tests/fixtures`
before the first test, re-checks them after every test, and fails whichever
test modified, created or deleted one, naming the file. Run against the old
version of the test, it fails `MaliciousPdfTest.RedactionClearsUndoEvidence`
with `modified: test_single.pdf`. Its self-tests are
`FixtureImmutabilityGuard.*` and `FixtureImmutabilityGuardInstall.*`, both in the
parity set. The second pair fails if `test_main.cpp` stops installing the
guard, and re-runs the test binary against a scratch directory to prove that,
in a real run, the failure lands on the test that touched a file.

## Crash handler, preflight sandbox and PinnedTempFile on Linux

Until this phase, these three Windows security mechanisms were stubs on
Linux. Each now has a Linux implementation. Each one is partial, and the
gaps are listed. The guarantee-by-guarantee mapping is in the pipeline
evidence (`.claude/pipelines/linux-release/evidence/phase-6/windows-guarantees.md`).

**Crash handler** (`src/app/CrashHandler.cpp`). What it does:

- Installs `sigaction` handlers for SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT
  and SIGTRAP, using an alternate signal stack.
- On a fatal signal, writes a text report into a 0700 crash directory. The
  directory is `PDFE_CRASH_DUMP_DIR` if it is acceptable (absolute, no `..`,
  not on a network filesystem), else `AppLocalDataLocation/crashes`. Each
  report is a new 0600 file.
- Re-raises the signal, so the process still dies with it.
- The handler uses only async-signal-safe calls, plus a primed `backtrace()`.
- Keeps at most 150 reports, the same cap as Windows. They are pruned
  oldest-first at install time.

Gaps:

- No minidump: no memory contents, no other threads' stacks.
- A stack overflow on a worker thread is not reported. The alternate stack
  is per-thread and only the main thread gets one.
- A fatal signal shows no dialog.
- Most backtrace frames are `module+offset`. Resolve them with `addr2line`.

**Preflight sandbox** (`src/core/PreflightOpen.cpp`). Between fork and exec,
the helper gets:

- rlimits: no core files, 4 GiB address space, a CPU limit tied to the
  timeout, 256 open files;
- `PR_SET_NO_NEW_PRIVS`;
- a Landlock ruleset that denies every filesystem write and, on ABI 4 and
  later, TCP bind/connect;
- a seccomp filter that returns EPERM for `socket`, `socketpair`, `connect`,
  `bind`, `listen`, `accept`, `accept4`, `sendto`, `sendmsg`, `sendmmsg`,
  `ptrace`, `process_vm_readv`, `process_vm_writev` and the three io_uring
  calls. io_uring is denied because since Linux 5.19 it can create a socket
  without `socket(2)`.

Reads stay allowed. The child reports what it actually applied:
`lastHelperIsolation()` is `Sandboxed` only when all of it applied, and
`lastPosixSandboxReport()` gives each property. A kernel without Landlock or
seccomp still runs the preflight, but it is reported as `Unrestricted`.

Gaps:

- No analogue of the helper's Windows ACG/CIG mitigations.
- The helper can still send signals to its parent. Landlock signal scoping
  needs Linux 6.12.

**PinnedTempFile** (`src/core/TempFileSafety.cpp`). The unpredictable name
and the exclusive, no-follow 0600 creation hold as on Windows. The Windows
"held" property does not: POSIX has no share modes. The substitute detects
changes rather than preventing them:

- `seal()` makes the file 0400 once the producer is done. A by-path open for
  writing then fails unless the attacker first deliberately chmods the file
  back.
- `verifyUnchanged()` checks the inode, mode, ctime, size and a SHA-256 of
  the content. It detects a rename-over, delete-and-recreate or in-place
  rewrite made after the seal.

Which consumers verify:

- **Combine Files** verifies each image wrapper before every by-path open.
- **Clipboard paste** verifies before the first insert.
- **Not verified:** later `InsertImageCommand::redo()` re-reads. That command
  has no hook and no error path, so those re-reads rely on the seal only.

Gaps:

- Not guaranteed: a swap between `verifyUnchanged()` and the consumer's own
  open.
- No protection against root.

Also on Linux, `writeFixedNameTempFile` now works. It refuses a planted
symlink, a hardlink, a FIFO or device, or a file another user owns, and it
truncates only after that check. Until now it always failed on Linux.

## The full suite on Linux (GitHub #390)

The parity subset above is about a sixth of `pdfe_tests`. The whole binary,
now 1017 suites and 6,670 tests, was first run on Linux on 2026-11-14:

| Run | Tests | Passed | Failed | Skipped |
|---|---|---|---|---|
| First full run, before any full-suite fixes | 6,577 | 6,298 | 69 (+2 suite timeouts) | 208 |
| After the fixes below (2026-11-15) | 6,603 | 6,527 | **0** | 76 |
| Font-discovery skips removed, network-mount detection (2026-11-15) | 6,614 | 6,558 | **0** | 56 |
| Crash handler, preflight sandbox, held temp files (2026-11-15) | 6,670 | 6,617 | **0** | 53 |
| qpdf links vcpkg's libjpeg/zlib; POSIX code builds on macOS (GitHub #388), at 2bf31756 (2026-11-16) | 6,670 | 6,617 | **0** | 53 |

How it is run: one process per suite, three at a time
(`tests/scripts/run_full_suite_linux.py`). A crash or hang in one suite cannot
hide the rest. Each process gets its own `XDG_CONFIG_HOME`/`XDG_DATA_HOME`, so
parallel suites do not share a settings store, and its own `TMPDIR`, so the
search-index sweep suites cannot delete an index another suite is building.
Wall-clock budget suites run alone afterwards.

The 53 remaining skips, by reason:

- Windows-only mechanisms with nothing on Linux to assert: NTFS alternate data
  streams, `\\?\` and UNC paths, Win32 DLL presence, handle and GDI counters.
- Linux arms not written yet: the resource-bomb sandbox (Windows Job Objects),
  the child-process XObject depth sweeps, peak-memory measurement. (The crash
  handler and the preflight sandbox have POSIX arms and tests since phase 6.)
- Environment: no CNN font-classifier model file, no virtual printer.
- Child-role cases, which only run when their parent test launches them.

What the full suite found, by kind:

- **Product defects on Linux, fixed:**
  - The autosave recovery directory (full document copies) was created
    world-readable.
  - The POSIX owner-only directory helper followed a symlink planted at its
    path, narrowing and handing back the link's target.
  - Annotation typing never coalesced into one undo step on libstdc++, because
    its `std::hash<int>` is the identity.
  - Font discovery returned nothing. fontconfig is now used through the
    library, not the `fc-match` CLI, and only bindings the configuration
    declares are accepted.
  - Substitute-font selection gave up when the 20 structurally closest faces
    did not cover a script, and could pick a Bengali or console pixel face for
    Latin text.
  - Qt had no CUPS print backend.
  - A file on a network mount (NFS, SMB/CIFS, sshfs) counted as local: the
    remote-path gate only read the path's spelling on Linux. It now asks
    `statfs` (and, for FUSE, the mount's subtype), so a network mount gets
    the policy Windows gives a mapped drive (see building-linux.md).
- **Tests that asserted a Windows mechanism** were given a platform-accurate
  POSIX expectation, never a skip. The kinds:
  - share-mode locks;
  - case-insensitive paths;
  - registry-backed settings;
  - `pclose()` exit status;
  - cmd.exe quoting;
  - `C:\Windows\Fonts` paths, now resolved through
    `tests/system_font_path.cpp`;
  - PowerShell-only gate scripts. The ASan verdict now has a Python port that
    Windows cross-checks.
- **Tests that wrote into tracked files**, caught by the fixture immutability
  guard (GitHub #386):
  - batch output written to `tests/fixtures/batch_output`;
  - a "temporary" signer certificate written to `tests/fixtures`;
  - the account-name leak in the tracked conformance record.
- **Build-configuration calibration.** `PerfStressTest`'s save-exponent bound
  was derived from Debug. In Release the quadratic dedup cost is the same but
  the linear part is 3.7x cheaper, so the exponent reads higher. An `NDEBUG`
  bound, derived the same way, sits below a doubled dedup cost.

The per-failure triage, run by run, is in
[audit/linux-full-suite-triage.md](audit/linux-full-suite-triage.md). Linux test prerequisites
are in [building-linux.md](building-linux.md).
