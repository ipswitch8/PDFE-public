# Linux parity status

Status of the cross-platform core parity suite on Linux (Defect #42, the Linux
port; not GitHub #42). Measured 2026-11-14 on branch
`linux-port/compile-and-link`. Like every count in this repo, these numbers are
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
| Full parity filter, unfiltered | 1042 | 1019 | 20 | 3 |
| Parity filter minus the 3 tests below (gate run) | 1039 | 1019 | 20 | 0 |

Before the phase-2 fixes the unfiltered run was 1013 passed, 23 skipped and 6 failed.

## Tracking

Open work lives on GitHub, not in this file:

- [GitHub #390](https://github.com/ipswitch8/PDFE/issues/390): Linux port
  umbrella.
- [GitHub #385](https://github.com/ipswitch8/PDFE/issues/385): the save-path
  fixes below, with DCA record
  [0118](audit/dca/0118-posix-publish-replaced-destinations-windows-would-refuse.md).
- [GitHub #387](https://github.com/ipswitch8/PDFE/issues/387): the three
  excluded tests.
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

## Excluded from the gate run — Windows file-locking semantics

These three fail on Linux because each forces its failure with a Windows
kernel behaviour that POSIX does not have: **Windows share-mode / file-lock
semantics with no POSIX analogue**. That is the same platform gap behind the
out-of-scope PinnedTempFile stub, though neither test goes through
`PinnedTempFile` itself. They are excluded by exact name with a negative filter.
They are not disabled, and they still run and fail in the unfiltered run above.

| Test | What it relies on | Evidence | Follow-up |
|---|---|---|---|
| `DocumentSaveFileLockTest.HeldPageLocksFullRewriteSave` | It expects a full-rewrite `save()` to throw while a `Page` is held, because on Windows the held `fz_document` keeps the source file handle open and `remove()` of the original fails. | `tests/test_document_save_file_lock.cpp:6-13` describes the mechanism as Windows-specific ("A full-rewrite Document::save() on Windows ..."). Linux log: `test_document_save_file_lock.cpp:147: Expected: doc->save(tempPdf) throws an exception. Actual: it doesn't.` On POSIX the rename succeeds and the held Page keeps the old inode, so no changes are lost. `Document::save` has no live-page contract to implement, and `DroppingPageBeforeSaveUnlocksIt` describes dropping the page as the remedy. | Give the test a platform-accurate expectation (on POSIX the save succeeds and the file has the new bytes), decided by the test owner. |
| `RedactionApplyTest.RewriteExceptionPathRemovesBothSideFiles` | It "pins" the target with `openDenyWriteReadStream` so the publish fails. On Windows a handle this process holds blocks a later replace-existing rename. | `tests/test_redaction_apply.cpp:436-449`; `src/core/AtomicFileReplace.h:503-509` (measured, DCA 0021). The POSIX arm of `openDenyWriteReadStream` returns a plain `fopen` and says the deny-write half does not exist. Linux log: `test_redaction_apply.cpp:457: result.documentRewritten Actual: true Expected: false` — the publish was expected to FAIL (the target is pinned). | A POSIX failure-injection seam for the publish step (for example a read-only target, which `existingTargetIsWritable` now refuses), decided by the test owner. |
| `RedactionApplyTest.NoFilesystemResidueFromAnyApplyPath` | Its `publish_blocked` row uses the same `openDenyWriteReadStream` pin. | `tests/test_redaction_apply.cpp:683-701`. Linux log: `test_redaction_apply.cpp:701: r.documentRewritten Actual: true Expected: false [publish_blocked] the publish was expected to fail`. | Same as above. |

## Remaining skips (20) — pre-existing, none added by the port

| Area | Tests | Reason |
|---|---|---|
| Windows path forms (ADS, MAX_PATH, `\\?\`, UNC) | `SaveTest.SaveAs_RejectsAdsSelectorInFilename`, `SavePathSecurityTest.{WindowsADSRejected, LongPathIsRejectedWithALengthNamingError, ExplicitExtendedLengthLongPathIsAccepted, UNCPathInSaveTarget}`, `PathRoundTrips.{ExtendedLengthPrefixRoundTrip, UncSaveRoundTripViaLoopbackAdminShare}` | Win32 path forms that do not exist on POSIX. |
| Windows system fonts (out-of-scope fontconfig discovery stub area) | `EmbeddedFontAnalyzerTest.AnalyzeFromFileHasPanose`, `PdfFontEmbedderTest.{EmbedArialReturnsResourceName, EmbedMultipleFontsGetsDifferentNames, NullPageReturnsNullopt, NullDocReturnsNullopt}`, `PdfContentWriterTest.{WriteSimpleTextBlockReturnsOperators, WriteTextBlockWithMultiLineHocrData}`, `CidFontEncodingTest.{CidFontPath_ViaEmbeddedFont, EmbeddedCidFont_HasMultiByteEncoding}`, `EmbeddedFontMultiscriptTest.{RealArial_SupportedScriptsDerivedFromGlyphRanges, RealTimes_UncoveredScript_Penalized}`, `VectorSubsetDetectorTest.FindMissingGlyphsReportsUncoveredCodepoints` | Each looks for `arial.ttf`, `times.ttf` or `segoeui.ttf`, mostly at the hard-coded `C:\Windows\Fonts\...` path. Follow-up: fontconfig-based discovery or a POSIX font path in the test helpers. |
| Font-decode text extraction (fontconfig stub area) | `RedactionRoundTripTest.RedactedTextIsNotExtractableAfterApply` | A Linux-only `GTEST_SKIP` that predates the port: the fixture's ToUnicode path relies on Windows-side font resolution. |

## Observed outside the parity set

`MaliciousPdfTest.RedactionClearsUndoEvidence` (`tests/test_malicious_pdf_security.cpp`)
opens `tests/fixtures/test_single.pdf` directly and applies redactions with the
default `rewriteDocument = true`. That rewrites the tracked fixture in place,
and on later runs every suite reading that fixture sees empty text. The test is
not in the parity filter. It should work on a temp copy, and that is a fix for
the test owner.
