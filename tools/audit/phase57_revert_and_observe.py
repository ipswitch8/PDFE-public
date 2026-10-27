#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Revert-and-observe for WIN-87 phase-57, sites #3/#4/#5.

WHAT IT DOES
------------
For each of the three publish sites migrated in phase-57, it reverts THAT SITE
ALONE to its pre-phase-57, path-based form, rebuilds, and runs the site's own
hostile-filesystem cases against it. The cases are discriminating only if they
go RED there and GREEN again after the restore.

  site #3  src/redaction/RedactionManager.cpp   .redact_tmp -> .redact_out
  site #4  src/ui/MainWindow.cpp                the QPDF redaction rewrite
  site #5  src/ui/MainWindow.cpp                the alignment flush

Sites #4 and #5 live in the same file and are reverted in separate rounds, so a
red case can be attributed to one site rather than to "something in MainWindow".

WHY THE REBUILD IS PROVEN, NOT ASSUMED
--------------------------------------
Earlier in this pipeline a revert-and-observe produced a COMPLETE false finding
(F69): shutil.copy2 preserved the source mtime, MSBuild concluded the object was
up to date and skipped the compile, and five deterministic runs measured code
that was never built. It has since recurred three more times, once in the
product build itself. So every build here is checked twice:

  * the patched/restored source is force-touched (os.utime to now), AND
  * the object file's mtime must be strictly newer after the build than before,
    AND the build log must contain a compile line for that source file.

If either check fails the script ABORTS rather than reporting a measurement.

AND THE RUN IS CHECKED TOO. A gtest filter typo produces an empty run and a
clean exit; the script aborts unless every NAMED case actually appears in the
XML. A case that does not execute is not a case that passed.

NON-WITNESSES ARE LABELLED, NOT COUNTED. A case that stays green under its own
site's revert is reported as NOT A WITNESS with the recorded reason, or as
UNEXPLAINED — which is a non-zero exit. "A test that cannot red is decoration"
is only enforceable if the report says which is which instead of printing a
tally.
"""

import argparse
import io
import json
import os
import subprocess
import sys
import time
import xml.etree.ElementTree as ET
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
BUILD = REPO / "build" / "vs2022"
EXE = BUILD / "bin" / "Debug" / "pdfe_tests.exe"
CMAKE = Path(
    r"C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE"
    r"\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
)

REDACTION_SRC = REPO / "src" / "redaction" / "RedactionManager.cpp"
REDACTION_OBJ = (BUILD / "src" / "redaction" / "pdfe_redaction.dir" / "Debug"
                 / "RedactionManager.obj")
MAINWINDOW_SRC = REPO / "src" / "ui" / "MainWindow.cpp"
MAINWINDOW_OBJ = BUILD / "src" / "ui" / "pdfe_ui.dir" / "Debug" / "MainWindow.obj"
IMAGEEXPORTER_SRC = REPO / "src" / "exports" / "ImageExporter.cpp"
IMAGEEXPORTER_OBJ = (BUILD / "src" / "exports" / "pdfe_exports.dir" / "Debug"
                     / "ImageExporter.obj")
BATCHENGINE_SRC = REPO / "src" / "batch" / "BatchEngine.cpp"
BATCHENGINE_OBJ = (BUILD / "src" / "batch" / "pdfe_batch.dir" / "Debug"
                   / "BatchEngine.obj")
PDFA_SRC = REPO / "src" / "compliance" / "PdfAConverter.cpp"
PDFA_OBJ = (BUILD / "src" / "compliance" / "pdfe_compliance.dir" / "Debug"
            / "PdfAConverter.obj")
PDFUA_SRC = REPO / "src" / "compliance" / "PdfUAConverter.cpp"
PDFUA_OBJ = (BUILD / "src" / "compliance" / "pdfe_compliance.dir" / "Debug"
             / "PdfUAConverter.obj")

# ---------------------------------------------------------------------------
# Site #3 — RedactionManager::apply.
#
# RE-POINTED FOR OPTION A (2026-08-11). The first version of these hunks
# reverted a pin-the-published-temp narrowing that no longer exists: the fixed
# code no longer calls document->save(tempPath) at all, so every hunk below used
# to match zero times and the script would have aborted (correctly) rather than
# measured anything. The revert now restores the FULL pre-phase-57 three-step
# by-path relay - save to "<doc>.redact_tmp" by path, REOPEN IT BY NAME with
# QPDF, write "<doc>.redact_out" by path, publish with the path-based helper -
# which is the shape whose window A the cases are about.
#
# Note the reverted code also needs core/AtomicFileReplace.h, which the fixed
# file deliberately no longer includes; hunk 1 puts it back.
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# THE REVERTED TREES NEED A HELPER THAT NO LONGER EXISTS.
#
# phase-58 DELETED pdfe::core::atomicReplaceFileWithRetry (the path-based
# publish helper) once its last production caller was migrated. Every revert
# below reconstructs a PRE-phase-57 body, and those bodies called it — so from
# phase-58 onward a reverted tree would not COMPILE, and revert-and-observe
# would stop being reproducible for sites #3/#4/#5 at exactly the moment the
# class was declared closed.
#
# So each revert injects this shim alongside the reverted body. It is a faithful
# copy of the deleted helper (bounded 10 x 100 ms, rename-over only, never a
# remove), which is the point: the measurement must reconstruct the VULNERABLE
# shape, not an approximation of it.
#
# It lives in an anonymous namespace inside the reverted file, so it cannot leak
# into the restored tree even if a restore is interrupted — the restore rewrites
# the whole file from the .orig capture anyway.
# ---------------------------------------------------------------------------
REVERT_SHIM = """#include "core/AtomicFileReplace.h"
#include <chrono>
#include <filesystem>
#include <system_error>
#include <thread>

namespace {
/// phase-58 revert shim — a faithful copy of the DELETED
/// pdfe::core::atomicReplaceFileWithRetry. Present only in a reverted tree.
bool revertAtomicReplaceWithRetry(const std::filesystem::path& from,
                                  const std::filesystem::path& to,
                                  std::error_code& ec,
                                  int attempts = 10,
                                  std::chrono::milliseconds delay =
                                      std::chrono::milliseconds(100)) noexcept
{
    if (attempts < 1) attempts = 1;
    for (int attempt = 0; attempt < attempts; ++attempt) {
        if (pdfe::core::atomicReplaceFile(from, to, ec)) return true;
        if (attempt + 1 < attempts) std::this_thread::sleep_for(delay);
    }
    return false;
}
}  // namespace
"""

SITE3_REVERT = [
    (
        """// core/AtomicFileReplace.h is deliberately NOT included any more (WIN-87
// site #3, phase-57). This file used to reach for atomicReplaceFileWithRetry
// and, briefly, for openDenyWriteReadStream/StagingFile directly. The whole
// staging lifecycle now lives in Document::saveThroughExternalRewrite, and
// leaving the header included would keep the path-based primitive exactly one
// already-present #include away from being reached for again.
#include "core/Error.h"
""",
        """#include "core/AtomicFileReplace.h"
#include "core/Error.h"
""",
    ),
    (
        """        try {
            document->saveThroughExternalRewrite(
                [](std::FILE* in, std::FILE* out) {
                    // Ownership of BOTH streams is ours on every path — a
                    // stream still open in THIS process blocks the publishing
                    // rename. QpdfWrapper takes each over as it receives it
                    // (close_file = true, on its throwing paths too), so the
                    // only gap to cover is `out` while openFromStream runs.
                    struct OutGuard {
                        std::FILE* f;
                        ~OutGuard() { if (f) std::fclose(f); }
                    } outGuard{out};

                    pdfe::core::QpdfWrapper qpdf;
                    qpdf.openFromStream(in, "<staged redaction bake handle>");
                    std::FILE* owned = outGuard.f;
                    outGuard.f = nullptr;
                    qpdf.saveToStream(owned, false);
                });
            result.documentRewritten = true;
        } catch (const std::exception& e) {
""",
        """        auto tempPath = document->filePath();
        tempPath += ".redact_tmp";
        auto outputPath = document->filePath();
        outputPath += ".redact_out";
        struct RevertSideFileCleanup {
            const std::filesystem::path& tmp;
            const std::filesystem::path& out;
            ~RevertSideFileCleanup() {
                std::error_code e;
                std::filesystem::remove(tmp, e);
                std::filesystem::remove(out, e);
            }
        } revertCleanup{tempPath, outputPath};

        try {
            // THE PRE-PHASE-57 THREE-STEP BY-PATH RELAY. Window A is the
            // interval between the save publishing ".redact_tmp" and QPDF
            // reopening it BY NAME.
            document->save(tempPath);
            document->registerScratchTempForCleanup(tempPath);
            {
                pdfe::core::QpdfWrapper qpdf;
                qpdf.open(tempPath);
                qpdf.save(outputPath, false);
            }
            {
                std::error_code renameEc;
                if (!revertAtomicReplaceWithRetry(
                        outputPath, document->filePath(), renameEc)) {
                    throw std::runtime_error(
                        "Failed to replace original with the redacted rewrite "
                        "(original left intact): " + renameEc.message());
                }
            }
            result.documentRewritten = true;
        } catch (const std::exception& e) {
""",
    ),
]

# ---------------------------------------------------------------------------
# Site #4 — MainWindow's QPDF redaction rewrite. Back to: closeDocument()
# FIRST (no handle taken beforehand), then re-resolve the SOURCE by name, then
# ".redact_out" by path, then the path-based publish.
# ---------------------------------------------------------------------------
SITE4_REVERT = [
    (
        """    std::error_code srcEc;
    std::FILE* sourceStream =
        pdfe::core::openDenyWriteReadStream(filePath, srcEc);
    if (!sourceStream) {
        throw std::runtime_error(
            "Could not take an exclusive read handle on the redacted document "
            "before the rewrite (original left intact): " + srcEc.message());
    }
    struct SourceGuard {
        std::FILE* fp;
        ~SourceGuard() { if (fp) std::fclose(fp); }
        std::FILE* release() { auto* p = fp; fp = nullptr; return p; }
    } source{sourceStream};

    // Close the document to release ITS locks — required for the publish, and
    // now harmless, because the bytes QPDF will read are already pinned by the
    // handle above rather than by whatever is at this name afterwards.
    closeDocument();

""",
        """    closeDocument();

    auto revertTempOut = filePath;
    revertTempOut += ".redact_out";
    struct RevertOutCleanup {
        const std::filesystem::path& p;
        ~RevertOutCleanup() { std::error_code e; std::filesystem::remove(p, e); }
    } revertOutCleanup{revertTempOut};

""",
    ),
    (
        """    std::error_code stageEc;
    auto staged = pdfe::core::StagingFile::createBeside(filePath, stageEc);
    if (!staged) {
        throw std::runtime_error(
            "Could not create the staging file for the redacted rewrite "
            "(original left intact): " + stageEc.message());
    }
    std::FILE* stagedStream = staged->openDuplicateStream(stageEc);
    if (!stagedStream) {
        throw std::runtime_error(
            "Could not open the staging stream for the redacted rewrite "
            "(original left intact): " + stageEc.message());
    }

    // OWNERSHIP OF `stagedStream` PASSES TO QPDF ONLY ONCE saveToStream IS
    // ENTERED. Until then this scope owns it, and a throw from openFromStream
    // (any QPDF parse failure of the document we just wrote) used to leak it.
    // That is not a mere handle leak: the duplicate keeps the StagingFile's
    // FileDispositionInfo unlink PENDING, so the staging file survives on disk
    // for the life of the process, at an unpredictable name, beside the user's
    // document — contradicting the "no residue of any kind" property this phase
    // claims. Found by the non-gate security review.
    struct StagedStreamGuard {
        std::FILE* fp;
        ~StagedStreamGuard() { if (fp) std::fclose(fp); }
        std::FILE* release() { auto* p = fp; fp = nullptr; return p; }
    } stagedGuard{stagedStream};

""",
        "",
    ),
    (
        """        pdfe::core::QpdfWrapper qpdf;
        qpdf.openFromStream(source.release(), filePath.string());
        qpdf.saveToStream(stagedGuard.release(), false);
    }

    {
        std::error_code flushEc;
        staged->flushToStableStorage(flushEc);  // best-effort
    }
""",
        """        pdfe::core::QpdfWrapper qpdf;
        qpdf.open(filePath);
        qpdf.save(revertTempOut, false);
    }
""",
    ),
    (
        # The seam fire is KEPT, repointed at the reverted ".redact_out". It
        # marks the same semantic instant on both sides — staged output written,
        # not yet published — and it is the test seam, not the fix. `staged` no
        # longer exists in the reverted body, so this hunk is mandatory for the
        # reverted tree to compile at all.
        """    fireRedactionOutputStagingHookForTesting(staged->path());
""",
        """    fireRedactionOutputStagingHookForTesting(revertTempOut);
""",
    ),
    (
        """    if (!staged->commitOverWithRetry(filePath, renameEc)) {
""",
        """    if (!revertAtomicReplaceWithRetry(revertTempOut, filePath,
                                                renameEc)) {
""",
    ),
]

# ---------------------------------------------------------------------------
# Site #5 — MainWindow's alignment flush. Back to the predictable
# "<savedPath>.align.tmp" written by path and published by path.
# ---------------------------------------------------------------------------
SITE5_REVERT = [
    (
        """        std::FILE* sourceStream = preTaken.release();
        if (!sourceStream) {
            return false;
        }
        std::error_code stageEc;
        auto staged = pdfe::core::StagingFile::createBeside(savedPath, stageEc);
        if (!staged) {
            std::fclose(sourceStream);
            return false;
        }
        std::FILE* stagedStream = staged->openDuplicateStream(stageEc);
        if (!stagedStream) {
            std::fclose(sourceStream);
            return false;
        }

        // Both streams are consumed (and closed) by the flush, whatever it
        // does — including on its throwing path.
        alignmentPipeline_->flushThroughHandles(sourceStream, savedPath,
                                                stagedStream, staged->path());

        {
            std::error_code flushEc;
            staged->flushToStableStorage(flushEc);  // best-effort
        }

        // THE PUBLISH WINDOW, marked deterministically. The staging artifact is
        // fully written and has not yet been renamed over the user's document —
        // the exact instant an attacker must hit to have PDFE publish their
        // bytes. In this build the StagingFile still holds it exclusively, so a
        // substitution attempted here fails; in the pre-phase-57 build it is an
        // ordinary file at ".align.tmp" and the substitution lands. Null in
        // production.
        fireAlignStagingWindowHookForTesting(staged->path());

        std::error_code ec;
        if (!staged->commitOverWithRetry(savedPath, ec)) {
            // Nothing was ever removed first, so savedPath is untouched; the
            // StagingFile destructor removes the unpublished staging file.
            return false;
        }
""",
        # The seam fire is KEPT, pointed at the reverted artifact. It marks the
        # same semantic instant on both sides — staging written, not yet
        # published — and it is the test seam, not the fix. Reverting it would
        # leave the case measuring luck again, which is how the site-#4 case
        # came to pass against vulnerable code.
        """        if (std::FILE* revertUnused = preTaken.release()) {
            std::fclose(revertUnused);
        }
        auto revertTmp = savedPath;
        revertTmp += ".align.tmp";
        alignmentPipeline_->flush(savedPath, revertTmp);
        fireAlignStagingWindowHookForTesting(revertTmp);
        std::error_code ec;
        if (!revertAtomicReplaceWithRetry(revertTmp, savedPath, ec)) {
            std::error_code rmec;
            std::filesystem::remove(revertTmp, rmec);
            return false;
        }
""",
    ),
]

# Site #5 has TWO independent windows and they need separate rounds, or a red
# case cannot be attributed to either. SITE5_REVERT above restores the old
# ".align.tmp" staging file — the WRITE side. This one restores the old
# SEQUENCING on the READ side and nothing else: the caller stops pinning the
# just-saved document before closeDocument(), and the flush goes back to
# resolving the name itself afterwards. The staging hardening stays in place,
# so a red here is the input window and only the input window.
#
# The `fireSaveCloseWindowHookForTesting()` line is kept verbatim on both sides
# of every hunk. It is the test seam, not the fix; reverting it would remove the
# deterministic instant and leave the case measuring luck — which is exactly how
# the site-#4 case came to pass against vulnerable code.
SITE5SRC_REVERT = [
    (
        """            std::error_code alignSrcEc;
            std::FILE* alignSource =
                pdfe::core::openDenyWriteReadStream(filePath, alignSrcEc);
            closeDocument();
            // THE WINDOW, marked deterministically. Every handle on the
            // just-saved document has been released; the flush has not read it
            // yet. Null in production.
            fireSaveCloseWindowHookForTesting();
            flushAlignmentPipelineIfPending(filePath, alignSource);
            openFile(reopenPath);
""",
        """            closeDocument();
            fireSaveCloseWindowHookForTesting();
            flushAlignmentPipelineIfPending(filePath, nullptr);
            openFile(reopenPath);
""",
    ),
    (
        """            std::error_code alignSrcEc;
            std::FILE* alignSource =
                pdfe::core::openDenyWriteReadStream(filePath, alignSrcEc);
            closeDocument();
            // Same window as onSaveFile — see there. Null in production.
            fireSaveCloseWindowHookForTesting();
            flushAlignmentPipelineIfPending(filePath, alignSource);
            openFile(fileName);
""",
        """            closeDocument();
            fireSaveCloseWindowHookForTesting();
            flushAlignmentPipelineIfPending(filePath, nullptr);
            openFile(fileName);
""",
    ),
    (
        """        std::FILE* sourceStream = preTaken.release();
        if (!sourceStream) {
            return false;
        }
""",
        """        if (std::FILE* revertUnused = preTaken.release()) {
            std::fclose(revertUnused);
        }
        std::error_code revertSrcEc;
        std::FILE* sourceStream =
            pdfe::core::openDenyWriteReadStream(savedPath, revertSrcEc);
        if (!sourceStream) {
            return false;
        }
""",
    ),
]


# ---------------------------------------------------------------------------
# Site #6 - ImageExporter::writeImageAtomic. Back to rendering into
# "<finalPath>.part" BY NAME and publishing it by path.
# ---------------------------------------------------------------------------
SITE6_REVERT = [
    (
        '    std::error_code ec;\n    auto staged = pdfe::core::StagingFile::createBeside(finalPath, ec);\n    if (!staged) {\n        err = "could not create the staging file for " + finalPath.string()\n            + " (original left intact): " + ec.message();\n        return false;\n    }\n\n    // Scoped so the duplicate is CLOSED before the publish: a handle this\n    // process still holds on the source blocks a replace-existing rename, and\n    // that self-inflicted lock has cost this programme three separate\n    // debugging sessions already.\n    {\n        std::FILE* fp = staged->openDuplicateStream(ec);\n        if (!fp) {\n            err = "could not open the staging stream for " + finalPath.string()\n                + " (original left intact): " + ec.message();\n            return false;\n        }\n        QFile out;\n        // DontCloseHandle: `fp` is OURS. QFile must not close a handle whose\n        // lifetime the StagingFile contract owns.\n        if (!out.open(fp, QIODevice::WriteOnly, QFileDevice::DontCloseHandle)) {\n            std::fclose(fp);\n            err = "could not adopt the staging stream for " + finalPath.string();\n            return false;\n        }\n        const bool saved = img.save(&out, qtFormatTag(fmt),\n                                    fmt == ImageFormat::Jpeg ? jpegQuality : -1);\n        // Flush THROUGH Qt and then through the CRT, in that order: QFile\n        // buffers, and a commit taken over half-written bytes publishes a\n        // truncated image that is not a failure anywhere.\n        out.flush();\n        out.close();\n        const bool flushed = (std::fflush(fp) == 0);\n        std::fclose(fp);\n        if (!saved) {\n            err = "QImage::save failed for the staged export of "\n                + finalPath.string();\n            return false;\n        }\n        if (!flushed) {\n            err = "could not flush the staged export of " + finalPath.string();\n            return false;\n        }\n    }\n\n    {\n        std::error_code flushEc;\n        staged->flushToStableStorage(flushEc);  // best-effort\n    }\n\n    // F10 / WIN-85 (data loss), still in force: the previous fallback here was\n    // remove(finalPath) + rename(temp, finalPath) — the exact remove-first\n    // anti-pattern R2-04 removed from Document::save(). If the second rename\n    // then failed (A/V still holding the temp, target directory ACL, media\n    // gone) the user\'s EXISTING exported image had already been deleted and the\n    // replacement never arrived: silent data loss on an overwrite. Nothing is\n    // removed first here either; on failure finalPath is completely untouched\n    // and the StagingFile\'s destructor removes the unpublished artifact.\n    std::error_code renameEc;\n    if (!staged->commitOverWithRetry(finalPath, renameEc)) {\n        err = "rename failed: " + renameEc.message();\n        return false;\n    }\n    return true;\n',
        '    const auto tempPath = finalPath.string() + ".part";\n    const bool saved = img.save(QString::fromStdString(tempPath),\n                                qtFormatTag(fmt),\n                                fmt == ImageFormat::Jpeg ? jpegQuality : -1);\n    if (!saved) {\n        err = "QImage::save failed for " + tempPath;\n        std::error_code rmec0;\n        std::filesystem::remove(tempPath, rmec0);\n        return false;\n    }\n    std::error_code ec;\n    if (!revertAtomicReplaceWithRetry(tempPath, finalPath, ec)) {\n        err = "rename failed: " + ec.message();\n        std::error_code rmec;\n        std::filesystem::remove(tempPath, rmec);\n        return false;\n    }\n    return true;\n',
    ),
]

# ---------------------------------------------------------------------------
# Site #7 - BatchEngine::processFile. Back to the computable
# "temp_<n>_<inputfilename>" intermediates. The FINAL publish is left on the
# handle-based form deliberately: this target isolates the INTERMEDIATE-name
# window, so a red is attributable to it and not to the publish.
# ---------------------------------------------------------------------------
SITE7_REVERT = [
    (
        '\n        // WIN-87 site #7, phase-58: UNPREDICTABLE intermediate names.\n        //\n        // This used to be "temp_<n>_<inputfilename>" in a directory whose name\n        // is also derived from the input ("<dir>/batch_output"), so every\n        // intermediate in the chain had a name an attacker who knows the input\n        // could compute exactly. Each action READS the previous intermediate BY\n        // NAME, so substituting one has the next action parse attacker bytes and\n        // the chain publish them as the batch output.\n        //\n        // These cannot be StagingFile handles: each action opens its OUTPUT by\n        // name (Document::saveAs, QPDF), so an exclusive handle would lock the\n        // producer out of the file it is meant to write. The achievable property\n        // here is UNPREDICTABILITY, not exclusivity — 128 bits of CSPRNG the\n        // attacker cannot aim at — and that difference is stated on\n        // unpredictableSiblingPath rather than glossed.\n        std::error_code tmpEc;\n        auto tempOutput = pdfe::core::StagingFile::unpredictableSiblingPath(\n            outputDir / file.filename(), tmpEc);\n        if (tempOutput.empty()) {\n            result.errorMessage =\n                "Could not create an unpredictable name for the batch "\n                "intermediate (nothing written): " + tmpEc.message();\n            if (currentInput != file) {\n                std::error_code rmec;\n                std::filesystem::remove(currentInput, rmec);\n            }\n            return result;\n        }\n',
        '        auto tempOutput = outputDir / ("temp_" + std::to_string(result.actionsCompleted) + "_" + file.filename().string());\n',
    ),
]

# ---------------------------------------------------------------------------
# Sites #8 and #9 - the two COMPLIANCE CONVERTERS (phase-59).
#
# Each revert restores the pre-phase-59 shape IN FULL, and that shape had TWO
# defects, not one:
#   * the three-step by-path relay - MuPDF wrote "<outputPath>.pdfe_pdfa_tmp"
#     (resp. ".pdfe_pdfua_tmp") BY NAME, nothing held it, QPDF REOPENED IT BY
#     NAME. That is the WIN-87 substitution class, site #3's exact shape.
#   * QPDFWriter then wrote DIRECTLY over outputPath, so there was no atomic
#     publish at any point - the R2-04 data-loss shape.
# No revert shim is needed here: neither converter ever called the deleted
# path-based helper. They did not publish atomically at all.
# ---------------------------------------------------------------------------
SITE8_REVERT = [
    (
        '        return result;\n    }\n\n    try {\n',
        '        return result;\n    }\n\n    auto tempPath = outputPath;\n    tempPath += ".pdfe_pdfa_tmp";\n\n    try {\n',
    ),
    (
        '        // 1g/2. Bake and structurally rewrite through EXCLUSIVE HANDLES —\n        // WIN-87 phase-59.\n        //\n        // This used to be a three-step by-path relay: MuPDF wrote\n        // "<outputPath>.pdfe_pdfa_tmp" BY NAME, nothing held it, QPDF REOPENED\n        // IT BY NAME, and then QPDFWriter wrote DIRECTLY over outputPath. Two\n        // separate defects. An attacker who knows where you are converting knows\n        // that temp\'s name and can have PDFE republish their bytes as the\n        // "PDF/A" output — the WIN-87 substitution class, site #3\'s exact shape.\n        // And there was NO atomic publish at all, so a failure part-way through\n        // the QPDF write left the user\'s output TRUNCATED: the R2-04 data-loss\n        // shape, which every other publish path in this product had already had\n        // removed.\n        //\n        // Both close together. The bake goes into a StagingFile behind an\n        // exclusive handle at an unpredictable name, QPDF reads a DUPLICATE of\n        // that handle and writes into a second one, and only that output is\n        // published — by renaming through its handle. outputPath is never\n        // written in place and never removed first.\n        doc->saveAsThroughExternalRewrite(\n            outputPath,\n            [level](std::FILE* in, std::FILE* out) {\n                // Ownership of BOTH streams is ours on every path; a stream\n                // still open in THIS process blocks the publishing rename.\n                struct OutGuard {\n                    std::FILE* f;\n                    ~OutGuard() { if (f) std::fclose(f); }\n                } outGuard{out};\n\n                core::QpdfWrapper qpdf;\n                qpdf.openFromStream(in, "<staged PDF/A bake handle>");\n                std::FILE* owned = outGuard.f;\n                outGuard.f = nullptr;\n                qpdf.savePdfAToStream(owned, !levelAllowsObjStreams(level));\n            });\n',
        '        // 1g. Save to temp file with MuPDF\n        impl_->context_->execute([doc, &tempPath, level](fz_context* ctx) {\n            pdf_document* pdfDoc = pdf_document_from_fz_document(ctx, doc->fzDocument());\n            if (!pdfDoc) {\n                fz_throw(ctx, FZ_ERROR_ARGUMENT, "Not a PDF document");\n            }\n\n            std::string pathStr = tempPath.string();\n            pdf_write_options opts{};\n            // Tier 2 (pipeline b2e47d91): font substitution during conversion\n            // embeds fonts (PdfFontEmbedder → markNeedsFullGc), so this resolves\n            // to 3 when fonts were embedded; otherwise the cheap default.\n            opts.do_garbage = doc->effectiveGarbageLevel();\n            opts.do_compress = 1;\n            // Out of scope for pipeline 3f8a1c72: a compliance converter legitimately\n            // rewrites streams for PDF/A, and the MuPDF /Filter guard still prevents\n            // any lossy image re-encode or font re-subset of already-compressed data.\n            opts.do_compress_images = 1;\n            opts.do_compress_fonts = 1;\n            opts.do_encrypt = PDF_ENCRYPT_NONE;\n            opts.do_preserve_metadata = 1;\n\n            // For PDF/A-1, disable object streams\n            if (!levelAllowsObjStreams(level)) {\n                opts.do_use_objstms = 0;\n            }\n\n            pdf_save_document(ctx, pdfDoc, pathStr.c_str(), &opts);\n        });\n\n        // ================================================================\n        // Stage 2: QPDF structural operations\n        // ================================================================\n        {\n            core::QpdfWrapper qpdf;\n            qpdf.open(tempPath);\n\n            // QPDFWriter stores the filename pointer; the std::string\n            // returned by .string() must outlive the writer or .write()\n            // dereferences a freed buffer (caught by ASAN).\n            const std::string outputPathStr = outputPath.string();\n            QPDFWriter writer(qpdf.qpdf(), outputPathStr.c_str());\n\n            // Remove encryption from output\n            writer.setPreserveEncryption(false);\n\n            // For PDF/A-1: disable object streams; for 2+: enable them\n            if (!levelAllowsObjStreams(level)) {\n                writer.setObjectStreamMode(qpdf_o_disable);\n            } else {\n                writer.setObjectStreamMode(qpdf_o_generate);\n            }\n\n            writer.setLinearization(false);\n            writer.write();\n        }\n\n        // Clean up temp file\n        std::filesystem::remove(tempPath);\n',
    ),
    (
        '    } catch (const std::exception& e) {\n        // Nothing to clean up: both staging files are owned by\n        // saveAsThroughExternalRewrite and die with it, and outputPath was\n        // never written in place.\n        result.success = false;\n        result.errorMessage = std::string("Conversion failed: ") + e.what();\n',
        '    } catch (const std::exception& e) {\n        std::error_code ec;\n        std::filesystem::remove(tempPath, ec);\n        result.success = false;\n        result.errorMessage = std::string("Conversion failed: ") + e.what();\n',
    ),
]

SITE9_REVERT = [
    (
        '        return result;\n    }\n\n    try {\n',
        '        return result;\n    }\n\n    auto tempPath = outputPath;\n    tempPath += ".pdfe_pdfua_tmp";\n\n    try {\n',
    ),
    (
        '        // 7/8. Bake and structurally rewrite through EXCLUSIVE HANDLES —\n        // WIN-87 phase-59. Identical defect and identical fix to\n        // PdfAConverter::convert; see the long comment there. In short: MuPDF\n        // wrote "<outputPath>.pdfe_pdfua_tmp" BY NAME, QPDF REOPENED IT BY\n        // NAME, and QPDFWriter then wrote DIRECTLY over outputPath — the WIN-87\n        // substitution class plus an R2-04 data-loss window, in one sequence.\n        //\n        // PDF/UA always generates object streams (there is no PDF/A-1-style\n        // level that forbids them), so disableObjectStreams is false here.\n        doc->saveAsThroughExternalRewrite(\n            outputPath,\n            [](std::FILE* in, std::FILE* out) {\n                struct OutGuard {\n                    std::FILE* f;\n                    ~OutGuard() { if (f) std::fclose(f); }\n                } outGuard{out};\n\n                core::QpdfWrapper qpdf;\n                qpdf.openFromStream(in, "<staged PDF/UA bake handle>");\n                std::FILE* owned = outGuard.f;\n                outGuard.f = nullptr;\n                qpdf.savePdfAToStream(owned, /*disableObjectStreams=*/false);\n            });\n',
        '        // 7. Save to temp via MuPDF\n        impl_->context_->execute([doc, &tempPath](fz_context* ctx) {\n            pdf_document* pdfDoc = pdf_document_from_fz_document(ctx, doc->fzDocument());\n            if (!pdfDoc) {\n                fz_throw(ctx, FZ_ERROR_ARGUMENT, "Not a PDF document");\n            }\n\n            std::string pathStr = tempPath.string();\n            pdf_write_options opts{};\n            // Tier 2 (pipeline b2e47d91): PDF/UA tagging is additive (structure\n            // tree), so the cheap default GC suffices unless a dedup-worthy op\n            // (e.g. font embed) ran this session.\n            opts.do_garbage = doc->effectiveGarbageLevel();\n            opts.do_compress = 1;\n            // Out of scope for pipeline 3f8a1c72: PDF/UA conversion legitimately\n            // rewrites for compliance, and the MuPDF /Filter guard still prevents any\n            // lossy image re-encode or font re-subset of already-compressed data.\n            opts.do_compress_images = 1;\n            opts.do_compress_fonts = 1;\n            opts.do_encrypt = PDF_ENCRYPT_NONE;\n            opts.do_preserve_metadata = 1;\n\n            pdf_save_document(ctx, pdfDoc, pathStr.c_str(), &opts);\n        });\n\n        // 8. QPDF stage (remove encryption, generate object streams)\n        {\n            core::QpdfWrapper qpdf;\n            qpdf.open(tempPath);\n\n            // QPDFWriter stores the filename pointer; the std::string\n            // returned by .string() must outlive the writer or .write()\n            // dereferences a freed buffer (caught by ASAN).\n            const std::string outputPathStr = outputPath.string();\n            QPDFWriter writer(qpdf.qpdf(), outputPathStr.c_str());\n            writer.setPreserveEncryption(false);\n            writer.setObjectStreamMode(qpdf_o_generate);\n            writer.setLinearization(false);\n            writer.write();\n        }\n\n        // Clean up temp\n        std::filesystem::remove(tempPath);\n',
    ),
    (
        '    } catch (const std::exception& e) {\n        // Nothing to clean up: the staging files die with the save.\n        result.success = false;\n        result.errorMessage = std::string("PDF/UA conversion failed: ") + e.what();\n',
        '    } catch (const std::exception& e) {\n        std::error_code ec;\n        std::filesystem::remove(tempPath, ec);\n        result.success = false;\n        result.errorMessage = std::string("PDF/UA conversion failed: ") + e.what();\n',
    ),
]


SITES = {
    "site3": {
        "name": "RedactionManager::apply (.redact_tmp -> .redact_out)",
        "src": REDACTION_SRC,
        "obj": REDACTION_OBJ,
        "revert": SITE3_REVERT,
        "cases": [
            "RedactionPublishTocttou."
            "PoisoningTheBakeIntermediateCannotSubstituteThePublishedRedaction",
            "RedactionPublishTocttou."
            "PoisoningAnyStagedArtifactCannotSubstituteThePublishedRedaction",
        ],
        "non_witnesses": {},
    },
    "site4": {
        "name": "MainWindow QPDF redaction rewrite (source + .redact_out)",
        "src": MAINWINDOW_SRC,
        "obj": MAINWINDOW_OBJ,
        "revert": SITE4_REVERT,
        "cases": [
            "MainWindowRedactionRewriteTocttou."
            "PoisoningTheSourceAcrossTheDeliberateCloseCannotRepublishAttackerBytes",
            # The artifact criterion 3 actually names — the OUTPUT. Site #5 had
            # both a staging-side and a source-side case from the start; site #4
            # had only the source-side one, and that asymmetry is what the
            # phase-4 validator gate caught.
            "MainWindowRedactionRewriteTocttou."
            "PoisoningTheStagedRewriteBeforeItIsPublishedCannotSubstituteTheRedaction",
        ],
        "non_witnesses": {},
    },
    "site5": {
        "name": "MainWindow::flushAlignmentPipelineIfPending (.align.tmp)",
        "src": MAINWINDOW_SRC,
        "obj": MAINWINDOW_OBJ,
        "revert": SITE5_REVERT,
        # THE FREE-RUNNING CASE IS DELIBERATELY NOT LISTED HERE, and the reason
        # is a measurement, not a preference:
        # `PoisoningTheFlushStagingAcrossTheClosedDocumentBracket...` does not
        # TERMINATE against the reverted build — killed at 600 s on 2026-08-11,
        # after an earlier run of the same case took the whole harness down at
        # 5400 s, where the fixed build passes in about a second. A hang is an
        # absence of measurement, so listing it here would spend ten minutes to
        # learn nothing and would re-import the wedge that already destroyed two
        # completed sites' results. It remains in the suite, where it passes and
        # asserts the counter properties the deterministic case cannot.
        #
        # The hang itself is UNDIAGNOSED and is a phase-5 obligation: the shape
        # that does not terminate publishes via `atomicReplaceFileWithRetry`,
        # which sites #6/#7 still use, and a case that can hang wedges a CI
        # shard (the F70 shape).
        "cases": [
            "AlignmentFlushTocttou."
            "PoisoningTheStagingArtifactAtTheDeterministicPublishInstantCannotSubstituteTheSave",
        ],
        "non_witnesses": {},
    },
    "site5src": {
        "name": "MainWindow save -> flush INPUT pin (pin taken before closeDocument)",
        "src": MAINWINDOW_SRC,
        "obj": MAINWINDOW_OBJ,
        "revert": SITE5SRC_REVERT,
        "cases": [
            "AlignmentFlushTocttou."
            "PoisoningTheSOURCEAcrossTheClosedDocumentBracketCannotSubstituteTheSave",
        ],
        "non_witnesses": {},
    },
    "site6": {
        "name": "ImageExporter::writeImageAtomic (<final>.part)",
        "src": IMAGEEXPORTER_SRC,
        "obj": IMAGEEXPORTER_OBJ,
        "revert": SITE6_REVERT,
        "cases": [
            "ImageExportPublishTocttou."
            "PoisoningAnyStagedExportArtifactCannotSubstituteThePublishedImage",
        ],
        "non_witnesses": {},
    },
    "site7": {
        "name": "BatchEngine::processFile (temp_<n>_<name> intermediates)",
        "src": BATCHENGINE_SRC,
        "obj": BATCHENGINE_OBJ,
        "revert": SITE7_REVERT,
        "cases": [
            "BatchPublishTocttou."
            "PoisoningAnyBatchIntermediateCannotSubstituteThePublishedOutput",
        ],
        "non_witnesses": {},
    },
    "site8": {
        "name": "PdfAConverter::convertAndSave (.pdfe_pdfa_tmp relay + direct write over outputPath)",
        "src": PDFA_SRC,
        "obj": PDFA_OBJ,
        "revert": SITE8_REVERT,
        "cases": [
            "ComplianceConverterPublishTocttou."
            "PoisoningAnyStagedPdfAArtifactCannotSubstituteTheConvertedDocument",
        ],
        "non_witnesses": {},
    },
    "site9": {
        "name": "PdfUAConverter::convertAndSave (.pdfe_pdfua_tmp relay + direct write over outputPath)",
        "src": PDFUA_SRC,
        "obj": PDFUA_OBJ,
        "revert": SITE9_REVERT,
        "cases": [
            "ComplianceConverterPublishTocttou."
            "PoisoningAnyStagedPdfUAArtifactCannotSubstituteTheConvertedDocument",
        ],
        "non_witnesses": {},
    },
}


CASE_TIMEOUT = 600   # seconds; a hostile case that exceeds this has hung


def run(cmd, log_path=None, timeout=5400):
    proc = subprocess.run(cmd, cwd=str(REPO), capture_output=True, text=True,
                          encoding="utf-8", errors="replace", timeout=timeout)
    out = (proc.stdout or "") + (proc.stderr or "")
    if log_path:
        io.open(log_path, "w", encoding="utf-8").write(out)
    return proc.returncode, out


def build_and_verify(src, obj, tag, outdir):
    """Build pdfe_tests and PROVE `src` was actually recompiled."""
    before = obj.stat().st_mtime if obj.exists() else 0.0
    now = time.time()
    os.utime(src, (now, now))
    log = outdir / f"build_{tag}.log"
    rc, out = run([str(CMAKE), "--build", str(BUILD), "--config", "Debug",
                   "--target", "pdfe_tests"], log)
    if rc != 0:
        sys.exit(f"BUILD FAILED ({tag}); see {log}")
    after = obj.stat().st_mtime if obj.exists() else 0.0
    compiled = src.name in out
    if not (after > before and compiled):
        sys.exit(
            f"ABORT: the {tag} build did not recompile {src.name} "
            f"(obj mtime {before} -> {after}, compile line seen: {compiled}). "
            "Measuring now would repeat the F69 false finding."
        )
    return {"source": str(src), "obj": str(obj), "obj_mtime_before": before,
            "obj_mtime_after": after, "compile_line_seen": compiled,
            "log": str(log)}


def run_cases(cases, tag, outdir, case_timeout=CASE_TIMEOUT):
    xml = outdir / f"gtest_{tag}.xml"
    # A HANG IS A RESULT, NOT A CRASH. The original code let
    # subprocess.TimeoutExpired escape, which killed the whole harness with a
    # traceback and discarded every site measured so far -- observed on
    # 2026-08-11, when site 5's reverted case ran 5400 s and took the run with
    # it. A hostile case that has not finished in minutes has hung, and that is
    # itself a finding worth recording (a publish path that never terminates
    # under a hostile filesystem is a CI-wedging defect in its own right).
    try:
        rc, _ = run([str(EXE), "--gtest_filter=" + ":".join(cases),
                     f"--gtest_output=xml:{xml}"], outdir / f"tests_{tag}.log",
                    timeout=case_timeout)
    except subprocess.TimeoutExpired:
        return {"exit_code": None, "xml": None, "timed_out_seconds": case_timeout,
                "per_case": {c: "HUNG" for c in cases},
                "tests": 0, "failures": 0, "errors": 0}
    root = ET.parse(str(xml)).getroot()
    per_case = {}
    for case in root.iter("testcase"):
        name = f"{case.get('classname')}.{case.get('name')}"
        if case.get("status") == "notrun" or case.get("result") == "skipped":
            per_case[name] = "SKIPPED"
        elif case.find("failure") is not None:
            per_case[name] = "FAIL"
        else:
            per_case[name] = "PASS"
    missing = [c for c in cases if c not in per_case]
    if missing:
        sys.exit(f"ABORT: {tag} run did not execute {missing} — a filter typo "
                 "would otherwise be reported as a clean result.")
    skipped = [c for c, v in per_case.items() if v == "SKIPPED"]
    if skipped:
        sys.exit(f"ABORT: {tag} run SKIPPED {skipped}. A skipped case measures "
                 "nothing; fix the fixture precondition first.")
    return {"exit_code": rc, "xml": str(xml), "per_case": per_case,
            "tests": int(root.get("tests", 0)),
            "failures": int(root.get("failures", 0)),
            "errors": int(root.get("errors", 0))}


def inject_shim(text, crlf):
    """Put REVERT_SHIM after the file's last #include, if the revert used it.

    Injected rather than carried in each hunk's replacement text so the three
    sites cannot drift apart, and so a site that stops needing it does not
    silently keep a dead copy. Returns the text unchanged when no reverted call
    site references the shim.
    """
    if "revertAtomicReplaceWithRetry(" not in text:
        return text
    if "bool revertAtomicReplaceWithRetry(" in text:
        return text                      # already injected
    eol = "\r\n" if crlf else "\n"
    shim = REVERT_SHIM.replace("\n", eol) if crlf else REVERT_SHIM
    lines = text.split(eol)
    last = max(i for i, l in enumerate(lines) if l.startswith("#include"))
    lines.insert(last + 1, shim)
    return eol.join(lines)


def patch(text, hunks, crlf):
    for find, replace in hunks:
        a, b = find, replace
        if crlf:
            a = a.replace("\n", "\r\n")
            b = b.replace("\n", "\r\n")
        n = text.count(a)
        if n != 1:
            sys.exit(f"ABORT: revert hunk matched {n} times, expected 1:\n"
                     f"{a[:160]}")
        text = text.replace(a, b)
    return text


def do_site(key, outdir, case_timeout=CASE_TIMEOUT):
    site = SITES[key]
    src, obj = site["src"], site["obj"]
    original = io.open(src, encoding="utf-8", newline="").read()
    io.open(outdir / f"{key}_{src.name}.orig", "w", encoding="utf-8",
            newline="").write(original)
    # core.autocrlf=true, so the working-tree file is CRLF while the hunks here
    # are LF. Normalise the HUNKS to the file, never the file to the hunks: a
    # whole-file line-ending rewrite would show up as a thousand-line diff if a
    # restore ever failed.
    crlf = "\r\n" in original

    result = {"site": key, "description": site["name"], "cases": site["cases"]}
    try:
        io.open(src, "w", encoding="utf-8", newline="").write(
            inject_shim(patch(original, site["revert"], crlf), crlf))
        result["reverted_build"] = build_and_verify(src, obj, f"{key}_reverted",
                                                    outdir)
        result["reverted_tests"] = run_cases(site["cases"], f"{key}_reverted",
                                             outdir, case_timeout)
    finally:
        # RESTORE THE SOURCE **AND THE BINARY**. Restoring only the source was a
        # real hazard, observed on 2026-08-11: this harness died mid-site, left
        # the tree correct and `pdfe_tests.exe` still built from the REVERTED,
        # vulnerable code, and everything downstream (a suite run, a gate) would
        # have measured code that is not in the tree. That is the F69
        # stale-binary species arriving through a crashed harness rather than a
        # skipped compile. Failure mode "correct source, wrong binary" is worse
        # than a clean crash, because nothing downstream looks wrong.
        io.open(src, "w", encoding="utf-8", newline="").write(original)
        try:
            result["restored_build"] = build_and_verify(
                src, obj, f"{key}_restored", outdir)
        except BaseException as e:      # noqa: BLE001 - must not mask the original
            result["restored_build"] = {"error": f"REBUILD FAILED: {e}"}

    result["restored_tests"] = run_cases(site["cases"], f"{key}_restored",
                                         outdir, case_timeout)

    rev = result["reverted_tests"]["per_case"]
    res = result["restored_tests"]["per_case"]
    verdict = {}
    for c in site["cases"]:
        if rev[c] == "HUNG" or res[c] == "HUNG":
            # Not a verdict about the fix: the case never terminated, so it
            # measured nothing. Recorded loudly rather than counted either way.
            verdict[c] = ("HUNG — the case did not terminate within the timeout; "
                          "discrimination is UNMEASURED, not established")
        elif res[c] != "PASS":
            verdict[c] = "BROKEN — fails against the FIXED code"
        elif rev[c] == "FAIL":
            verdict[c] = "DISCRIMINATING — red when the site is reverted, green fixed"
        else:
            verdict[c] = ("NOT A WITNESS FOR THIS SITE — "
                          + site["non_witnesses"].get(c, "UNEXPLAINED; investigate"))
    result["verdict"] = verdict
    result["discriminating_count"] = sum(
        1 for v in verdict.values() if v.startswith("DISCRIMINATING"))
    result["all_green_when_fixed"] = all(v == "PASS" for v in res.values())
    result["unexplained_non_witnesses"] = [
        c for c, v in verdict.items() if "UNEXPLAINED" in v]
    return result


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--outdir", default=str(REPO / "build" / "p57_revert"))
    ap.add_argument("--sites", default="site3,site4,site5")
    ap.add_argument("--case-timeout", type=int, default=CASE_TIMEOUT,
                    help="seconds before a hostile case is recorded as HUNG")
    args = ap.parse_args()
    outdir = Path(args.outdir)
    outdir.mkdir(parents=True, exist_ok=True)

    report = {"sites": {}}
    for key in [s.strip() for s in args.sites.split(",") if s.strip()]:
        report["sites"][key] = do_site(key, outdir, args.case_timeout)
        io.open(outdir / "phase57_revert_and_observe.json", "w",
                encoding="utf-8").write(json.dumps(report, indent=2))

    report["all_sites_discriminating"] = all(
        r["all_green_when_fixed"] and not r["unexplained_non_witnesses"]
        and r["discriminating_count"] == len(r["cases"])
        for r in report["sites"].values())
    io.open(outdir / "phase57_revert_and_observe.json", "w",
            encoding="utf-8").write(json.dumps(report, indent=2))
    print(json.dumps(report, indent=2))

    for key, r in report["sites"].items():
        if not r["all_green_when_fixed"]:
            sys.exit(f"{key}: a case fails against the FIXED code — fix that first.")
        if r["unexplained_non_witnesses"]:
            sys.exit(f"{key}: a case failed to discriminate with no recorded reason.")


if __name__ == "__main__":
    os.environ.setdefault("PYTHONIOENCODING", "utf-8")
    main()
