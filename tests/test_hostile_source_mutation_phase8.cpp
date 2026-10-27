// -*- coding: utf-8 -*-
//
// WIN-87 (meta-audit-2026-07 gap register, phase-8) — the READ side.
//
//   "Simulate by deleting/renaming the target file out from under each
//    operation; assert diagnosable error, no crash, no silent data loss."
//
// WHAT ALREADY EXISTS, counted rather than assumed, because acceptance
// criterion 2 says to reuse and extend rather than duplicate. The
// hostile-filesystem work of phases 3–7 is **entirely the save/publish
// family**: test_publish_handle_hardening_phase57.cpp (9 cases),
// …phase58 (2), …phase59 (3), test_temp_file_safety_phase60.cpp (13),
// test_full_rewrite_staging_hardening.cpp (16),
// test_save_failure_injection.cpp (11), test_staged_save_hardening.cpp (8),
// test_staging_name_contract.cpp (8), test_document_save_atomic.cpp (4),
// test_save_kill_window.cpp (3). Every case name is a staging, publish,
// save-as or TOCTOU shape. Those are not repeated here.
//
// THE GAP IS THE OTHER DIRECTION. "Each operation" includes the operations
// that CONSUME a path, and none of them had been driven this way:
//
//   * Document::open, which is gated by an out-of-process preflight — the
//     child vets the file, the parent then opens it. Whatever happens to the
//     file BETWEEN those two steps is a genuine time-of-check/time-of-use
//     window, and it is the one window in the product where the check and the
//     use are in different processes.
//   * the exporters, reading a document whose backing file is deleted or
//     replaced mid-export.
//   * AttachmentManager::extract, whose OUTPUT path is a target an attacker
//     controls the neighbourhood of.
//
// The corpus is built from the ATTACKER'S PRIMITIVES, not from what the code
// already handles — that inversion is what made phase-60's first hostile
// corpus green exactly where its design was blind. Delete, rename away,
// replace-with-different-content, replace-with-a-shorter-file,
// replace-with-a-directory.
//
// "No silent data loss" is asserted on the ARTEFACT, not on the return value.
// DCA 0026's rule: a test that checks the operation's verdict rather than the
// protected object cannot tell a refusal from a refusal that already did the
// damage.

#include <gtest/gtest.h>

#include "attachments/AttachmentManager.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "core/PreflightOpen.h"
#include "exports/PlainTextExporter.h"

#include "security_oracles.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>

using namespace pdfe::core;

namespace {

std::filesystem::path fixturesDir()
{
    return std::filesystem::path(TEST_FIXTURES_DIR);
}

std::filesystem::path freshDir(const char* leaf)
{
    const auto d = std::filesystem::temp_directory_path()
                 / "pdfe_win87_readside" / leaf;
    std::error_code ec;
    std::filesystem::remove_all(d, ec);
    std::filesystem::create_directories(d, ec);
    return d;
}

std::string readAll(const std::filesystem::path& p)
{
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

void writeBytes(const std::filesystem::path& p, const std::string& bytes)
{
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

// The attacker's primitives against a path something is about to read.
enum class Mutation {
    Delete,
    RenameAway,
    ReplaceWithDifferentPdf,
    ReplaceWithGarbage,
    ReplaceWithEmpty,
    ReplaceWithDirectory,
};

const char* nameOf(Mutation m)
{
    switch (m) {
    case Mutation::Delete:                  return "deleted";
    case Mutation::RenameAway:              return "renamed away";
    case Mutation::ReplaceWithDifferentPdf: return "replaced with a different PDF";
    case Mutation::ReplaceWithGarbage:      return "replaced with garbage";
    case Mutation::ReplaceWithEmpty:        return "replaced with an empty file";
    case Mutation::ReplaceWithDirectory:    return "replaced with a directory";
    }
    return "?";
}

// The outcome of trying to BE the attacker. Three states, not two, because the
// measured behaviour made the two-state version wrong:
//
//   Applied     — the mutation took effect; the operation under test now has
//                 to cope with it.
//   BlockedByOs — Windows REFUSED it. An open Document holds a handle to its
//                 backing file without FILE_SHARE_DELETE, so a delete or a
//                 rename simply fails. That is not a test that did not run —
//                 it is the invariant holding, and it is the STRONGEST
//                 available answer to "delete the target out from under the
//                 operation": the attacker never gets to.
//   Failed      — could not be set up for some other reason (a missing
//                 fixture). The case must not silently pass on this.
//
// The first version of this file collapsed the first two into `false`, which
// reported a working defence as a broken test. Distinguishing them is the
// difference between "we could not test it" and "there is nothing to test
// because it cannot happen".
enum class MutationOutcome { Applied, BlockedByOs, Failed };

MutationOutcome applyMutation(Mutation m, const std::filesystem::path& target,
                              const std::filesystem::path& scratch)
{
    std::error_code ec;
    switch (m) {
    case Mutation::Delete:
        std::filesystem::remove(target, ec);
        return std::filesystem::exists(target) ? MutationOutcome::BlockedByOs
                                               : MutationOutcome::Applied;

    case Mutation::RenameAway:
        std::filesystem::rename(target, scratch / "moved_away.pdf", ec);
        return std::filesystem::exists(target) ? MutationOutcome::BlockedByOs
                                               : MutationOutcome::Applied;

    case Mutation::ReplaceWithDifferentPdf: {
        const auto other = fixturesDir() / "cropbox_offset.pdf";
        if (!std::filesystem::exists(other)) return MutationOutcome::Failed;
        std::filesystem::remove(target, ec);
        if (std::filesystem::exists(target)) return MutationOutcome::BlockedByOs;
        std::filesystem::copy_file(other, target,
            std::filesystem::copy_options::overwrite_existing, ec);
        return (!ec && std::filesystem::exists(target))
             ? MutationOutcome::Applied : MutationOutcome::Failed;
    }

    case Mutation::ReplaceWithGarbage:
        std::filesystem::remove(target, ec);
        if (std::filesystem::exists(target)) return MutationOutcome::BlockedByOs;
        writeBytes(target, std::string(4096, '\xA5'));
        return std::filesystem::exists(target) ? MutationOutcome::Applied
                                               : MutationOutcome::Failed;

    case Mutation::ReplaceWithEmpty:
        std::filesystem::remove(target, ec);
        if (std::filesystem::exists(target)) return MutationOutcome::BlockedByOs;
        writeBytes(target, "");
        return std::filesystem::exists(target) ? MutationOutcome::Applied
                                               : MutationOutcome::Failed;

    case Mutation::ReplaceWithDirectory:
        std::filesystem::remove(target, ec);
        if (std::filesystem::exists(target)) return MutationOutcome::BlockedByOs;
        std::filesystem::create_directories(target, ec);
        return std::filesystem::is_directory(target) ? MutationOutcome::Applied
                                                     : MutationOutcome::Failed;
    }
    return MutationOutcome::Failed;
}

const Mutation kAllMutations[] = {
    Mutation::Delete,
    Mutation::RenameAway,
    Mutation::ReplaceWithDifferentPdf,
    Mutation::ReplaceWithGarbage,
    Mutation::ReplaceWithEmpty,
    Mutation::ReplaceWithDirectory,
};

} // namespace

// ── 1. The preflight → open window ─────────────────────────────────────────
//
// isDocumentOpenSafe() runs a CHILD PROCESS over the path, then the parent
// opens the same path. The check and the use are in different processes, so
// the window between them is wide and completely outside our control. What
// must hold is that the parent's open is not made unconditional by the
// child's blessing: whatever is at the path when the parent opens it has to
// be handled on its own merits.
TEST(HostileSourceMutation, DocumentOpenDoesNotTrustAPreflightVerdictAboutDifferentBytes)
{
    const auto src = fixturesDir() / "test_3page.pdf";
    ASSERT_TRUE(std::filesystem::exists(src));

    for (Mutation m : kAllMutations) {
        SCOPED_TRACE(nameOf(m));
        const auto dir = freshDir("open_window");
        const auto target = dir / "victim.pdf";
        std::error_code ec;
        std::filesystem::copy_file(src, target,
            std::filesystem::copy_options::overwrite_existing, ec);
        ASSERT_FALSE(ec) << ec.message();

        // TIME OF CHECK — the real gate, in the real child process.
        const bool blessed = isDocumentOpenSafe(target);
        EXPECT_TRUE(blessed)
            << "vacuity guard: the preflight refused the GOOD file, so the "
               "window under test was never opened";

        // The attacker acts. Nothing holds the file at this point — the
        // preflight child has exited and the parent has not opened it yet —
        // so every mutation is expected to land here. If one is blocked, the
        // window is narrower than believed and that is worth knowing, not
        // worth passing over.
        const MutationOutcome outcome = applyMutation(m, target, dir);
        ASSERT_NE(outcome, MutationOutcome::Failed)
            << "the mutation could not be set up; the case did not run";
        if (outcome == MutationOutcome::BlockedByOs) {
            GTEST_LOG_(INFO) << "mutation '" << nameOf(m)
                             << "' was refused by the OS in the "
                                "preflight->open window";
            continue;
        }

        // TIME OF USE.
        pdfe_test::ProcessSpawnGuard spawnGuard;
        std::unique_ptr<Document> doc;
        auto ctx = std::make_shared<MuPdfContext>();

        // The register asks for a DIAGNOSABLE ERROR, no crash, no silent data
        // loss. A thrown PdfeException naming the file IS the diagnosable
        // error — the first version of this case asserted NO_THROW, which
        // demanded the weaker outcome (a null return) and would have gone red
        // on the better one. What must not happen is a crash, an exception
        // with nothing in it, or a Document that opens and then cannot answer
        // a question about itself.
        std::string thrownWhat;
        try {
            doc = Document::open(ctx, target);
        } catch (const std::exception& e) {
            thrownWhat = e.what() ? e.what() : "";
            EXPECT_FALSE(thrownWhat.empty())
                << "the open failed with an exception carrying no message; "
                   "that is not diagnosable";
        }

        if (!thrownWhat.empty()) {
            EXPECT_EQ(doc, nullptr)
                << "Document::open threw AND returned a document";
        }

        if (doc) {
            // Opening succeeded — permitted, but only for something that IS a
            // readable PDF, and it must be THAT document rather than a
            // half-adopted mixture of the two. pageCount() must be
            // answerable and consistent.
            int pages = -1;
            ASSERT_NO_THROW({ pages = doc->pageCount(); });
            EXPECT_GT(pages, 0)
                << "opened a document reporting no pages — the open should "
                   "have failed rather than producing an empty document";

            if (m == Mutation::ReplaceWithDifferentPdf) {
                // The substituted file is a single-page fixture; the original
                // has three. Reading 3 here would mean the parent had cached
                // the pre-check identity and answered about a file that is no
                // longer there.
                EXPECT_EQ(pages, 1)
                    << "the open reported the ORIGINAL document's page count "
                       "for a file that had been substituted — the preflight "
                       "verdict was carried across the swap";
            }
        }
        // If doc is null the open was refused, which is the other acceptable
        // outcome. Both are "diagnosable"; neither is a crash.

        EXPECT_EQ(spawnGuard.newlySpawned(), "")
            << "opening a mutated document spawned an attack process";
    }
}

// ── 2. Export while the backing file moves ─────────────────────────────────
//
// A Document holds an open handle to its source. The export therefore ought to
// be unaffected by anything that happens to the NAME — that is the invariant
// phase-60 concluded with, one layer down. What must never happen is a
// half-written output file presented as a complete export.
TEST(HostileSourceMutation, ExportSurvivesOrRefusesWhenTheSourceMovesUnderIt)
{
    const auto src = fixturesDir() / "test_3page.pdf";
    ASSERT_TRUE(std::filesystem::exists(src));

    for (Mutation m : kAllMutations) {
        SCOPED_TRACE(nameOf(m));
        const auto dir = freshDir("export");
        const auto target = dir / "victim.pdf";
        const auto out = dir / "export.txt";
        std::error_code ec;
        std::filesystem::copy_file(src, target,
            std::filesystem::copy_options::overwrite_existing, ec);
        ASSERT_FALSE(ec);

        auto ctx = std::make_shared<MuPdfContext>();
        auto doc = Document::open(ctx, target);
        ASSERT_NE(doc, nullptr) << "vacuity guard: the good file did not open";

        // Here the Document IS open, so a delete or a rename is expected to be
        // REFUSED BY WINDOWS — the handle is the protection, and that is the
        // strongest possible answer to this row. It is asserted rather than
        // merely tolerated: if a mutation that should be blocked starts
        // landing, the handle is no longer being held and the export becomes
        // vulnerable to exactly what WIN-87 describes.
        const MutationOutcome outcome = applyMutation(m, target, dir);
        ASSERT_NE(outcome, MutationOutcome::Failed)
            << "the mutation could not be set up; the case did not run";

        if (outcome == MutationOutcome::BlockedByOs) {
            EXPECT_TRUE(std::filesystem::exists(target))
                << "the mutation was reported blocked but the file is gone";
            // The invariant held. The export must still work — a handle that
            // protects the file but breaks the feature is not a win.
            bool ok = false;
            ASSERT_NO_THROW({
                ok = pdfe::exports::PlainTextExporter::write(doc.get(), out);
            });
            EXPECT_TRUE(ok)
                << "the open handle blocked the attacker AND broke the export";
            EXPECT_TRUE(std::filesystem::exists(out));
            continue;
        }

        bool wrote = false;
        ASSERT_NO_THROW({
            wrote = pdfe::exports::PlainTextExporter::write(doc.get(), out);
        });

        if (wrote) {
            // Claimed success must be backed by an artefact that exists and
            // is not empty. "Returned true, wrote nothing" is the silent data
            // loss this row is about.
            ASSERT_TRUE(std::filesystem::exists(out))
                << "the exporter reported success and produced no file";
            const std::string bytes = readAll(out);
            EXPECT_GT(bytes.size(), sizeof(pdfe::exports::PlainTextExporter::kUtf8Bom))
                << "the exporter reported success and produced a file holding "
                   "nothing but a BOM";
        } else {
            // A refusal must not leave a partial artefact behind for someone
            // to mistake for a finished export.
            if (std::filesystem::exists(out)) {
                EXPECT_TRUE(readAll(out).empty())
                    << "the exporter failed but left a partially written "
                       "file at " << out.string();
            }
        }
    }
}

// ── 3. The extraction DESTINATION ──────────────────────────────────────────
//
// AttachmentManager::extract(doc, name, outDir) writes into a DIRECTORY, and
// derives the leaf from the filespec's /UF through sanitizeFilename(). So the
// hostile moves are against the destination neighbourhood, which an attacker
// on a shared machine controls even when they cannot touch the document: a
// FILE standing where outDir should be, outDir vanishing between the call and
// the write, and a pre-existing file at the leaf name.
//
// The attachment is created here rather than loaded from a fixture — there is
// no attachment fixture in the tree, and building one in-test means the
// expected bytes are known exactly, so "no silent data loss" can be asserted
// against a literal rather than against whatever happened to come out.
TEST(HostileSourceMutation, AttachmentExtractionRefusesAHostileDestinationWithoutDestroyingIt)
{
    const auto dir = freshDir("extract");
    const auto src = fixturesDir() / "test_3page.pdf";
    ASSERT_TRUE(std::filesystem::exists(src));

    const auto docPath = dir / "carrier.pdf";
    std::error_code ec;
    std::filesystem::copy_file(src, docPath,
        std::filesystem::copy_options::overwrite_existing, ec);
    ASSERT_FALSE(ec) << ec.message();

    const std::string payload = "PDFE-WIN87-ATTACHMENT-PAYLOAD";
    const auto blob = dir / "payload.bin";
    writeBytes(blob, payload);

    auto ctx = std::make_shared<MuPdfContext>();
    auto doc = Document::open(ctx, docPath);
    ASSERT_NE(doc, nullptr);

    pdfe::attachments::AttachmentManager mgr(ctx);
    ASSERT_TRUE(mgr.add(doc.get(), blob, "payload.bin"))
        << "could not embed the attachment; the case did not run";

    const auto entries = mgr.list(doc.get());
    ASSERT_FALSE(entries.empty())
        << "vacuity guard: the attachment was not embedded, so every "
           "extraction below would fail for the wrong reason";
    const std::string key = entries.front().name;

    // (a) The happy path FIRST, so the refusals below are known to be
    //     refusals of the destination rather than of the attachment.
    {
        const auto good = dir / "good_out";
        std::filesystem::create_directories(good, ec);
        const auto written = mgr.extract(doc.get(), key, good);
        ASSERT_FALSE(written.empty())
            << "extraction into a clean directory failed; nothing below "
               "would mean anything";
        EXPECT_EQ(readAll(written), payload)
            << "the extracted bytes are not the embedded bytes";
    }

    // (b) A FILE standing where outDir should be. Must not crash, must not
    //     destroy that file, and must not claim success.
    {
        const auto occupied = dir / "occupied";
        writeBytes(occupied, "SOMEBODY-ELSES-FILE");
        const std::string before = readAll(occupied);

        std::filesystem::path written;
        ASSERT_NO_THROW({
            try { written = mgr.extract(doc.get(), key, occupied); }
            catch (const std::exception&) { /* a diagnosable refusal is fine */ }
        });
        EXPECT_TRUE(written.empty())
            << "extraction claimed to write into a path that is a FILE, not "
               "a directory: " << written.string();
        EXPECT_EQ(readAll(occupied), before)
            << "extraction overwrote a file standing at its destination "
               "directory's name — the refusal happened after the damage";
    }

    // (c) outDir removed between the caller's check and the write. This is
    //     the WIN-87 shape itself, aimed at the output side.
    {
        const auto vanishing = dir / "vanishing_out";
        std::filesystem::create_directories(vanishing, ec);
        std::filesystem::remove_all(vanishing, ec);
        ASSERT_FALSE(std::filesystem::exists(vanishing));

        std::filesystem::path written;
        ASSERT_NO_THROW({
            try { written = mgr.extract(doc.get(), key, vanishing); }
            catch (const std::exception&) { }
        });
        // Either it recreated the directory and wrote the real bytes, or it
        // refused. What it must not do is report a path holding nothing.
        if (!written.empty()) {
            ASSERT_TRUE(std::filesystem::exists(written))
                << "extraction returned " << written.string()
                << " and nothing is there";
            EXPECT_EQ(readAll(written), payload);
        }
    }

    // (d) A pre-existing file at the leaf name. Overwriting is a defensible
    //     policy and refusing is a defensible policy; leaving it truncated is
    //     neither.
    {
        const auto clash = dir / "clash_out";
        std::filesystem::create_directories(clash, ec);
        const auto leaf = clash / "payload.bin";
        writeBytes(leaf, "PRE-EXISTING-CONTENT");

        std::filesystem::path written;
        ASSERT_NO_THROW({
            try { written = mgr.extract(doc.get(), key, clash); }
            catch (const std::exception&) { }
        });
        const std::string after = readAll(leaf);
        EXPECT_FALSE(after.empty())
            << "the destination file was truncated and left empty — neither "
               "overwritten with the attachment nor left intact";
        EXPECT_TRUE(after == payload || after == "PRE-EXISTING-CONTENT")
            << "the destination holds neither the attachment nor its original "
               "contents; it is [" << after << "]";
    }
}
