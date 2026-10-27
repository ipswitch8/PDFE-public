// -*- coding: utf-8 -*-
// ===========================================================================
// WIN-87 sites #3/#4/#5, phase-57 — the three remaining HIGH-severity publish
// paths: RedactionManager::apply, MainWindow's QPDF redaction rewrite, and
// MainWindow's alignment flush.
//
// WHAT MAKES THESE THREE DIFFERENT FROM SITES #1/#2 (phase-56), AND WHY A TEST
// THAT ONLY ATTACKS THE FINAL RENAME WOULD GO GREEN OVER ALL OF THEM.
//
// Sites #1/#2 had one artifact each: a staging file PDFE wrote and then
// published. These three each have a SECOND poisonable artifact that the
// staging migration does not touch, because the staging primitive protects
// what PDFE WRITES, not what it READS:
//
//   site #3  document->save(".redact_tmp")   <- PDFE writes it
//            ---- window A ----              <- nothing holds it
//            qpdf.open(".redact_tmp")        <- PDFE READS IT BACK BY NAME
//            qpdf.save(".redact_out")
//            ---- window B ----
//            rename(".redact_out" -> doc)
//
//            Both names are GONE as of this phase — the bake and the rewrite
//            output live and die behind StagingFile exclusive handles at
//            unguessable names, and only the output is ever published. That is
//            why the site-#3 cases below attack every sibling by enumeration
//            rather than a literal filename: an attacker aimed at
//            ".redact_tmp" would now attack nothing and report success.
//
//   site #4  closeDocument()                 <- releases ALL locks, on purpose
//            ---- window ----
//            qpdf.open(filePath)             <- re-resolves the SOURCE by name
//            qpdf.save(".redact_out") ; rename
//
//   site #5  runs under the SAME deliberately-closed document, reading the
//            file the user just saved and staging at "<saved>.align.tmp".
//
// Poisoning window A, or site #4's source window, reaches the same publication
// as poisoning the final artifact — and for the two redaction sites the thing
// published IS the redaction, so a silent substitution can serve content the
// user believes they destroyed under a filename they trust as redacted.
//
// SO EVERY TEST HERE ATTACKS THE UPSTREAM ARTIFACT, NOT ONLY THE LAST ONE.
//
// THE ORACLES ARE THE ATTACKER'S OWN INSTRUMENTS. Following phase-56: each
// attacker records BOTH what it observed and what it managed to do, so a green
// result cannot come from "the window never happened", and every destructive
// operation is gated behind a CreateFileW(DELETE, OPEN_EXISTING) probe so a
// rename onto an ALREADY-FREE name cannot be miscounted as a successful
// substitution (the phantom that fooled phase-56's first draft).
//
// AND EVERY DECOY IS A VALID PDF CARRYING A MARKER. A crude prefix-clobber
// makes the file unparseable, which the product then reports as a failure —
// that is not the attack that matters. The attack that matters substitutes a
// WELL-FORMED document, which the product would parse and republish happily.
// The marker is what turns "the published file is fine" into a real oracle.
// ===========================================================================

#include <gtest/gtest.h>

#include "core/AtomicFileReplace.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "redaction/RedactionManager.h"
#include "reflow/AlignmentPipeline.h"
#include "ui/MainWindow.h"

#include <QAction>
#include <QApplication>
#include <QRectF>
#include <QString>
#include <QTemporaryDir>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#ifndef TEST_FIXTURES_DIR
#define TEST_FIXTURES_DIR "."
#endif

namespace {

using pdfe::core::Document;
using pdfe::core::MuPdfContext;
using pdfe::core::StagingFile;

/// The string an attacker plants. If it ever reaches a published document, the
/// substitution succeeded.
constexpr const char* kPayload = "PWNED-WIN87-PHASE57-0123456789ABCDEF";

std::filesystem::path fixtures()
{
    return std::filesystem::path(TEST_FIXTURES_DIR);
}

std::string readAll(const std::filesystem::path& p)
{
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f),
                       std::istreambuf_iterator<char>());
}

std::filesystem::path makeWorkDir(const char* tag)
{
    std::error_code ec;
    auto d = std::filesystem::temp_directory_path()
             / (std::string("pdfe_win87p57_") + tag + "_"
                + std::to_string(
                    std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(d, ec);
    return d;
}

/// THE FIXTURE IS SMALL ON PURPOSE, AND THE OBSERVABILITY ORACLE STILL HOLDS.
///
/// The obvious choice is a multi-megabyte real-world PDF, so the staging
/// artifacts exist long enough for a polling attacker to find them. Measured,
/// that is unusable here: `applyAllRedactions` sets markNeedsFullGc(), so the
/// bake is a garbage-collecting FULL REWRITE, and one hostile run against
/// `realworld/irs_p17.pdf` in a Debug build burned 25 minutes of CPU without
/// finishing — with the spinning attacker thread contending for the same
/// directory, which is exactly what the test is for.
///
/// It is not needed, because the artifact these cases are ABOUT is long-lived
/// regardless of document size: the bake staging file is created before the
/// MuPDF full rewrite and is held open across the whole QPDF rewrite, so it is
/// on disk for the entire operation. That is precisely window A, and precisely
/// what acceptance criterion 2 names. So the `saw > 0` oracle is satisfied by
/// construction rather than by racing.
///
/// WHAT IS HONESTLY WEAKER: the QPDF OUTPUT staging file is short-lived on a
/// small document, so a poll may miss it. Its coverage is carried by the
/// payload-in-the-published-document oracle (which does not depend on catching
/// the file mid-flight) and by revert-and-observe, not by the `saw` count.
std::filesystem::path redactionFixture()
{
    for (const char* rel : {"test_redaction.pdf", "test_3page.pdf",
                            "test_single.pdf"}) {
        const auto p = fixtures() / rel;
        if (std::filesystem::exists(p)) return p;
    }
    return {};
}

/// A VALID PDF carrying kPayload, used as the substitution decoy. It has to
/// parse: a decoy that does not is a corruption test, not a substitution one.
/// Does the PUBLISHED document actually RENDER the decoy's payload?
///
/// WHY THIS EXISTS RATHER THAN A BYTE SCAN. The decoy writes kPayload into an
/// UNCOMPRESSED content stream, and makeValidDecoy's comment asserts the marker
/// therefore "survives as a raw byte sequence for the oracle". That is true of
/// the decoy FILE and false of anything QPDF republishes from it: QPDF
/// re-serialises and Flate-compresses content streams, so the literal bytes are
/// gone from the output even though the decoy's PAGE was published verbatim.
///
/// MEASURED, not theorised: with a deterministic seam substituting the decoy
/// inside the window, the site-#4 case still passed against the REVERTED,
/// vulnerable build — `seam_substituted_the_source=1` in the XML, republication
/// confirmed, and the byte oracle saw nothing. A republication oracle that
/// cannot see a republication is the exact species this programme keeps
/// finding, and it was sitting inside the phase's own acceptance evidence.
///
/// Extracting text decompresses, so this sees the payload however it was
/// re-encoded. Returns false if the file cannot be opened at all — the caller
/// asserts openability separately, and a failure to open is not evidence of
/// republication.
inline bool publishedDocumentRendersPayload(const std::filesystem::path& doc)
{
    try {
        auto ctx = std::make_shared<MuPdfContext>();
        auto d = Document::open(ctx, doc);
        if (!d) return false;
        for (int i = 0; i < d->pageCount(); ++i) {
            auto pg = d->page(i);
            if (!pg) continue;
            for (const auto& b : pg->extractText()) {
                if (b.text().find(kPayload) != std::string::npos) return true;
            }
        }
    } catch (...) {
        return false;
    }
    return false;
}

std::filesystem::path makeValidDecoy(const std::filesystem::path& dir)
{
    const auto p = dir / "decoy_source.pdf";
    std::ofstream f(p, std::ios::binary);
    f << "%PDF-1.4\n"
      << "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n"
      << "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n"
      << "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
         "/Contents 4 0 R >>\nendobj\n"
      << "4 0 obj\n<< /Length 60 >>\nstream\n"
      << "BT /F1 24 Tf 72 700 Td (" << kPayload << ") Tj ET\n"
      << "endstream\nendobj\n"
      << "trailer\n<< /Root 1 0 R /Size 5 >>\n"
      << "%%EOF\n";
    f.close();
    // The marker must also survive as a raw byte sequence for the oracle, which
    // scans bytes rather than extracting text — it is in the content stream
    // above uncompressed, so it does.
    return p;
}

bool looksLikeAPdf(const std::filesystem::path& p)
{
    std::ifstream f(p, std::ios::binary);
    char head[5] = {0};
    f.read(head, 5);
    return f.gcount() == 5 && std::string(head, 5) == "%PDF-";
}

#if defined(_WIN32)

/// The shared hostile instrument. It polls a directory and, for every file that
/// is not the victim document and not its own decoy, tries BOTH halves of the
/// WIN-87 prescribed action:
///
///   * open it for WRITING and stamp kPayload over offset 0 (substitutes a
///     prefix silently), and
///   * take DELETE access and rename the decoy over it (substitutes the WHOLE
///     file — the dangerous one, and the one a content check on the staging
///     file alone would miss).
///
/// The DELETE probe in front of the destructive half is not decoration: a
/// MoveFileExW(REPLACE_EXISTING) onto a name that is already FREE simply
/// creates the file and would be miscounted as a successful substitution. That
/// exact false positive was reported against phase-56's FIXED code before the
/// probe was added. The probe fails when the name is gone and succeeds only
/// when a file is there AND our exclusive handle is not denying DELETE — which
/// is the property under test.
///
/// AND IT ONLY ATTACKS WHILE THE OPERATION IS RUNNING (`inOperation`). Without
/// that gate the site-#4 case has a real false-failure race: its victim IS the
/// user's document, which exists and is unheld again the instant the rewrite
/// finishes, so a rename landing between "the call returned" and "join()" would
/// put the payload into the published file and be reported as a defect that
/// never happened. The window under test is the operation, so the attacker is
/// scoped to the operation — and the gate is checked immediately before each
/// destructive act, not once per sweep.
struct SiblingAttacker {
    std::atomic<bool> stop{false};
    std::atomic<bool> inOperation{false};
    std::atomic<int> saw{0};             ///< sibling artifacts observed at all
    std::atomic<int> openedForWrite{0};  ///< got write access to one
    std::atomic<int> tamperedBytes{0};
    std::atomic<int> openedForDelete{0}; ///< got delete access to one
    std::atomic<int> deleted{0};
    std::atomic<int> renamedOver{0};     ///< FULL-FILE substitution landed
    std::string seenNames;
    /// The same thing machine-readable. A prose blob is fine for a failure
    /// message and useless for an assertion, and once the artifacts stop having
    /// predictable names the assertion has to be ABOUT the set of names.
    std::set<std::string> seenSet;
    /// Exactly WHICH name each successful attack landed on, and which kind.
    /// A count alone cannot distinguish "the staging file was substitutable"
    /// from "a name that had already been published away was recreated", and
    /// those two have opposite meanings.
    std::string wins;
    std::mutex namesMutex;
    std::thread th;

    void win(const char* what, const std::filesystem::path& p)
    {
        std::lock_guard<std::mutex> lk(namesMutex);
        wins += std::string(what) + ":" + p.filename().string() + " ";
    }

    void note(const std::filesystem::path& p)
    {
        std::lock_guard<std::mutex> lk(namesMutex);
        const auto n = p.filename().string();
        if (seenSet.insert(n).second) {
            seenNames += n + " ";
        }
    }

    /// Snapshot of every sibling name observed. Safe to call after join().
    std::set<std::string> observedNames()
    {
        std::lock_guard<std::mutex> lk(namesMutex);
        return seenSet;
    }

    /// `victims` is the set of filenames to ATTACK when non-empty; when empty
    /// every sibling except `skip` is attacked.
    void start(const std::filesystem::path& dir,
               const std::filesystem::path& decoy,
               std::set<std::string> skip,
               std::set<std::string> victims = {})
    {
        th = std::thread([this, dir, decoy, skip, victims] {
            while (!stop.load(std::memory_order_relaxed)) {
                std::error_code it_ec;
                for (std::filesystem::directory_iterator it(dir, it_ec), end;
                     !it_ec && it != end; it.increment(it_ec)) {
                    const auto p = it->path();
                    const auto name = p.filename().string();
                    if (name == decoy.filename().string()) continue;
                    if (skip.count(name)) continue;
                    if (!victims.empty() && !victims.count(name)) continue;
                    saw.fetch_add(1, std::memory_order_relaxed);
                    note(p);
                    if (!inOperation.load(std::memory_order_relaxed)) continue;

                    HANDLE w = ::CreateFileW(
                        p.native().c_str(), GENERIC_WRITE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                    if (w != INVALID_HANDLE_VALUE) {
                        openedForWrite.fetch_add(1, std::memory_order_relaxed);
                        win("wrote", p);
                        DWORD wrote = 0;
                        ::SetFilePointer(w, 0, nullptr, FILE_BEGIN);
                        if (::WriteFile(w, kPayload,
                                        static_cast<DWORD>(
                                            std::char_traits<char>::length(kPayload)),
                                        &wrote, nullptr)) {
                            tamperedBytes.fetch_add(static_cast<int>(wrote),
                                                    std::memory_order_relaxed);
                        }
                        ::FlushFileBuffers(w);
                        ::CloseHandle(w);
                    }

                    if (!inOperation.load(std::memory_order_relaxed)) continue;
                    HANDLE d = ::CreateFileW(
                        p.native().c_str(), DELETE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                    if (d == INVALID_HANDLE_VALUE) {
                        continue;  // gone, or refused — either way, no window
                    }
                    ::CloseHandle(d);
                    openedForDelete.fetch_add(1, std::memory_order_relaxed);

                    // Full-file substitution first: it is the stronger attack.
                    if (::MoveFileExW(decoy.native().c_str(), p.native().c_str(),
                                      MOVEFILE_REPLACE_EXISTING)) {
                        renamedOver.fetch_add(1, std::memory_order_relaxed);
                        win("renamed-over", p);
                        // Put the decoy back so the loop can keep trying.
                        std::error_code cp_ec;
                        std::filesystem::copy_file(
                            p, decoy,
                            std::filesystem::copy_options::overwrite_existing,
                            cp_ec);
                        continue;
                    }
                    if (::DeleteFileW(p.native().c_str())) {
                        deleted.fetch_add(1, std::memory_order_relaxed);
                        win("deleted", p);
                    }
                }
            }
        });
    }

    /// Open the window. Called immediately before the operation under test —
    /// NOT at start() — because the pre-operation state is deliberately
    /// unprotected: MainWindow holds the user's document through the CRT in the
    /// default share mode, so an attacker CAN write to it before the rewrite
    /// begins, and counting that would report a defect at a moment the product
    /// makes no claim about. Measured: without this the site-#4 case failed
    /// because the attacker clobbered work.pdf while it was merely OPEN.
    void beginOperation() { inOperation.store(true, std::memory_order_relaxed); }

    /// Call the instant the operation under test returns; join() may then run
    /// at leisure without the attacker acting on a world that is no longer the
    /// world under test.
    void endOperation() { inOperation.store(false, std::memory_order_relaxed); }

    void join()
    {
        stop.store(true, std::memory_order_relaxed);
        if (th.joinable()) th.join();
    }
};

/// Can an unprivileged opener get DELETE access to `p` right now? Used as the
/// CONTROL for the attacker's technique: after the operation under test has
/// finished and released everything, this must succeed. Without it, "the
/// attacker got zero delete handles" is equally explained by the technique not
/// working on this filesystem at all.
bool canTakeDeleteAccess(const std::filesystem::path& p)
{
    HANDLE d = ::CreateFileW(
        p.native().c_str(), DELETE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (d == INVALID_HANDLE_VALUE) return false;
    ::CloseHandle(d);
    return true;
}

#endif  // _WIN32

}  // namespace

#if defined(_WIN32)

// ===========================================================================
// A. The primitive the read side is built on: openDenyWriteReadStream.
//
// Class-level, deliberately: sites #4 and #5 both depend on this one property,
// and any future site that reads a file it is about to republish inherits it
// instead of having to re-earn it.
// ===========================================================================

TEST(DenyWriteReadStream, HoldsAReaderNoOneElseCanWriteRenameOrDelete)
{
    const auto dir = makeWorkDir("denywrite");
    const auto target = dir / "doc.pdf";
    { std::ofstream f(target, std::ios::binary); f << "%PDF-1.7\nORIGINAL\n"; }

    // CONTROL: with nothing held, all three attacks are available. Without
    // this, the assertions below could be satisfied by a filesystem on which
    // the attack never works.
    ASSERT_TRUE(canTakeDeleteAccess(target))
        << "precondition: an unheld file must be delete-openable, or the "
           "negative assertions below prove nothing";

    std::error_code ec;
    std::FILE* fp = pdfe::core::openDenyWriteReadStream(target, ec);
    ASSERT_NE(fp, nullptr) << ec.message();

    HANDLE w = ::CreateFileW(target.native().c_str(), GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    EXPECT_EQ(w, INVALID_HANDLE_VALUE)
        << "another process could still open the file being read FOR WRITING";
    if (w != INVALID_HANDLE_VALUE) ::CloseHandle(w);

    EXPECT_FALSE(canTakeDeleteAccess(target))
        << "another process could still take DELETE access, so it could rename "
           "a decoy over the file we are reading";

    // Readers stay welcome — denying them buys no integrity and breaks A/V,
    // backup agents and size-polling watchers.
    HANDLE r = ::CreateFileW(target.native().c_str(), GENERIC_READ,
                             FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    EXPECT_NE(r, INVALID_HANDLE_VALUE) << "readers must not be locked out";
    if (r != INVALID_HANDLE_VALUE) ::CloseHandle(r);

    // And the stream really reads the bytes.
    char head[9] = {0};
    ASSERT_EQ(std::fread(head, 1, 8, fp), 8u);
    EXPECT_EQ(std::string(head, 8), "%PDF-1.7");

    std::fclose(fp);

    // Released on close — the handle must not outlive the read, because a
    // handle this process holds blocks the later replace-existing rename.
    EXPECT_TRUE(canTakeDeleteAccess(target))
        << "the reader did not release the file on fclose";

    std::error_code rm;
    std::filesystem::remove_all(dir, rm);
}

TEST(DenyWriteReadStream, MissingFileIsADiagnosableRefusalNotAnEmptyStream)
{
    const auto dir = makeWorkDir("denywrite_missing");
    std::error_code ec;
    std::FILE* fp = pdfe::core::openDenyWriteReadStream(dir / "nope.pdf", ec);
    EXPECT_EQ(fp, nullptr);
    EXPECT_TRUE(static_cast<bool>(ec))
        << "a failed open must report WHY — a silent nullptr is what turns a "
           "refusal into a caller's fallback to opening by name";
    if (fp) std::fclose(fp);
    std::error_code rm;
    std::filesystem::remove_all(dir, rm);
}

// ===========================================================================
// B. Site #3 — RedactionManager::apply. TWO artifacts, and the INTERMEDIATE is
//    the higher-severity one.
// ===========================================================================

class RedactionPublishTocttou : public ::testing::Test {
protected:
    void SetUp() override
    {
        src_ = redactionFixture();
        if (src_.empty()) GTEST_SKIP() << "no suitable fixture available";
        dir_ = makeWorkDir("redact");
        work_ = dir_ / "work.pdf";
        std::filesystem::copy_file(src_, work_);
        decoy_ = makeValidDecoy(dir_);
        ctx_ = std::make_shared<MuPdfContext>();
    }
    void TearDown() override
    {
        pages_.clear();
        doc_.reset();
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }

    /// Run a full rewriteDocument=true redaction with `attacker` live for the
    /// whole operation.
    void runRedactionUnderAttack(SiblingAttacker& attacker,
                                 std::set<std::string> victims)
    {
        doc_ = Document::open(ctx_, work_);
        ASSERT_NE(doc_, nullptr);
        auto page = doc_->page(0);
        ASSERT_NE(page, nullptr);
        pdfe::redaction::RedactionManager mgr(ctx_);
        mgr.createRedactionMark(page.get(), QRectF(100, 660, 400, 30));
        pages_.push_back(std::move(page));

        attacker.start(dir_, decoy_, /*skip=*/{"work.pdf", "decoy_source.pdf"},
                       std::move(victims));

        pdfe::redaction::RedactionApplyOptions opts;
        opts.rewriteDocument = true;
        attacker.beginOperation();
        result_ = mgr.applyAllRedactions(doc_.get(), opts);
        attacker.endOperation();

        attacker.join();
        pages_.clear();
        doc_.reset();   // releases the bake temp so cleanup can run
    }

    std::filesystem::path src_, dir_, work_, decoy_;
    std::shared_ptr<MuPdfContext> ctx_;
    std::unique_ptr<Document> doc_;
    std::vector<std::unique_ptr<pdfe::core::Page>> pages_;
    pdfe::redaction::RedactionApplyResult result_;
};

// THE CRITERION-2 CASE — window A, the BAKE INTERMEDIATE.
//
// Until phase-57 that file was written by MuPDF at "<doc>.redact_tmp", left
// unheld under a name derived from the user's own document path, and then
// REOPENED BY NAME by QPDF, whose output is what gets published as the redacted
// document. Making the final rename atomic does not touch this at all: it
// faithfully publishes whatever QPDF produced.
//
// THE ATTACKER USED TO TARGET THE LITERAL NAME "work.pdf.redact_tmp" AND THAT
// IS NOW THE WRONG TEST — this is the cost of the fix, paid rather than
// skipped. The fix is that the bake is never published under a guessable name
// at all, so an attacker aimed at that one literal string would attack a file
// that does not exist and the case would go green FOR THE WRONG REASON. A test
// that can no longer fail is not a passing test.
//
// So it is re-pointed at the real surface: it enumerates the document's
// directory and attacks EVERY sibling that appears, winning if ANY staged
// artifact is writable, deletable or renameable-over under ANY name. That is
// strictly stronger than the literal form, and it is the only shape that stays
// honest once names are unpredictable.
//
// What keeps this case distinct from its sibling below is what it ASSERTS: the
// structural property that no intermediate with a PREDICTABLE name is ever
// created here, and that every artifact that does appear carries the
// StagingFile residue shape. That is the property that closes window A rather
// than narrowing it, and it is checked by name-shape rather than by racing.
TEST_F(RedactionPublishTocttou,
       PoisoningTheBakeIntermediateCannotSubstituteThePublishedRedaction)
{
    SiblingAttacker attacker;
    runRedactionUnderAttack(attacker, /*victims=*/{});

    // The window was REAL: an intermediate existed on disk and the attacker
    // found it. A green result below cannot therefore come from "no file was
    // ever there". The bake staging file is long-lived by construction — it is
    // held open across the entire QPDF rewrite — so this is satisfied by
    // construction rather than by winning a race.
    EXPECT_GT(attacker.saw.load(), 0)
        << "the attacker never observed ANY staged artifact, so this test "
           "proves nothing about the bake. Seen: [" << attacker.seenNames << "]";

    // WINDOW A IS CLOSED STRUCTURALLY, NOT NARROWED. Neither predictable name
    // may ever appear, and every artifact that does appear must be a
    // StagingFile residue ("work.pdf.pdfe_part.<32 hex>"), i.e. created
    // CREATE_NEW behind an exclusive handle at an unguessable name. Asserting
    // only "the old name is gone" would pass over a fix that swapped one
    // guessable name for another.
    const auto seen = attacker.observedNames();
    for (const auto& n : seen) {
        EXPECT_TRUE(StagingFile::isStagingResidueName("work.pdf", n))
            << "the redaction rewrite created an intermediate at a name that "
               "is not an unpredictable StagingFile residue: " << n
            << ". Window A is exactly the interval in which such a name exists.";
    }
    EXPECT_EQ(seen.count("work.pdf.redact_tmp"), 0u)
        << "the predictable bake intermediate is back";
    EXPECT_EQ(seen.count("work.pdf.redact_out"), 0u)
        << "the predictable rewrite output is back";

    EXPECT_EQ(attacker.openedForWrite.load(), 0)
        << "the bake intermediate was openable FOR WRITING while QPDF was "
           "about to parse it — attacker bytes reach the published redaction "
           "through it. Wins: [" + attacker.wins + "]";
    EXPECT_EQ(attacker.renamedOver.load(), 0)
        << "a valid decoy PDF was renamed over the bake intermediate; QPDF "
           "would have parsed it and republished it AS THE REDACTED DOCUMENT";
    EXPECT_EQ(attacker.deleted.load(), 0);

    EXPECT_TRUE(result_.success);
    // AVAILABILITY UNDER ATTACK IS DELIBERATELY *NOT* ASSERTED HERE, and the
    // first draft of this case got that wrong. It required
    // `documentRewritten == true` on the reasoning that "a fix that turns every
    // hostile directory into a failed redaction is a regression" — but that
    // pins a property WIN-87 explicitly trades away. WIN-87's contract, and
    // StagingFile's own header, trade availability FOR integrity: "save
    // refused, original byte-intact" is the prescribed acceptable outcome and
    // is strictly better than the substitution it replaces.
    //
    // So the oracle asserts the property that actually distinguishes fixed from
    // broken: the refusal must be DIAGNOSABLE, never silent. Against the
    // reverted code this still reds hard, because there the decoy is PARSED and
    // REPUBLISHED and the payload lands in work.pdf below.
    if (!result_.documentRewritten) {
        EXPECT_FALSE(result_.errorMessage.empty())
            << "the rewrite did not happen and NOTHING said why — a silent "
               "failure here leaves the user believing previous revisions were "
               "eliminated when they were not";
    }
    ASSERT_TRUE(looksLikeAPdf(work_)) << "the published document is not a PDF";
    EXPECT_FALSE(publishedDocumentRendersPayload(work_))
        << "THE PUBLISHED REDACTED DOCUMENT CONTAINS ATTACKER BYTES";
}

// The other artifact — window B, the QPDF output that is renamed over the
// user's document. Every sibling is attacked here, so this covers the staging
// file whatever it is called; the previous case is what pins the intermediate
// specifically.
TEST_F(RedactionPublishTocttou,
       PoisoningAnyStagedArtifactCannotSubstituteThePublishedRedaction)
{
    SiblingAttacker attacker;
    runRedactionUnderAttack(attacker, /*victims=*/{});

    EXPECT_GT(attacker.saw.load(), 0)
        << "the attacker observed no staging artifact at all. Seen: ["
        << attacker.seenNames << "]";
    EXPECT_EQ(attacker.openedForWrite.load(), 0)
        << "a staged artifact was writable. Seen: [" << attacker.seenNames
        << "] Wins: [" << attacker.wins << "]";
    EXPECT_EQ(attacker.renamedOver.load(), 0)
        << "a decoy was renamed over a staged artifact. Seen: ["
        << attacker.seenNames << "] Wins: [" << attacker.wins << "]";

    EXPECT_TRUE(result_.success);
    // Same deliberate omission as the sibling case above: availability under
    // attack is not claimed, a diagnosable refusal is. See that case's comment.
    if (!result_.documentRewritten) {
        EXPECT_FALSE(result_.errorMessage.empty())
            << "the rewrite did not happen and nothing said why";
    }
    ASSERT_TRUE(looksLikeAPdf(work_));
    EXPECT_FALSE(publishedDocumentRendersPayload(work_))
        << "THE PUBLISHED REDACTED DOCUMENT CONTAINS ATTACKER BYTES";

    // No residue of any kind: the staging file has no name of its own to leak.
    std::error_code ec;
    std::set<std::string> left;
    for (std::filesystem::directory_iterator it(dir_, ec), end;
         !ec && it != end; it.increment(ec)) {
        left.insert(it->path().filename().string());
    }
    EXPECT_EQ(left, (std::set<std::string>{"work.pdf", "decoy_source.pdf"}))
        << "the redaction rewrite left residue behind";
}

// ===========================================================================
// C. Site #4 — MainWindow's QPDF redaction rewrite. The attack is on the
//    SOURCE, which no acceptance criterion for this phase names.
// ===========================================================================

class MainWindowRedactionRewriteTocttou : public ::testing::Test {
protected:
    void SetUp() override
    {
        if (!qApp) GTEST_SKIP() << "QApplication not initialized";
        src_ = redactionFixture();
        if (src_.empty()) GTEST_SKIP() << "no suitable fixture available";
        dir_ = makeWorkDir("mwredact");
        work_ = dir_ / "work.pdf";
        std::filesystem::copy_file(src_, work_);
        decoy_ = makeValidDecoy(dir_);
        mw_ = std::make_unique<pdfe::ui::MainWindow>();
        QApplication::processEvents();
    }
    void TearDown() override
    {
        mw_.reset();
        QApplication::processEvents();
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }
    std::filesystem::path src_, dir_, work_, decoy_;
    std::unique_ptr<pdfe::ui::MainWindow> mw_;
};

// THE WINDOW NO CRITERION NAMES. The production sequence deliberately calls
// closeDocument() to release every lock so the later rename can land, and then
// re-resolves the SAME path by name to read it back. An attacker who renames a
// valid decoy PDF over it in that interval has QPDF read attacker bytes and
// republish them over the user's document as the redacted result — with no
// staging file involved in the attack anywhere.
//
// Recorded as a dated supersession in DCA 0022 and in the RTM: acceptance
// criterion 3 names ".redact_out / the QPDF rewrite's equivalent intermediate",
// which is the OUTPUT. The input window is outside its wording and is fixed
// and tested here anyway.
TEST_F(MainWindowRedactionRewriteTocttou,
       PoisoningTheSourceAcrossTheDeliberateCloseCannotRepublishAttackerBytes)
{
    mw_->openFile(QString::fromStdString(work_.string()));
    QApplication::processEvents();
    ASSERT_NE(mw_->documentForTesting(), nullptr);

    // NO FREE-RUNNING ATTACKER ON THIS CASE, AND THAT IS THE FIX, NOT A
    // WEAKENING. A racing thread was the ONLY actor here, and it produced two
    // failures at once: it missed the microsecond window (so the case passed
    // against the REVERTED, vulnerable build — measured), and it also wrote to
    // work.pdf AFTER publication, which made the content oracle fail ~2 runs in
    // 5 against the FIXED build. The pre-existing comment below already
    // recorded that second effect as "1 run in 5" and worked around it by
    // splitting the oracle on `ok`; the split is what let the first effect hide.
    //
    // The seam below replaces the race with a deterministic act at exactly the
    // instant under test, so the attacker is now pure noise on this case: it can
    // only damage the artifact being asserted on, at moments PDFE has never
    // claimed to defend (a process with write access writing to a document that
    // is merely open — conceded below and still true). Sites #3 and #5 keep
    // their free-running attackers, where the artifact is long-lived and the
    // race is winnable.

    // THE DETERMINISTIC SEAM, AND WHY THE FREE-RUNNING ATTACKER ALONE WAS NOT
    // ENOUGH. This window is microseconds wide, and unlike sites #3 and #5 it
    // cannot be closed structurally — the artifact under attack is the USER'S
    // OWN DOCUMENT, whose name is the thing being protected. A racing thread
    // therefore hits it only by luck, and MEASURED IT DID NOT: revert-and-
    // observe found this very case passing against the REVERTED, vulnerable
    // build (`[ OK ]`, 915 ms), i.e. a green test over a live defect. That is
    // the "criterion satisfied by evidence that cannot establish it" species
    // this programme keeps catching, and it had reached the phase's own
    // acceptance evidence.
    //
    // The hook fires INSIDE the window: locks released, source not yet read.
    // Hardened build — the deny-write pin is already held, so this write fails.
    // Pre-phase-57 build — nothing is held, the decoy lands, and QPDF parses
    // and republishes it. Red every run instead of one run in N.
    std::atomic<bool> seamRan{false};
    std::atomic<bool> seamSubstituted{false};
    // RAII, not a bare reset statement. The hook is a STATIC that outlives the
    // test and captures `this`; a GTEST_SKIP, a throw, or a future early return
    // before the reset would leave it armed with a DANGLING capture, firing
    // inside any later case that drives this path. Found by the non-gate
    // security review.
    struct HookArmer {
        ~HookArmer() {
            pdfe::ui::MainWindow::setRedactionRewriteWindowHookForTesting(nullptr);
        }
    } hookArmer;
    pdfe::ui::MainWindow::setRedactionRewriteWindowHookForTesting(
        [&seamRan, &seamSubstituted, this]() {
            seamRan.store(true, std::memory_order_relaxed);
            std::error_code ec;
            std::filesystem::copy_file(
                decoy_, work_,
                std::filesystem::copy_options::overwrite_existing, ec);
            seamSubstituted.store(!ec, std::memory_order_relaxed);
        });

    QString err;
    const bool ok = mw_->rewriteRedactedDocumentThroughQpdfForTesting(work_, &err);

    // WITNESS. Without this, a green result is equally explained by a seam that
    // never fired — the same vacuity the `attacker.saw` guards catch elsewhere.
    ASSERT_TRUE(seamRan.load())
        << "the publish-window seam never fired, so this case proves nothing "
           "about site #4";
    // Diagnostic, deliberately NOT an assertion: whether the substitution was
    // blocked is an implementation detail of the fix. The PROPERTY is that
    // attacker bytes are never republished, asserted below.
    RecordProperty("seam_substituted_the_source",
                   seamSubstituted.load() ? 1 : 0);

    // THE CONTENT ORACLE IS SPLIT ON WHETHER PDFE ACTUALLY PUBLISHED, AND THAT
    // SPLIT IS A CORRECTION, NOT A WEAKENING. MEASURED: this case failed 1 run
    // in 5 on `looksLikeAPdf(work_)` — and the byte that failed it was written
    // by the ATTACKER, directly into the user's document, during the interval
    // between beginOperation() and the pin the product takes INSIDE the call.
    // The comment further down already conceded that interval belongs to nobody
    // and refused to assert the counters over it; asserting the file's CONTENT
    // over it was the same mistake in a place nobody had noticed.
    //
    // PDFE cannot stop a process that already has write access to your document
    // from writing to it while the document is merely open. That is not the
    // property under test. The property under test is that PDFE never
    // REPUBLISHES attacker bytes as the redacted result — so:
    //
    //   * if the rewrite PUBLISHED (ok), the published bytes are PDFE's output
    //     and must be a clean PDF. Unconditional, and this is the assertion
    //     that reddens against the reverted code, where QPDF re-resolves the
    //     source by name, parses the decoy and republishes it.
    //   * if the rewrite REFUSED (!ok), PDFE wrote nothing. The claim is that
    //     the refusal is diagnosable — and, when the attacker never actually
    //     obtained access, that the document is still intact, which keeps a
    //     genuine "PDFE corrupted it on the failure path" defect in scope.
    const bool attackerReachedTheSource = seamSubstituted.load();

    if (ok) {
        ASSERT_TRUE(looksLikeAPdf(work_))
            << "the PUBLISHED document is not a PDF";
        EXPECT_FALSE(publishedDocumentRendersPayload(work_))
            << "A DECOY PDF WAS REPUBLISHED AS THE REDACTED DOCUMENT. The "
               "attack landed on the SOURCE between closeDocument() and the "
               "read, which no staging-file hardening can prevent.";
    } else {
        EXPECT_FALSE(err.isEmpty())
            << "a failed rewrite must say why; a silent false is not a "
               "diagnosable error";
        if (!attackerReachedTheSource) {
            ASSERT_TRUE(looksLikeAPdf(work_))
                << "the rewrite failed and the attacker never obtained any "
                   "access, so PDFE itself damaged the user's document on its "
                   "own failure path";
            EXPECT_FALSE(publishedDocumentRendersPayload(work_))
                << "attacker bytes are in the document although the attacker "
                   "never obtained access to it — impossible unless PDFE put "
                   "them there";
        } else {
            RecordProperty("refused_after_attacker_damaged_source", 1);
        }
    }


    // THE COUNTERS ARE DIAGNOSTICS HERE, NOT ASSERTIONS, AND THE REASON IS
    // WORTH MORE THAN THE COUNTERS WOULD HAVE BEEN.
    //
    // The exclusivity property — "while the read handle is held, nobody can
    // write or delete" — is real and IS asserted, at
    // DenyWriteReadStream.HoldsAReaderNoOneElseCanWriteRenameOrDelete, where
    // the test owns the handle and can bound the interval exactly. It cannot
    // be asserted from OUT HERE, because the handle is acquired INSIDE the
    // function under test: between beginOperation() and that acquisition there
    // is a short interval that belongs to nobody, and the product makes no
    // claim about it. Measured, an attacker does win a substitution there
    // roughly once per run.
    //
    // Asserting zero would therefore have been a flaky test pinning a property
    // the code does not have. What the code DOES have is the property the
    // payload assertion above measures, and the measurement is stronger for
    // the substitution having landed: the attacker replaced the source file
    // outright, and the document that got published is still the user's,
    // because QPDF read through a handle taken before the swap rather than
    // through the name the swap changed.
    RecordProperty("seam_substituted_the_source_during_window",
                   seamSubstituted.load() ? 1 : 0);

    // The attacker's technique works on this file when nothing is protecting
    // it — so a clean published document is evidence about the pin, not about
    // an attack that could never have landed anywhere.
    EXPECT_TRUE(canTakeDeleteAccess(work_))
        << "the attacker's technique does not work on this file even when "
           "nothing is protecting it, so nothing above is evidence";
}

// THE ARTIFACT CRITERION 3 ACTUALLY NAMES — site #4's OUTPUT, not its input.
//
// The case above attacks the SOURCE, and that window is real and was worth
// closing. It is not what acceptance criterion 3 asks for: the criterion names
// ".redact_out / the QPDF rewrite's equivalent intermediate", which is the file
// PDFE WRITES and then renames over the user's document. The phase's first pass
// gave site #4 a source-side seam only, so the OUTPUT — the artifact the whole
// WIN-87 primitive exists to protect — had code-level hardening and no hostile
// test at all. Caught by the phase-4 validator gate.
//
// The tell was an ASYMMETRY, not an absence: site #5 had both a staging-side
// and a source-side seam from the start, and site #4 had only one. Two sites
// with the same shape and different coverage is a coverage question, and it
// went unasked because the source-side window was the more interesting finding.
TEST_F(MainWindowRedactionRewriteTocttou,
       PoisoningTheStagedRewriteBeforeItIsPublishedCannotSubstituteTheRedaction)
{
    mw_->openFile(QString::fromStdString(work_.string()));
    QApplication::processEvents();
    ASSERT_NE(mw_->documentForTesting(), nullptr);

    // In-window control, for the same measured reason as the other seam cases:
    // an "and the technique works unprotected" check taken afterwards cannot
    // distinguish a protected target from a busy one.
    const std::filesystem::path control = dir_ / "control_unprotected.pdf";
    std::filesystem::copy_file(work_, control);

    std::atomic<bool> seamRan{false};
    std::atomic<bool> seamSubstituted{false};
    std::atomic<bool> controlSubstituted{false};
    std::string stagingName;
    std::mutex stagingNameMutex;
    struct HookArmer {
        ~HookArmer() {
            pdfe::ui::MainWindow::setRedactionOutputStagingWindowHookForTesting(
                nullptr);
        }
    } hookArmer;
    pdfe::ui::MainWindow::setRedactionOutputStagingWindowHookForTesting(
        [&seamRan, &seamSubstituted, &controlSubstituted, &stagingName,
         &stagingNameMutex, control, this](const std::filesystem::path& staging) {
            seamRan.store(true, std::memory_order_relaxed);
            {
                std::lock_guard<std::mutex> lk(stagingNameMutex);
                stagingName = staging.filename().string();
            }
            std::error_code controlEc;
            std::filesystem::copy_file(
                decoy_, control,
                std::filesystem::copy_options::overwrite_existing, controlEc);
            controlSubstituted.store(!controlEc, std::memory_order_relaxed);
            std::error_code ec;
            std::filesystem::copy_file(
                decoy_, staging,
                std::filesystem::copy_options::overwrite_existing, ec);
            seamSubstituted.store(!ec, std::memory_order_relaxed);
        });

    QString err;
    const bool ok = mw_->rewriteRedactedDocumentThroughQpdfForTesting(work_, &err);

    ASSERT_TRUE(seamRan.load())
        << "the output-staging seam never fired, so this case proves nothing "
           "about the artifact criterion 3 names";
    RecordProperty("seam_substituted_the_staged_output",
                   seamSubstituted.load() ? 1 : 0);

    // THE NAME IS PART OF THE PROPERTY. A fix that swapped ".redact_out" for
    // another guessable name would satisfy everything else here.
    {
        std::lock_guard<std::mutex> lk(stagingNameMutex);
        EXPECT_TRUE(pdfe::core::StagingFile::isStagingResidueName(
                        "work.pdf", stagingName))
            << "the QPDF rewrite staged its output under a name that is not an "
               "unpredictable staging residue: " << stagingName;
    }

    if (ok) {
        ASSERT_TRUE(looksLikeAPdf(work_))
            << "the PUBLISHED document is not a PDF";
        EXPECT_FALSE(publishedDocumentRendersPayload(work_))
            << "A DECOY PDF WAS PUBLISHED AS THE REDACTED DOCUMENT. The "
               "substitution landed on the STAGED OUTPUT between the last write "
               "to it and the rename that publishes it — the exact artifact "
               "acceptance criterion 3 names.";
    } else {
        EXPECT_FALSE(err.isEmpty())
            << "a failed rewrite must say why; a silent false is not a "
               "diagnosable error";
        ASSERT_TRUE(looksLikeAPdf(work_))
            << "the rewrite failed, so PDFE published nothing and the user's "
               "document must be byte-intact";
    }

    EXPECT_TRUE(controlSubstituted.load())
        << "the substitution technique failed even on an UNPROTECTED sibling "
           "in the same window, so a clean document above is evidence about "
           "nothing";
}

// ===========================================================================
// D. Site #5 — MainWindow's alignment flush. THE WIDEST WINDOW OF THE SEVEN.
//
// CRITERION 6 IS A TEST-DESIGN CONSTRAINT, NOT A STYLE PREFERENCE. The flush's
// documented precondition is that the caller has ALREADY CLOSED the document,
// so Windows will permit the in-place rewrite: onSaveFile/onSaveAsFile call
// closeDocument() before and openFile() after. That closed-document window is
// the property that makes this site worse than the redaction pair. A test that
// called the flush directly, with the document still open, would hold locks the
// production path deliberately releases — and would therefore PASS AGAINST THE
// VULNERABLE CODE while looking exactly like coverage.
//
// So this drives the real File>Save action, exactly as AlignmentE2ETest does.
// ===========================================================================

class AlignmentFlushTocttou : public ::testing::Test {
protected:
    void SetUp() override
    {
        if (!qApp) GTEST_SKIP() << "QApplication not initialized";
        const auto src = fixtures() / "test_single.pdf";
        if (!std::filesystem::exists(src)) GTEST_SKIP() << "test_single.pdf missing";
        dir_ = makeWorkDir("align");
        work_ = dir_ / "work.pdf";
        std::filesystem::copy_file(src, work_);
        decoy_ = makeValidDecoy(dir_);
        mw_ = std::make_unique<pdfe::ui::MainWindow>();
        QApplication::processEvents();
    }
    void TearDown() override
    {
        mw_.reset();
        QApplication::processEvents();
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }
    std::filesystem::path dir_, work_, decoy_;
    std::unique_ptr<pdfe::ui::MainWindow> mw_;
};

TEST_F(AlignmentFlushTocttou,
       PoisoningTheFlushStagingAcrossTheClosedDocumentBracketCannotSubstituteTheSave)
{
    mw_->openFile(QString::fromStdString(work_.string()));
    QApplication::processEvents();
    ASSERT_NE(mw_->documentForTesting(), nullptr);

    mw_->queueAlignmentForTest(
        /*pageIndex=*/0, Qt::AlignCenter,
        QRectF(72.0, 700.0, 400.0, 60.0),
        "win87 phase57 alignment flush hostile filesystem marker text",
        /*fontSizePt=*/12.0f);

    SiblingAttacker attacker;
    // Everything except the user's document and our own decoy: the staging
    // artifact, whatever it is called. Under the pre-phase-57 code that set
    // contains "work.pdf.align.tmp".
    attacker.start(dir_, decoy_, /*skip=*/{"work.pdf", "decoy_source.pdf"});

    auto* saveAction = mw_->findChild<QAction*>("saveAction");
    ASSERT_NE(saveAction, nullptr);
    ASSERT_TRUE(saveAction->isEnabled());
    attacker.beginOperation();
    saveAction->trigger();          // real closeDocument()/openFile() bracket
    QApplication::processEvents();
    attacker.endOperation();
    attacker.join();

    // VACUITY GUARD. If the flush did not actually run, every assertion below
    // is trivially satisfied. The marker proves the alignment injection
    // happened and was published.
    const std::string published = readAll(work_);
    ASSERT_NE(published.find("%%PDFE-ALIGN"), std::string::npos)
        << "the alignment flush never published anything, so this test did not "
           "exercise the site at all";

    EXPECT_GT(attacker.saw.load(), 0)
        << "the attacker observed no staging artifact during a save that "
           "definitely staged one. Seen: [" << attacker.seenNames << "]";
    EXPECT_EQ(attacker.openedForWrite.load(), 0)
        << "a staging artifact was writable across the closed-document window. "
           "Seen: [" << attacker.seenNames << "]";
    EXPECT_EQ(attacker.renamedOver.load(), 0)
        << "a decoy PDF was renamed over a staging artifact. Seen: ["
        << attacker.seenNames << "]";

    ASSERT_TRUE(looksLikeAPdf(work_));
    // TEXT EXTRACTION, NOT A BYTE SCAN. The decoy carries kPayload in an
    // UNCOMPRESSED content stream, so the literal bytes survive in the DECOY
    // FILE and not necessarily in anything the flush republishes from it —
    // the injector re-serialises through MuPDF. A byte scan here would be
    // blind in exactly the way the site-#4 scan was measured to be blind,
    // where it passed with the substitution confirmed landed.
    EXPECT_FALSE(publishedDocumentRendersPayload(work_))
        << "ATTACKER BYTES WERE PUBLISHED OVER THE DOCUMENT THE USER JUST SAVED";

    // No ".align.tmp" and no other residue may survive the flush.
    std::error_code ec;
    std::set<std::string> left;
    for (std::filesystem::directory_iterator it(dir_, ec), end;
         !ec && it != end; it.increment(ec)) {
        left.insert(it->path().filename().string());
    }
    left.erase("work.pdf");
    left.erase("decoy_source.pdf");
    EXPECT_TRUE(left.empty()) << "alignment flush residue survived the save";
}

// THE SAME STAGING WINDOW AS THE CASE ABOVE, MEASURED DETERMINISTICALLY.
//
// The case above is the right shape and it CANNOT SATISFY CRITERION 8, because
// against the reverted (pre-phase-57) code it does not terminate: killed at
// 600 s on 2026-08-11, after an earlier run of it took the whole
// revert-and-observe harness down at 5400 s and discarded two other sites'
// completed measurements. The fixed build passes it in about a second. A hang
// is an ABSENCE of measurement, not a red, so "reverting the site makes its
// hostile test fail" was left unestablished.
//
// This case establishes it. It keeps the same subject — the staging artifact,
// attacked at the instant it is fully written and not yet published — and
// removes the only thing that can wedge: there is no free-running attacker
// thread at all. The seam acts once, at exactly that instant. Reverted, the
// artifact is an ordinary file called "<doc>.align.tmp" and the substitution
// lands and is published; fixed, the StagingFile still holds it exclusively at
// an unpredictable name and the substitution fails.
//
// It does NOT diagnose the hang, and must not be read as having done so. That
// remains a phase-5 obligation — the shape that hangs publishes through
// `atomicReplaceFileWithRetry`, which sites #6/#7 still use.
TEST_F(AlignmentFlushTocttou,
       PoisoningTheStagingArtifactAtTheDeterministicPublishInstantCannotSubstituteTheSave)
{
    mw_->openFile(QString::fromStdString(work_.string()));
    QApplication::processEvents();
    ASSERT_NE(mw_->documentForTesting(), nullptr);

    mw_->queueAlignmentForTest(
        /*pageIndex=*/0, Qt::AlignCenter,
        QRectF(72.0, 700.0, 400.0, 60.0),
        "win87 phase57 alignment staging-substitution marker text",
        /*fontSizePt=*/12.0f);

    // Same in-window control as the input case, and for the same MEASURED
    // reason: `canTakeDeleteAccess(work_)` afterwards fails against the CORRECT
    // build, because onSaveFile ends with openFile() and MuPDF holds the
    // document again by the time the test looks.
    const std::filesystem::path control = dir_ / "control_unprotected.pdf";
    std::filesystem::copy_file(work_, control);

    std::atomic<bool> seamRan{false};
    std::atomic<bool> seamSubstituted{false};
    std::atomic<bool> controlSubstituted{false};
    std::string stagingName;
    std::mutex stagingNameMutex;
    struct HookArmer {
        ~HookArmer() {
            pdfe::ui::MainWindow::setAlignStagingWindowHookForTesting(nullptr);
        }
    } hookArmer;
    pdfe::ui::MainWindow::setAlignStagingWindowHookForTesting(
        [&seamRan, &seamSubstituted, &controlSubstituted, &stagingName,
         &stagingNameMutex, control, this](const std::filesystem::path& staging) {
            seamRan.store(true, std::memory_order_relaxed);
            {
                std::lock_guard<std::mutex> lk(stagingNameMutex);
                stagingName = staging.filename().string();
            }
            std::error_code controlEc;
            std::filesystem::copy_file(
                decoy_, control,
                std::filesystem::copy_options::overwrite_existing, controlEc);
            controlSubstituted.store(!controlEc, std::memory_order_relaxed);
            std::error_code ec;
            std::filesystem::copy_file(
                decoy_, staging,
                std::filesystem::copy_options::overwrite_existing, ec);
            seamSubstituted.store(!ec, std::memory_order_relaxed);
        });

    auto* saveAction = mw_->findChild<QAction*>("saveAction");
    ASSERT_NE(saveAction, nullptr);
    ASSERT_TRUE(saveAction->isEnabled());
    saveAction->trigger();          // real closeDocument()/openFile() bracket
    QApplication::processEvents();

    ASSERT_TRUE(seamRan.load())
        << "the staging-publish seam never fired, so this case says nothing "
           "about site #5";
    RecordProperty("seam_substituted_the_staging_artifact",
                   seamSubstituted.load() ? 1 : 0);

    // THE NAME IS PART OF THE PROPERTY, not decoration. A fix that swapped one
    // guessable name for another would satisfy every assertion below while
    // leaving the site attackable by a process that simply knows the document's
    // path. Asserted here for the same reason `isStagingResidueName` is
    // asserted at site #3.
    {
        std::lock_guard<std::mutex> lk(stagingNameMutex);
        // The base name IS derived from the document ("work.pdf.pdfe_part.<32
        // hex>") and that is fine: what an attacker cannot guess is the
        // 128 bits of CSPRNG suffix, which is exactly the shape this predicate
        // enforces. It also fails ".align.tmp" and any other fix that merely
        // swapped one guessable name for another.
        EXPECT_TRUE(pdfe::core::StagingFile::isStagingResidueName(
                        "work.pdf", stagingName))
            << "the alignment flush staged under a name that is not an "
               "unpredictable staging residue: " << stagingName;
    }

    ASSERT_TRUE(looksLikeAPdf(work_))
        << "the document the user saved is not a PDF";
    EXPECT_FALSE(publishedDocumentRendersPayload(work_))
        << "A DECOY PDF WAS PUBLISHED OVER THE DOCUMENT THE USER JUST SAVED. "
           "The substitution landed on the staging artifact between the last "
           "write to it and the rename that publishes it.";

    EXPECT_TRUE(controlSubstituted.load())
        << "the substitution technique failed even on an UNPROTECTED sibling "
           "in the same window, so a clean document above is evidence about "
           "nothing";

    std::error_code ec;
    std::set<std::string> left;
    for (std::filesystem::directory_iterator it(dir_, ec), end;
         !ec && it != end; it.increment(ec)) {
        left.insert(it->path().filename().string());
    }
    left.erase("work.pdf");
    left.erase("decoy_source.pdf");
    left.erase("control_unprotected.pdf");
    EXPECT_TRUE(left.empty()) << "alignment flush residue survived the save";
}

// THE OTHER SITE-#5 WINDOW — ON THE INPUT, NOT THE STAGING FILE.
//
// The case above attacks the artifact the flush WRITES. This one attacks the
// artifact it READS, and the two are not the same defect. Phase-57 made the
// staging name unpredictable, which closes the write side structurally. The
// read side cannot be closed that way: the file being read is the user's own
// document, and its name is the thing being protected.
//
// So the property is a SEQUENCING one — the pin must be taken BEFORE
// closeDocument() releases the document, by the caller, and handed in. It was
// not: the pin was taken inside flushAlignmentPipelineIfPending, i.e. after the
// caller had already released everything, leaving the just-saved document
// unheld at its own predictable name across the gap. The function's own comment
// asserted "the name is resolved exactly twice, both times up front ... so
// nothing can substitute the document being aligned" — a true statement about
// that FUNCTION standing in for a false one about the SEQUENCE it belongs to.
// Caught by the phase-4 karen gate, after the fix for the staging side had
// already been reviewed and measured.
//
// NOTHING WITNESSED THAT FIX UNTIL THIS CASE. The case above cannot: its
// attacker skips "work.pdf" by construction, because attacking the user's
// document with a free-running thread is what made the site-#4 case flaky. A
// fix with no oracle is the thing this phase spent its whole length rejecting,
// so it does not get to ship in the correction itself.
TEST_F(AlignmentFlushTocttou,
       PoisoningTheSOURCEAcrossTheClosedDocumentBracketCannotSubstituteTheSave)
{
    mw_->openFile(QString::fromStdString(work_.string()));
    QApplication::processEvents();
    ASSERT_NE(mw_->documentForTesting(), nullptr);

    mw_->queueAlignmentForTest(
        /*pageIndex=*/0, Qt::AlignCenter,
        QRectF(72.0, 700.0, 400.0, 60.0),
        "win87 phase57 alignment source-substitution marker text",
        /*fontSizePt=*/12.0f);

    // DETERMINISTIC SEAM, for the same reason site #4 needed one: this window
    // is microseconds wide and a racing thread hits it only by luck. Measured
    // at site #4, luck was not enough — the hostile case passed against the
    // reverted, vulnerable build. The hook fires with every handle released and
    // the source not yet read.
    //
    // THE CONTROL FIRES IN THE SAME WINDOW, and it has to. The usual
    // "canTakeDeleteAccess(work_) afterwards" control does not work at this
    // site and MEASURED it does not: onSaveFile ends with openFile(), so by the
    // time the test regains control MuPDF holds the document again and the
    // control fails against the CORRECT build — a red test over working code,
    // which is the same species as a green test over broken code. Instead the
    // seam substitutes an UNPROTECTED sibling first. If that succeeds and the
    // one on the document fails, the technique works and the pin is the only
    // thing that stopped it.
    const std::filesystem::path control = dir_ / "control_unprotected.pdf";
    std::filesystem::copy_file(work_, control);

    std::atomic<bool> seamRan{false};
    std::atomic<bool> seamSubstituted{false};
    std::atomic<bool> controlSubstituted{false};
    // RAII: the hook is a static that outlives the test and captures `this`.
    struct HookArmer {
        ~HookArmer() {
            pdfe::ui::MainWindow::setSaveCloseWindowHookForTesting(nullptr);
        }
    } hookArmer;
    pdfe::ui::MainWindow::setSaveCloseWindowHookForTesting(
        [&seamRan, &seamSubstituted, &controlSubstituted, control, this]() {
            seamRan.store(true, std::memory_order_relaxed);
            std::error_code controlEc;
            std::filesystem::copy_file(
                decoy_, control,
                std::filesystem::copy_options::overwrite_existing, controlEc);
            controlSubstituted.store(!controlEc, std::memory_order_relaxed);
            std::error_code ec;
            std::filesystem::copy_file(
                decoy_, work_,
                std::filesystem::copy_options::overwrite_existing, ec);
            seamSubstituted.store(!ec, std::memory_order_relaxed);
        });

    auto* saveAction = mw_->findChild<QAction*>("saveAction");
    ASSERT_NE(saveAction, nullptr);
    ASSERT_TRUE(saveAction->isEnabled());
    saveAction->trigger();          // real closeDocument()/openFile() bracket
    QApplication::processEvents();

    // WITNESS. A green explained by a seam that never fired proves nothing.
    ASSERT_TRUE(seamRan.load())
        << "the save-window seam never fired, so this case says nothing about "
           "the input side of site #5";
    // Diagnostic, not an assertion: whether the substitution was BLOCKED is an
    // implementation detail of the pin. The property is that attacker bytes are
    // never published as the saved document.
    RecordProperty("seam_substituted_the_source",
                   seamSubstituted.load() ? 1 : 0);

    ASSERT_TRUE(looksLikeAPdf(work_))
        << "the document the user saved is not a PDF";
    EXPECT_FALSE(publishedDocumentRendersPayload(work_))
        << "A DECOY PDF WAS ALIGNED AND PUBLISHED AS THE USER'S SAVED "
           "DOCUMENT. The attack landed on the SOURCE across the deliberate "
           "closeDocument(), which no staging-file hardening can prevent.";

    // See the control's rationale above: same act, same instant, same file
    // content, differing only in whether the product pinned it.
    EXPECT_TRUE(controlSubstituted.load())
        << "the substitution technique failed even on an UNPROTECTED sibling "
           "in the same window, so a clean document above is evidence about "
           "nothing";
}

#endif  // _WIN32
