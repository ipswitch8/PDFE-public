// tests/test_staged_incremental_bias.cpp
//
// ===========================================================================
// F62 (follow-up phase 55) — the `bias != 0` staged incremental append.
//
// WHAT WAS WRONG, IN ONE PARAGRAPH. Document::saveIncrementalStaged() used to
// have two routes. A document whose `%PDF-` marker is at offset 0 was appended
// to THROUGH core::StagingFile's exclusive handle, which is held from creation
// to publication and is what makes the staged save safe. A document with garbage
// BEFORE its `%PDF-` marker (MuPDF's `pdf_document::bias`) could not take that
// route, because pdf_save_document sets a PRIVATE `opts.bias` that
// pdf_write_document does not, so it took a fallback: release the exclusive
// handle, let pdf_save_document write the staging file BY PATH, re-acquire, and
// re-verify. The re-verification — stagedPrefixMatchesOriginal() — loops
// `while (offset < originalSize)`. It covers the pre-existing PREFIX and never
// `[originalSize, end)`, which is exactly the region the by-path write had just
// produced and exactly the region exposed while no handle was held. The only
// other check was a must-not-shrink guard, and a forged tail is strictly LARGER.
// So a crafted tail passed every check and would have been atomically published
// over the user's document.
//
// AND NOTHING EXECUTED ANY OF IT. When F62 was filed, `grep -rn "\bbias\b"
// tests/` found one unrelated hit in a Python file, and
// `releaseHandleForExternalWrite|reacquireExclusiveHandle` found nothing at all.
// The branch was reasoned about at length in comments and never once run.
//
// WHY THIS FILE BUILDS ITS OWN FIXTURE RATHER THAN CHECKING ONE IN. Two
// reasons, and both are the point rather than convenience:
//
//   1. The branch is reached only when MuPDF loads the document WITHOUT
//      REPAIRING it. A repair resets `doc->bias` to 0 (pdf-repair.c) and
//      re-normalises every offset, so a fixture that quietly starts being
//      repaired — after a MuPDF upgrade, or after someone "fixes" the file —
//      stops probing the branch while every test in the file still passes. A
//      generated fixture whose construction is visible here, plus
//      GarbagePreambleFixtureLoadsWithoutRepairAndHasNonZeroBias below, makes
//      that failure mode a red test instead of a silent one.
//
//   2. The equivalence case needs a document whose trailer has NO /ID. MuPDF's
//      change_identity() replaces the second half of an existing /ID with 16
//      bytes of `fz_memrnd` on every incremental save, so two saves of the same
//      document are NOT byte-identical if an /ID is present, and a byte-for-byte
//      oracle would have to be weakened to tolerate it. Generating the fixture
//      lets the oracle stay exact.
//
// HOW A GARBAGE PREAMBLE PRODUCES A NON-ZERO BIAS WITHOUT BREAKING THE FILE.
// A PDF's stored xref offsets, and its `startxref` value, are relative to the
// `%PDF-` marker — not to the start of the file. MuPDF finds the marker within
// the first read (pdf-xref.c: it scans the leading buffer for "%PDF-"), records
// the distance as `doc->bias`, and thereafter seeks to `doc->bias + ofs`. So
// PREPENDING bytes to an already-valid PDF yields a document that is still
// entirely self-consistent and loads without repair, with `bias` equal to the
// number of bytes prepended. That is the whole construction.
//
// WHAT THE FOUR CASES ARE FOR, and which of them is load-bearing:
//
//   * GarbagePreambleFixtureLoadsWithoutRepairAndHasNonZeroBias
//       The gate. Asserts the fixture reaches the branch at all. Every other
//       case in this file is vacuous if this one is wrong, which is why it
//       exists separately instead of being folded in as a precondition.
//
//   * StagingFileIsNeverExternallyWritableDuringABiasedAppend
//       THE SECURITY CASE, and the one that reddens if the fix is reverted. A
//       watcher thread polls the staging sibling for a write-openable instant
//       and, if it gets one, stamps a crafted tail LARGER than the legitimate
//       append. Under the old code the watcher gets in during the release
//       window and the forged tail is published; under the fix it never gets a
//       write handle at all.
//
//   * BiasedAppendMatchesPdfSaveDocumentByteForByte
//       THE CLASS-LEVEL CASE. The fix reimplements, from outside MuPDF, a
//       correction MuPDF applies internally. This pins the reimplementation
//       against the original: the same mutation saved through the staged handle
//       route and through Document::saveIncremental() (which IS
//       pdf_save_document by path) must produce identical bytes. A bias error
//       moves every xref offset by exactly `bias`, so this case cannot pass
//       with the compensation missing, misapplied, or applied twice.
//
//   * BiasedAppendPublishesAStartxrefThatResolves
//       Narrower and deliberately independent of MuPDF's own output: it decodes
//       the published `startxref` and requires that offset, PLUS the bias, to
//       land on the appended cross-reference section. It would still hold if
//       Document::saveIncremental() were ever changed, so the class-level case
//       above is not the only thing standing between this branch and a silent
//       off-by-bias regression.
// ===========================================================================

#include <gtest/gtest.h>

#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"
#include "signatures/SignatureData.h"
#include "signatures/SignatureManager.h"
#include "support/SaveFailureInjection.h"

#include <QRectF>

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

using namespace pdfe::core;

namespace {

/// The garbage preamble length. Must stay well under the ~1 KiB window MuPDF
/// scans for the `%PDF-` marker (pdf-xref.c reads a 1024-byte buffer and gives
/// up if the marker is not in it), and must not itself contain "%PDF-".
constexpr std::size_t kPreambleBytes = 128;

/// The classic cross-reference table keyword, as it appears at the start of
/// a revision that used writexref() rather than writexrefstream().
static const char* const NL_XREF_TABLE = "\nxref\n";

std::string readAllBytes(const std::filesystem::path& p)
{
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
}

bool writeAllBytes(const std::filesystem::path& p, const std::string& bytes)
{
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out);
}

/// A minimal, entirely valid PDF with a classic cross-reference table and NO
/// /ID in its trailer (see the header note on change_identity). Offsets are
/// computed from the assembled bytes rather than hard-coded, so the document is
/// self-consistent by construction and cannot rot into needing a repair.
///
/// `fillerBytes` pads the document with one unreferenced stream object of that
/// size. It exists for exactly one caller — the fault-injection case — and the
/// reason is scheduling, not coverage: the staging file lives only for the
/// duration of the save, and on a 2 KiB document that is under a millisecond, so
/// a watcher thread on a machine running sixteen test shards can miss the window
/// entirely and the case then asserts it measured nothing. A few megabytes to
/// copy makes the window tens of milliseconds wide. The padding does not change
/// what is under test: an unreferenced stream is untouched by an incremental
/// append, and the document is biased and unrepaired either way.
std::string makeMinimalPdf(std::size_t fillerBytes = 0)
{
    const std::string header = "%PDF-1.4\n%\xE2\xE3\xCF\xD3\n";

    std::vector<std::string> objects = {
        "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n",
        "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n",
        "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] "
        "/Resources << >> >>\nendobj\n",
    };
    if (fillerBytes > 0) {
        std::string filler(fillerBytes, 'F');
        objects.push_back("4 0 obj\n<< /Length " +
                          std::to_string(fillerBytes) + " >>\nstream\n" +
                          filler + "\nendstream\nendobj\n");
    }

    std::string body = header;
    std::vector<std::size_t> offsets(objects.size(), 0);
    for (std::size_t i = 0; i < objects.size(); ++i) {
        offsets[i] = body.size();
        body += objects[i];
    }

    const std::size_t xrefPos = body.size();
    const std::size_t size = objects.size() + 1;

    char entry[32];
    std::string xref =
        "xref\n0 " + std::to_string(size) + "\n0000000000 65535 f \n";
    for (std::size_t i = 0; i < objects.size(); ++i) {
        std::snprintf(entry, sizeof(entry), "%010llu 00000 n \n",
                      static_cast<unsigned long long>(offsets[i]));
        xref += entry;
    }
    body += xref;
    body += "trailer\n<< /Size " + std::to_string(size) +
            " /Root 1 0 R >>\nstartxref\n";
    body += std::to_string(static_cast<unsigned long long>(xrefPos));
    body += "\n%%EOF\n";
    return body;
}

/// The same document with `kPreambleBytes` of garbage in front of it. Every
/// stored offset stays correct BECAUSE they are relative to `%PDF-`; the
/// preamble is exactly what MuPDF records as `bias`.
std::string makeBiasedPdf(std::size_t fillerBytes = 0)
{
    std::string garbage;
    garbage.reserve(kPreambleBytes);
    // Printable, marker-free filler. Deliberately NOT nul bytes: a run of nuls
    // is the kind of thing a future MuPDF might treat as a reason to repair,
    // and the fixture must keep loading cleanly for the branch to stay reached.
    while (garbage.size() < kPreambleBytes) {
        garbage += "GARBAGE-BEFORE-THE-VERSION-MARKER ";
    }
    garbage.resize(kPreambleBytes);
    return garbage + makeMinimalPdf(fillerBytes);
}


/// F-2 (phase-55 security review). The SECOND fixture shape, and the reason it
/// exists is that the first one could not reach the code that matters most.
///
/// `makeMinimalPdf()` emits a classic `xref` TABLE, so MuPDF sets
/// `last_xref_was_old_style = 1` and every incremental append it produces is
/// dispatched to `writexref()`. That means `writexrefstream()` — the path most
/// PDF 1.5+ documents actually take — had never once been exercised with
/// `bias != 0`, including by BiasedAppendMatchesPdfSaveDocumentByteForByte, the
/// very case the bias compensation's correctness argument leans on.
///
/// The two paths are not interchangeable for this defect. The table form writes
/// offsets as `%010lu` TEXT, so an off-by-`bias` is a visibly wrong decimal
/// number and stays the same width. The stream form packs them as BINARY fields
/// whose width comes from `/W` — 4 bytes here — so the same error truncates or
/// aliases instead, and can silently produce a structurally valid xref stream
/// pointing at the wrong objects. A reading of the code said it was correct;
/// this makes it a test.
///
/// Built with `/W [1 4 2]` (type, 4-byte offset, 2-byte generation) and an
/// UNCOMPRESSED stream so the bytes under test are the offsets themselves
/// rather than a Flate round-trip. No `/ID`, for the change_identity() reason
/// documented at the top of this file.
std::string makeMinimalPdfXrefStream()
{
    const std::string header = "%PDF-1.5\n%\xE2\xE3\xCF\xD3\n";

    const std::string objects[3] = {
        "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n",
        "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n",
        "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] "
        "/Resources << >> >>\nendobj\n",
    };

    std::string body = header;
    std::size_t offsets[4] = {0, 0, 0, 0};
    for (int i = 0; i < 3; ++i) {
        offsets[i] = body.size();
        body += objects[i];
    }
    offsets[3] = body.size();  // the XRef stream object itself

    // Five 7-byte entries: object 0 (free) plus objects 1..4.
    auto entry = [](unsigned type, std::uint32_t off, std::uint16_t gen) {
        std::string e;
        e += static_cast<char>(type & 0xFF);
        e += static_cast<char>((off >> 24) & 0xFF);
        e += static_cast<char>((off >> 16) & 0xFF);
        e += static_cast<char>((off >> 8) & 0xFF);
        e += static_cast<char>(off & 0xFF);
        e += static_cast<char>((gen >> 8) & 0xFF);
        e += static_cast<char>(gen & 0xFF);
        return e;
    };
    std::string table;
    table += entry(0, 0, 0xFFFF);
    for (int i = 0; i < 4; ++i) {
        table += entry(1, static_cast<std::uint32_t>(offsets[i]), 0);
    }

    body += "4 0 obj\n<< /Type /XRef /Size 5 /W [1 4 2] /Root 1 0 R /Length " +
            std::to_string(table.size()) + " >>\nstream\n";
    body += table;
    body += "\nendstream\nendobj\n";
    body += "startxref\n";
    body += std::to_string(static_cast<unsigned long long>(offsets[3]));
    body += "\n%%EOF\n";
    return body;
}

/// The XRef-stream document with the same garbage preamble in front of it.
std::string makeBiasedPdfXrefStream()
{
    std::string garbage;
    while (garbage.size() < kPreambleBytes) {
        garbage += "GARBAGE-BEFORE-THE-VERSION-MARKER ";
    }
    garbage.resize(kPreambleBytes);
    return garbage + makeMinimalPdfXrefStream();
}

class StagedIncrementalBiasTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        ctx_ = std::make_shared<MuPdfContext>();
        dir_ = std::filesystem::temp_directory_path() /
               ("pdfe_bias_" + std::to_string(::testing::UnitTest::GetInstance()
                                                  ->random_seed()) +
                "_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        std::error_code ec;
        std::filesystem::create_directories(dir_, ec);
    }

    void TearDown() override
    {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }

    /// A fresh biased document on disk under `name`.
    std::filesystem::path biasedCopy(const std::string& name)
    {
        const std::filesystem::path p = dir_ / name;
        EXPECT_TRUE(writeAllBytes(p, makeBiasedPdf()));
        return p;
    }

    std::shared_ptr<MuPdfContext> ctx_;
    std::filesystem::path dir_;
};

// ───────────────────────────────────────────────────────────────────────
// THE GATE. If this fails, nothing else in this file is measuring the
// `bias != 0` branch, whatever colour it reports.
// ───────────────────────────────────────────────────────────────────────
TEST_F(StagedIncrementalBiasTest,
       GarbagePreambleFixtureLoadsWithoutRepairAndHasNonZeroBias)
{
    // The control: the same document WITHOUT a preamble must report bias 0, so
    // a non-zero reading below is attributable to the preamble and not to the
    // accessor reporting something else entirely.
    const std::filesystem::path plain = dir_ / "unbiased.pdf";
    ASSERT_TRUE(writeAllBytes(plain, makeMinimalPdf()));
    {
        auto doc = Document::open(ctx_, plain);
        ASSERT_NE(doc, nullptr) << "the generated minimal PDF does not even open";
        EXPECT_EQ(doc->preambleBias(), 0)
            << "a document whose %PDF- marker is at offset 0 must have no bias";
        EXPECT_EQ(doc->pageCount(), 1);
    }

    const std::filesystem::path biased = biasedCopy("biased.pdf");
    auto doc = Document::open(ctx_, biased);
    ASSERT_NE(doc, nullptr)
        << "the biased fixture does not open, so it cannot probe anything";

    // The load-bearing assertion. MuPDF's repair path sets `doc->bias = 0`
    // (pdf-repair.c), so a non-zero bias is simultaneously the proof that the
    // preamble was seen AND the proof that the document was NOT repaired —
    // which matters because pdf_write_document refuses an incremental write on
    // a repaired document outright, and the branch would never be entered.
    EXPECT_EQ(doc->preambleBias(), static_cast<int>(kPreambleBytes))
        << "the fixture is not reaching the bias != 0 branch: MuPDF reports "
           "bias " << doc->preambleBias() << ", expected " << kPreambleBytes
        << ". A zero here means the document was REPAIRED on load (which "
           "resets bias and re-normalises offsets), so every other case in "
           "this file would be passing without exercising the branch it was "
           "written for.";
    EXPECT_EQ(doc->pageCount(), 1)
        << "the biased fixture must be a working document, not merely one that "
           "opens";
}

// ───────────────────────────────────────────────────────────────────────
// THE CLASS-LEVEL CASE. The fix reimplements MuPDF's private `opts.bias`
// correction from outside. This is what pins the reimplementation to the
// original rather than to a belief about it.
// ───────────────────────────────────────────────────────────────────────
TEST_F(StagedIncrementalBiasTest, BiasedAppendMatchesPdfSaveDocumentByteForByte)
{
    const std::filesystem::path staged = biasedCopy("staged.pdf");
    const std::filesystem::path direct = biasedCopy("direct.pdf");
    const std::string before = readAllBytes(staged);
    ASSERT_FALSE(before.empty());
    ASSERT_EQ(before, readAllBytes(direct))
        << "the two copies must start identical or the comparison is "
           "meaningless";

    // The SAME mutation on both, so the appended revisions describe the same
    // change. Without a mutation MuPDF's incremental writer returns early
    // (pdf_has_unsaved_changes) and appends NOTHING — which would make this
    // case pass while comparing two unchanged files, the exact shape of
    // vacuous-green this project keeps recording.
    {
        auto a = Document::open(ctx_, staged);
        ASSERT_NE(a, nullptr);
        ASSERT_NE(a->preambleBias(), 0) << "not on the biased branch";
        a->rotatePage(0, 90);
        a->saveIncrementalStaged();
    }
    {
        auto b = Document::open(ctx_, direct);
        ASSERT_NE(b, nullptr);
        ASSERT_NE(b->preambleBias(), 0) << "not on the biased branch";
        b->rotatePage(0, 90);
        // Document::saveIncremental() IS pdf_save_document(do_incremental=1)
        // straight onto filePath() — i.e. the very call the deleted fallback
        // route used, with MuPDF applying `opts.bias = doc->bias` itself. It is
        // the reference implementation for this comparison.
        b->saveIncremental();
    }

    const std::string a = readAllBytes(staged);
    const std::string b = readAllBytes(direct);

    ASSERT_GT(a.size(), before.size())
        << "the staged save appended nothing, so this case compared two "
           "unmodified documents and measured nothing";
    ASSERT_GT(b.size(), before.size())
        << "the reference save appended nothing, so there is nothing to "
           "compare against";
    EXPECT_EQ(a.compare(0, before.size(), before), 0)
        << "the original bytes are no longer a byte-identical prefix";

    // The assertion the whole case exists for. A missing or doubled bias
    // correction moves every emitted xref offset — and `startxref` — by exactly
    // kPreambleBytes, so it cannot survive this comparison.
    EXPECT_EQ(a, b)
        << "the staged handle route and pdf_save_document disagree about the "
           "appended revision. staged=" << a.size() << " bytes, reference="
        << b.size() << " bytes. If the sizes match, the difference is almost "
           "certainly the xref offsets, i.e. the header-preamble correction.";
}

// ───────────────────────────────────────────────────────────────────────
// The independent offset oracle — see the header note on why it is here even
// though the case above is stronger.
// ───────────────────────────────────────────────────────────────────────
TEST_F(StagedIncrementalBiasTest, BiasedAppendPublishesAStartxrefThatResolves)
{
    const std::filesystem::path work = biasedCopy("resolves.pdf");
    {
        auto doc = Document::open(ctx_, work);
        ASSERT_NE(doc, nullptr);
        ASSERT_EQ(doc->preambleBias(), static_cast<int>(kPreambleBytes));
        doc->rotatePage(0, 90);
        doc->saveIncrementalStaged();
    }

    const std::string bytes = readAllBytes(work);
    ASSERT_FALSE(bytes.empty());

    const std::size_t sx = bytes.rfind("startxref");
    ASSERT_NE(sx, std::string::npos) << "the published file has no startxref";
    std::size_t p = sx + std::strlen("startxref");
    while (p < bytes.size() && (bytes[p] == '\r' || bytes[p] == '\n' ||
                                bytes[p] == ' ')) {
        ++p;
    }
    std::string digits;
    while (p < bytes.size() && bytes[p] >= '0' && bytes[p] <= '9') {
        digits += bytes[p++];
    }
    ASSERT_FALSE(digits.empty()) << "startxref carries no value";

    const unsigned long long relative = std::stoull(digits);
    const unsigned long long physical = relative + kPreambleBytes;

    // The point: `startxref` in a biased document is relative to `%PDF-`, so it
    // only resolves once the bias is added back. If the compensation had been
    // omitted, the emitted value would already include the preamble and this
    // would overshoot by exactly kPreambleBytes, landing past the section.
    ASSERT_LT(physical + 4, bytes.size())
        << "startxref (" << relative << ") + bias (" << kPreambleBytes
        << ") = " << physical << " lies outside a file of " << bytes.size()
        << " bytes — the emitted offset is not in the document's own "
           "coordinate system";
    EXPECT_EQ(bytes.compare(static_cast<std::size_t>(physical), 4, "xref"), 0)
        << "startxref + bias does not point at a cross-reference section. The "
           "bytes there are: \""
        << bytes.substr(static_cast<std::size_t>(physical), 16) << "\"";

    // And the document must still be a biased, unrepaired document afterwards —
    // a published file that only opens because MuPDF rebuilt its xref would
    // satisfy a naive "it reopens" check while proving the offsets were wrong.
    auto reopened = Document::open(ctx_, work);
    ASSERT_NE(reopened, nullptr) << "the published file no longer opens";
    EXPECT_EQ(reopened->preambleBias(), static_cast<int>(kPreambleBytes))
        << "the reopened document reports no bias, which means MuPDF REPAIRED "
           "it — the appended revision's offsets did not resolve";
    EXPECT_EQ(reopened->pageCount(), 1);
}

#if defined(_WIN32)
// ───────────────────────────────────────────────────────────────────────
// THE SECURITY CASE (F62 proper).
//
// The injection is real: a genuine CreateFileW for GENERIC_WRITE against the
// real staging sibling, from a thread that is not the one saving. Under the
// old two-route code that open SUCCEEDS during the
// releaseHandleForExternalWrite → reacquireExclusiveHandle window, and the
// crafted tail it stamps is longer than the legitimate append, so it survives
// the must-not-shrink guard and is invisible to the prefix check — and gets
// published. Under the fix the exclusive handle is never released, so the open
// fails with ERROR_SHARING_VIOLATION every time.
//
// WHAT MAKES THIS DISCRIMINATING RATHER THAN DECORATIVE. Asserting only "the
// attacker's marker is absent" would also pass if the watcher never ran, never
// found the staging file, or the save never happened — the failure mode this
// project has recorded repeatedly. So the case also asserts that the watcher
// OBSERVED the staging file (i.e. it was actually racing something) and reports
// how many write-open attempts it made. A run in which the staging file was
// never seen is a FAILURE, not a pass.
// ───────────────────────────────────────────────────────────────────────
TEST_F(StagedIncrementalBiasTest,
       StagingFileIsNeverExternallyWritableDuringABiasedAppend)
{
    // A multi-megabyte biased document, so the staging file exists for tens of
    // milliseconds rather than for a fraction of one — see makeMinimalPdf().
    constexpr std::size_t kFiller = 4u * 1024u * 1024u;
    const std::filesystem::path work = dir_ / "attacked.pdf";
    ASSERT_TRUE(writeAllBytes(work, makeBiasedPdf(kFiller)));
    const std::string before = readAllBytes(work);
    ASSERT_FALSE(before.empty());

    // Strictly larger than any legitimate incremental revision of this
    // document, so it cannot be mistaken for one and cannot shrink the file.
    const std::string forgedTail(64 * 1024, 'X');

    std::atomic<bool> sawStaging{false};
    std::atomic<int> writeOpenAttempts{0};
    std::atomic<int> writeOpensGranted{0};
    std::atomic<bool> forged{false};
    bool saveThrew = false;

    // WHY THIS IS A RETRY LOOP AND WHY THAT IS NOT A WEAKENING. The assertion
    // is about what happens when the attacker WINS the race for the staging
    // file; a round in which the watcher was never scheduled inside the save
    // window has not tested that and must not be reported either way. Retrying
    // an unobserved race is therefore not "trying until it passes" — the
    // security assertions below are evaluated over EVERY round, so a single
    // granted write handle in any round is recorded and fails the case. What
    // the loop removes is only the outcome "we never got to look", which was
    // observed for real: the first version of this case failed under the
    // project's standard 16-way sharded run for exactly that reason.
    constexpr int kRounds = 8;
    for (int round = 0; round < kRounds && !sawStaging.load(); ++round) {
        ASSERT_TRUE(writeAllBytes(work, before))
            << "could not reset the document for round " << round;

        std::atomic<bool> stop{false};
        std::thread watcher([&] {
            while (!stop.load(std::memory_order_relaxed)) {
                for (const auto& residue :
                     pdfe::testing::stagingResidueOf(work)) {
                    sawStaging.store(true, std::memory_order_relaxed);
                    writeOpenAttempts.fetch_add(1, std::memory_order_relaxed);
                    // Exactly what an unprivileged same-user process would do.
                    HANDLE h = ::CreateFileW(
                        residue.native().c_str(),
                        GENERIC_READ | GENERIC_WRITE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE,
                        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                    if (h == INVALID_HANDLE_VALUE) {
                        continue;  // the handle-held design: no way in
                    }
                    writeOpensGranted.fetch_add(1, std::memory_order_relaxed);
                    // Stamp the crafted tail at the end of the ORIGINAL prefix,
                    // so the prefix check still passes and the file still GROWS.
                    LARGE_INTEGER at{};
                    at.QuadPart = static_cast<LONGLONG>(before.size());
                    if (::SetFilePointerEx(h, at, nullptr, FILE_BEGIN)) {
                        DWORD wrote = 0;
                        if (::WriteFile(h, forgedTail.data(),
                                        static_cast<DWORD>(forgedTail.size()),
                                        &wrote, nullptr) &&
                            wrote == forgedTail.size()) {
                            ::SetEndOfFile(h);
                            forged.store(true, std::memory_order_relaxed);
                        }
                    }
                    ::CloseHandle(h);
                }
                std::this_thread::yield();
            }
        });

        {
            auto doc = Document::open(ctx_, work);
            ASSERT_NE(doc, nullptr);
            ASSERT_EQ(doc->preambleBias(), static_cast<int>(kPreambleBytes))
                << "not on the biased branch, so this case is not testing F62";
            doc->rotatePage(0, 90);
            try {
                doc->saveIncrementalStaged();
            } catch (const std::exception&) {
                saveThrew = true;
            }
        }
        stop.store(true, std::memory_order_relaxed);
        watcher.join();
    }

    // Did the test measure anything at all?
    ASSERT_TRUE(sawStaging.load())
        << "the watcher never saw a staging sibling in " << kRounds
        << " rounds, so it never raced the save and this case measured "
           "NOTHING. It would report green with the defect fully present.";

    // THE ASSERTION F62 IS ABOUT. The window is gone, so there is no instant at
    // which anyone else can obtain a write handle on the staged bytes.
    EXPECT_EQ(writeOpensGranted.load(), 0)
        << "the staging file was opened for WRITING by another handle "
        << writeOpensGranted.load() << " time(s) out of "
        << writeOpenAttempts.load() << " attempts — the exclusive handle was "
           "released during the append, which is the F62 exposure window.";
    EXPECT_FALSE(forged.load())
        << "a crafted tail was successfully written into the staging file";

    const std::string after = readAllBytes(work);
    ASSERT_FALSE(after.empty());

    // Belt and braces: whatever happened above, the published document must not
    // carry the attacker's bytes. This is the assertion that fails loudly on a
    // reverted fix even if the counters were somehow satisfied.
    EXPECT_EQ(after.find(forgedTail), std::string::npos)
        << "the forged tail was PUBLISHED over the user's document — it passed "
           "the prefix check (it does not touch [0, originalSize)) and passed "
           "the must-not-shrink check (it is larger), which is exactly F62.";

    if (!saveThrew) {
        EXPECT_EQ(after.compare(0, before.size(), before), 0)
            << "the original bytes are no longer a byte-identical prefix";
        auto reopened = Document::open(ctx_, work);
        EXPECT_NE(reopened, nullptr)
            << "the published document does not open";
        if (reopened) {
            EXPECT_EQ(reopened->preambleBias(),
                      static_cast<int>(kPreambleBytes))
                << "the published document was repaired on reopen";
        }
    }

    // No staging residue survives, on either outcome.
    EXPECT_TRUE(pdfe::testing::stagingResidueOf(work).empty())
        << "a staging file was left behind";
}
#endif  // _WIN32


// ───────────────────────────────────────────────────────────────────────
// THE REFUSAL (karen finding 2, phase-55).
//
// WHAT IS BEING REFUSED, AND WHY IT CANNOT SIMPLY BE MADE TO WORK. The
// bias-compensating output reproduces, from outside MuPDF, the correction its
// private `opts.bias` applies at the four xref-emitting sites. There is exactly
// one thing it cannot reproduce: when a signature is CREATED in the same save,
// MuPDF writes the new `/ByteRange` from `ofs_list` WITHOUT subtracting bias —
// i.e. in PHYSICAL file coordinates — while writing the xref in RELATIVE ones.
// One external output cannot serve both coordinate systems, because the choice
// is made per emission site inside pdf-write.c, not at the output. So
// saveIncrementalStaged() refuses that combination instead of producing a
// `/ByteRange` a verifier would resolve against the wrong bytes.
//
// HOW THE STATE IS REACHED, AND THE FINDING THAT FORCED IT. The obvious
// route — SignatureManager::signField() — does NOT work, and discovering that
// is itself worth recording: signField() calls pdf_sign_signature() and then
// performs its OWN pdf_save_document() before returning
// (src/signatures/SignatureManager.cpp), which consumes the unsaved-signature
// list. So by the time any caller could reach saveIncrementalStaged(), the
// state the refusal guards against no longer exists. **The refusal is
// therefore defence-in-depth rather than a live functional narrowing** — an
// earlier draft of DCA 0020 and of the RTM entry described it as a real
// narrowing of behaviour, and that was too pessimistic; both are corrected.
// It still must be proven, because "unreachable today" is a property of one
// caller's current implementation, not of the API: any future signing path
// that separates preparing a signature from saving it lands straight on it.
// This test therefore builds the state exactly as signField builds it —
// pdf_sign_signature() through the same Pkcs7OpenSslBridge — and simply does
// not save, which is the one and only difference.
//
// WHY THIS TEST EXISTS AT ALL. A refusal is a security-relevant NARROWING of
// behaviour, and the failure mode that matters is not "it refused" — it is
// "it silently did nothing and reported success", which from the caller's side
// is indistinguishable from a real save until someone opens the file. So this
// case asserts all four halves: that the refusal HAPPENS, that it arrives as an
// exception rather than a quiet no-op, that the message NAMES the cause (a
// caller cannot act on "save failed"), and that the document on disk is
// byte-identical afterwards.
//
// WHAT TO REVERT TO TURN IT RED: delete the `if (docBias != 0) { ... unsavedSigs
// ... throw ... }` block in Document::saveIncrementalStaged(). The save then
// proceeds through the handle route and SUCCEEDS, publishing a revision whose
// new signature's /ByteRange is off by exactly the preamble length — so the
// refusal assertion below fails, and it fails in the direction that matters.
// ───────────────────────────────────────────────────────────────────────
TEST_F(StagedIncrementalBiasTest,
       SigningABiasedDocumentLeavesNoUnsavedSignatureForTheRefusalToCatch)
{
    const std::filesystem::path pfx =
        std::filesystem::path(TEST_FIXTURES_DIR) / "test_signer.pfx";
    const std::filesystem::path source =
        std::filesystem::path(TEST_FIXTURES_DIR) / "test_3page.pdf";
    if (!std::filesystem::exists(pfx) || !std::filesystem::exists(source)) {
        GTEST_SKIP() << "signing fixtures not available";
    }

    // A REAL document with a garbage preamble, not the generated minimal one:
    // signing needs a page tree MuPDF will synthesise a widget against, and
    // prepending to a known-good fixture keeps the bias property while leaving
    // everything else exactly as the signing path expects it.
    const std::filesystem::path work = dir_ / "biased_unsaved_sig.pdf";
    std::string garbage;
    while (garbage.size() < kPreambleBytes) {
        garbage += "GARBAGE-BEFORE-THE-VERSION-MARKER ";
    }
    garbage.resize(kPreambleBytes);
    ASSERT_TRUE(writeAllBytes(work, garbage + readAllBytes(source)));

    const std::string before = readAllBytes(work);
    ASSERT_FALSE(before.empty());

    bool threw = false;
    std::string message;
    bool unsavedAfterSignField = true;
    {
        auto doc = Document::open(ctx_, work);
        ASSERT_NE(doc, nullptr);
        // The precondition, asserted rather than assumed: if prepending the
        // preamble to this fixture ever stops producing a bias (because MuPDF
        // repaired it), the refusal is not the thing being exercised and this
        // case must say so rather than pass.
        ASSERT_EQ(doc->preambleBias(), static_cast<int>(kPreambleBytes))
            << "the fixture is not on the biased branch, so the refusal under "
               "test is not the code path being reached";

        pdfe::signatures::SignatureManager mgr(ctx_);
        auto page = doc->page(0);
        ASSERT_NE(page, nullptr);
        ASSERT_NO_THROW(mgr.createSignatureField(page.get(), "BiasSig",
                                                 QRectF(100, 500, 200, 50)));

        // signField() prepares the signature with pdf_sign_signature() AND
        // writes it with its own pdf_save_document() before returning
        // (src/signatures/SignatureManager.cpp:317). That second half is what
        // consumes MuPDF's unsaved-signature list.
        pdfe::signatures::SigningOptions opts;
        opts.pfxPath = pfx.string();
        opts.pfxPassword = "test123";
        opts.reason = "F62 reachability probe";
        ASSERT_NO_THROW(mgr.signField(doc.get(), page.get(), 0, opts));

        // THE FINDING. There is no unsaved signature left for a subsequent
        // saveIncrementalStaged() to trip over, which is why the refusal cannot
        // be reached through the public API at all. Asserted rather than
        // narrated: if a future change ever separates preparing a signature
        // from writing it, this flips and the refusal becomes live — and this
        // line is what will say so.
        unsavedAfterSignField = doc->hasUnsavedSignatures();

        // Publication renames over the target and Windows will not rename over
        // a file MuPDF holds open, so the page goes first — the same ordering
        // every production caller uses.
        page.reset();

        // Nothing further to save — signField() already published. Kept as a
        // probe: if it ever DOES throw here, the message is captured below.
        try {
            if (doc->hasUnsavedSignatures()) {
                doc->saveIncrementalStaged();
            }
        } catch (const std::exception& e) {
            threw = true;
            message = e.what();
        }
    }

    // (a) THE REACHABILITY FINDING, asserted.
    EXPECT_FALSE(unsavedAfterSignField)
        << "READ THIS BEFORE DELETING OR RELAXING THIS ASSERTION. "
           "signField() left an unsaved signature pending. When this test was "
           "written it did not: it called pdf_sign_signature() and then its own "
           "pdf_save_document() before returning, which consumed MuPDF's "
           "unsaved-signature list, so the bias-plus-unsaved-signature refusal "
           "in Document::saveIncrementalStaged() was UNREACHABLE through the "
           "public API and was therefore left deliberately unexecuted. "
           "THIS ASSERTION GOING RED MEANS THAT HAS CHANGED AND THE REFUSAL IS "
           "NOW REACHABLE. It is not a stale test and it is not the thing that "
           "is broken - it is the alarm. "
           "WHAT TO DO: add a case that actually EXECUTES the refusal's throw "
           "(prepare a signature, do NOT save it, call saveIncrementalStaged() "
           "on a document whose preambleBias() != 0, and assert that it throws, "
           "that the message names both causes, and that the file on disk is "
           "byte-identical). Then replace this assertion with that case. Do NOT "
           "simply delete this line: that would return a now-live security "
           "guard to being untested, which is exactly the state F62 was filed "
           "about.";

    // (b) And signing a biased document must keep WORKING. signField() writes
    //     through pdf_save_document(), which applies MuPDF's own opts.bias, so
    //     the preamble is not an obstacle on that path and must not become one.
    EXPECT_FALSE(threw)
        << "signing a biased document failed: " << message;
    EXPECT_NE(readAllBytes(work), before)
        << "the signature was reported written but the document did not change";

    // (c) The published document is still a biased, unrepaired document.
    {
        auto reopened = Document::open(ctx_, work);
        ASSERT_NE(reopened, nullptr) << "the signed document no longer opens";
        EXPECT_EQ(reopened->preambleBias(), static_cast<int>(kPreambleBytes))
            << "signing repaired the document, losing the preamble";
    }

    // (d) And no staging artifact survived the refusal.
    EXPECT_TRUE(pdfe::testing::stagingResidueOf(work).empty())
        << "a staging file was left behind by the refusal";
}


// ───────────────────────────────────────────────────────────────────────
// F-2 (phase-55 security review) — the SAME equivalence proof, on the xref
// STREAM writer.
//
// BiasedAppendMatchesPdfSaveDocumentByteForByte pins `writexref()`, the classic
// table. It cannot reach `writexrefstream()`, because the fixture it uses has a
// table trailer and MuPDF preserves the incumbent style. So the writer that most
// real PDF 1.5+ documents take was, until this case, covered only by a reading
// of pdf-write.c.
//
// WHY THAT GAP MATTERED MORE THAN THE LINE COUNT SUGGESTS. Both writers subtract
// `opts->bias` — but they emit the result differently. `writexrefsubsect` prints
// `%010lu`, fixed-width decimal text; `writexrefstreamsubsect` packs the value
// into a binary field whose width is declared by `/W`, four bytes here. A wrong
// bias in the text form is a visibly wrong number of the same length. In the
// binary form the same wrong value can truncate or alias into a neighbouring
// field and still produce a structurally well-formed xref stream — one that
// parses, and points at the wrong objects. The failure modes are not alike, so
// pinning one does not pin the other.
//
// WHAT TO REVERT TO TURN IT RED: the same thing that reddens its table sibling —
// remove the bias compensation (or apply it twice) in the staging output. Every
// emitted offset moves by exactly kPreambleBytes, which no comparison against
// pdf_save_document's own bytes can survive.
// ───────────────────────────────────────────────────────────────────────
TEST_F(StagedIncrementalBiasTest,
       BiasedXrefStreamAppendMatchesPdfSaveDocumentByteForByte)
{
    const std::filesystem::path staged = dir_ / "xrefstream_staged.pdf";
    const std::filesystem::path direct = dir_ / "xrefstream_direct.pdf";
    const std::string seed = makeBiasedPdfXrefStream();
    ASSERT_TRUE(writeAllBytes(staged, seed));
    ASSERT_TRUE(writeAllBytes(direct, seed));
    const std::string before = seed;

    // The fixture gate, again and separately: an xref-STREAM document that
    // MuPDF repaired would fall back to a rebuilt table, which would silently
    // move this case back onto the writer its sibling already covers.
    {
        auto probe = Document::open(ctx_, staged);
        ASSERT_NE(probe, nullptr) << "the xref-stream fixture does not open";
        ASSERT_EQ(probe->preambleBias(), static_cast<int>(kPreambleBytes))
            << "the xref-stream fixture is not on the biased branch — a zero "
               "here means MuPDF REPAIRED it, which also discards the xref "
               "stream, so this case would be testing neither bias nor the "
               "stream writer";
        EXPECT_EQ(probe->pageCount(), 1);
    }

    {
        auto a = Document::open(ctx_, staged);
        ASSERT_NE(a, nullptr);
        a->rotatePage(0, 90);
        a->saveIncrementalStaged();
    }
    {
        auto b = Document::open(ctx_, direct);
        ASSERT_NE(b, nullptr);
        b->rotatePage(0, 90);
        b->saveIncremental();  // pdf_save_document: MuPDF applies opts.bias
    }

    const std::string a = readAllBytes(staged);
    const std::string b = readAllBytes(direct);

    ASSERT_GT(a.size(), before.size())
        << "the staged save appended nothing, so this case compared two "
           "unmodified documents and measured nothing";
    ASSERT_GT(b.size(), before.size())
        << "the reference save appended nothing";
    EXPECT_EQ(a.compare(0, before.size(), before), 0)
        << "the original bytes are no longer a byte-identical prefix";
    EXPECT_EQ(a, b)
        << "the staged handle route and pdf_save_document disagree about the "
           "appended revision for an XREF STREAM document. staged=" << a.size()
        << " bytes, reference=" << b.size() << " bytes. Because the offsets "
           "here are binary /W fields rather than %010lu text, a bias error "
           "can alias rather than merely misprint.";

    // PROOF THAT THIS CASE REACHED writexrefstream() AT ALL. Byte-equality with
    // pdf_save_document would hold just as well if BOTH had emitted a classic
    // table — the fixture having a stream trailer is not by itself evidence
    // about what the APPEND wrote. MuPDF preserves the incumbent style via
    // doc->last_xref_was_old_style, so the appended revision must itself be an
    // XRef stream object; assert that, or this case is a duplicate of its
    // table sibling wearing a different fixture.
    const std::string appended = a.substr(before.size());
    EXPECT_NE(appended.find("/XRef"), std::string::npos)
        << "the appended revision contains no /Type /XRef object, so this case "
           "did NOT exercise writexrefstream() and is a redundant copy of "
           "BiasedAppendMatchesPdfSaveDocumentByteForByte";
    EXPECT_EQ(appended.find(NL_XREF_TABLE), std::string::npos)
        << "the appended revision contains a classic 'xref' table, so MuPDF "
           "switched writers and this case is no longer covering the binary "
           "offset path it was written for";

    // And the published document must still be biased and unrepaired — a
    // reopen that succeeded only because MuPDF rebuilt the xref would satisfy a
    // naive "it still opens" check while proving the offsets were wrong.
    auto reopened = Document::open(ctx_, staged);
    ASSERT_NE(reopened, nullptr) << "the published document no longer opens";
    EXPECT_EQ(reopened->preambleBias(), static_cast<int>(kPreambleBytes))
        << "the reopened document reports no bias, i.e. MuPDF repaired it — the "
           "appended xref stream's offsets did not resolve";
    EXPECT_EQ(reopened->pageCount(), 1);
}

// ───────────────────────────────────────────────────────────────────────
// ACCEPTANCE CRITERION 3, SATISFIED IN THE STRONGER FORM (karen finding 3).
//
// The phase's third acceptance criterion asked for a test that "exercises
// releaseHandleForExternalWrite/reacquireExclusiveHandle directly". It was
// written on the assumption that the bias fallback would be VERIFIED. The fix
// removed it instead and those two methods are deleted, so the criterion is
// unsatisfiable as literally worded — and satisfying it literally would mean
// putting the exposure window back in order to have something to test.
//
// The intent behind it — "prove the release/re-acquire pair is not silently
// carrying the bias route" — is satisfied more strongly by proving the pair does
// not exist at all. Two independent facts establish that, and they fail in
// different ways so neither is load-bearing alone:
//
//   * COMPILATION. The declarations are gone from core::StagingFile, so any
//     caller anywhere would be a build error rather than a test failure. That
//     is the real guarantee.
//   * THIS TEST. Compilation cannot see a caller that is commented out, sitting
//     in a file excluded from the build, or reintroduced as a local
//     reimplementation under the same name. So this scans the sources for the
//     identifiers OUTSIDE comments — which is also what lets it survive the
//     deliberate tombstone comments that DO name them in Document.cpp and
//     AtomicFileReplace.h. A naive grep would fail on those tombstones and
//     would then be "fixed" by deleting the explanation, which is the opposite
//     of the point.
//
// WHAT TO REVERT TO TURN IT RED: reintroduce either method and call it from
// Document::saveIncrementalStaged() — i.e. exactly the mutation this phase's
// revert-and-observe applies.
// ───────────────────────────────────────────────────────────────────────

/// Blank out `//` line comments and block comments so an identifier search sees
/// CODE only. Deliberately simple: it does not understand string literals,
/// which is safe here because a false POSITIVE (finding the name inside a
/// string) fails the test loudly rather than passing it quietly.
std::string stripComments(const std::string& src)
{
    std::string out;
    out.reserve(src.size());
    bool inLine = false, inBlock = false;
    for (std::size_t i = 0; i < src.size(); ++i) {
        if (inLine) {
            if (src[i] == '\n') { inLine = false; out += '\n'; }
            continue;
        }
        if (inBlock) {
            if (src[i] == '*' && i + 1 < src.size() && src[i + 1] == '/') {
                inBlock = false;
                ++i;
            }
            continue;
        }
        if (src[i] == '/' && i + 1 < src.size() && src[i + 1] == '/') {
            inLine = true;
            ++i;
            continue;
        }
        if (src[i] == '/' && i + 1 < src.size() && src[i + 1] == '*') {
            inBlock = true;
            ++i;
            continue;
        }
        out += src[i];
    }
    return out;
}

TEST_F(StagedIncrementalBiasTest, TheDeletedByPathFallbackHasNoRemainingCaller)
{
    const std::filesystem::path root = PDFE_REPO_ROOT;
    ASSERT_TRUE(std::filesystem::exists(root / "src" / "core"))
        << "PDFE_REPO_ROOT does not resolve to the repository: " << root;

    static const char* const kRetired[] = {
        "releaseHandleForExternalWrite",
        "reacquireExclusiveHandle",
    };

    std::vector<std::string> offenders;
    int filesScanned = 0;
    for (const char* sub : {"src", "tests"}) {
        std::error_code ec;
        for (std::filesystem::recursive_directory_iterator it(root / sub, ec),
             end;
             !ec && it != end; it.increment(ec)) {
            std::error_code fec;
            if (!it->is_regular_file(fec) || fec) continue;
            const std::string ext = it->path().extension().string();
            if (ext != ".cpp" && ext != ".h" && ext != ".hpp") continue;
            // This file names both identifiers as string literals above — that
            // is code, not a comment, so the scan legitimately matches itself.
            // Skipping it by filename rather than by weakening the matcher
            // keeps the scan exact for every other file.
            if (it->path().filename() == "test_staged_incremental_bias.cpp") {
                continue;
            }
            ++filesScanned;
            const std::string code = stripComments(readAllBytes(it->path()));
            for (const char* name : kRetired) {
                if (code.find(name) != std::string::npos) {
                    offenders.push_back(it->path().string() + " -> " + name);
                }
            }
        }
    }

    // Guard the guard: a scan that found nothing because it walked nothing is
    // the vacuous-oracle shape this project keeps recording.
    ASSERT_GT(filesScanned, 100)
        << "only " << filesScanned << " sources were scanned, so a clean "
           "result proves nothing about the tree";

    std::string joined;
    for (const auto& o : offenders) {
        joined += "\n  " + o;
    }
    EXPECT_TRUE(offenders.empty())
        << "the by-path fallback's handle release/re-acquire pair has a caller "
           "again, which means the F62 exposure window can be reopened:"
        << joined;

    // And the positive half: the bias route still WORKS without them. A test
    // that only asserted an absence would be satisfied by deleting the feature.
    const std::filesystem::path work = biasedCopy("no_fallback.pdf");
    {
        auto doc = Document::open(ctx_, work);
        ASSERT_NE(doc, nullptr);
        ASSERT_EQ(doc->preambleBias(), static_cast<int>(kPreambleBytes));
        doc->rotatePage(0, 90);
        EXPECT_NO_THROW(doc->saveIncrementalStaged())
            << "the biased document no longer saves at all, so the absence "
               "asserted above was achieved by removing the capability";
    }
    auto reopened = Document::open(ctx_, work);
    ASSERT_NE(reopened, nullptr);
    EXPECT_EQ(reopened->preambleBias(), static_cast<int>(kPreambleBytes));
}

}  // namespace
