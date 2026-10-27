// -*- coding: utf-8 -*-
//
// WIN-87 phase-58 — hostile-filesystem cases for the LAST TWO unhardened
// publish paths: site #6 (ImageExporter::writeImageAtomic) and site #7
// (BatchEngine::processFile).
//
// THE CLASS, RESTATED SO THESE CASES ARE READABLE ON THEIR OWN. WIN-85 migrated
// seven ad-hoc remove()+rename() publish sites onto one shared helper. That
// helper takes PATHS. It closed the DATA-LOSS hazard at all seven and left the
// SUBSTITUTION hazard open at all seven: a caller holding no handle re-resolves
// the staging name at rename time, so anything that can win the race between the
// last write and the rename gets its bytes published as the user's file. Phases
// 56 and 57 fixed sites #1-#5; these are #6 and #7, and with them the class is
// closed (see tools/gate/check_win87_publish_shapes.py, which enumerates the
// remaining shapes over comment-stripped code rather than by grep phrasing).
//
// WHY A FREE-RUNNING ATTACKER IS THE RIGHT SHAPE HERE, WHERE SITES #4/#5 NEEDED
// DETERMINISTIC SEAMS. At those sites the artifact under attack was the USER'S
// OWN DOCUMENT, whose name is the thing being protected and therefore cannot be
// made unpredictable — a racing thread hits a microsecond window only by luck,
// and MEASURED it did not. Here the artifacts are PDFE's own staging files in a
// directory PDFE controls, so the property under test is exactly the one an
// enumerating attacker can observe: what names appear, and whether any of them
// can be opened for write, renamed over, or deleted. The attacker therefore
// enumerates rather than guessing a name, and asserts on the SET of names it
// saw — which also fails a "fix" that merely swapped one guessable name for
// another.
//
// EVERY CASE HAS A VACUITY GUARD. `saw > 0` is asserted, because a green run in
// which the attacker never observed any artifact is equally explained by "the
// operation never staged anything", and that is not evidence about anything.

#include <gtest/gtest.h>

#include "batch/BatchEngine.h"
#include "batch/BatchTypes.h"
#include "core/AtomicFileReplace.h"
#include "support/DirectoryAttacker.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "exports/ImageExporter.h"

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
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#ifndef TEST_FIXTURES_DIR
#define TEST_FIXTURES_DIR "."
#endif

namespace {

namespace fs = std::filesystem;

constexpr const char* kPayload = "PWNED-WIN87-PHASE58-0123456789ABCDEF";

fs::path fixtures() { return fs::path(TEST_FIXTURES_DIR); }

fs::path makeWorkDir(const std::string& tag)
{
    static std::atomic<int> counter{0};
    auto d = fs::temp_directory_path()
           / ("pdfe_win87_p58_" + tag + "_"
              + std::to_string(::GetCurrentProcessId()) + "_"
              + std::to_string(counter.fetch_add(1)));
    std::error_code ec;
    fs::remove_all(d, ec);
    fs::create_directories(d, ec);
    return d;
}

std::string readAll(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)),
                       std::istreambuf_iterator<char>());
}

/// True when `p` can be opened with DELETE access — i.e. nothing is protecting
/// it. Used as the control: if the attacker's technique does not work on a file
/// nothing is guarding, a clean result proves nothing.
bool canTakeDeleteAccess(const fs::path& p)
{
    HANDLE h = ::CreateFileW(p.native().c_str(), DELETE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                             nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    ::CloseHandle(h);
    return true;
}

/// A decoy file whose bytes are unmistakable. It does not need to be a valid
/// image or PDF: the assertion is that these bytes are NEVER published, and a
/// producer that published them would publish them whatever they are.
fs::path makeDecoy(const fs::path& dir)
{
    const fs::path p = dir / "decoy_source.bin";
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    for (int i = 0; i < 64; ++i) f << kPayload;
    return p;
}

// DirectoryAttacker now lives in tests/support/DirectoryAttacker.h (GitHub #275).
//
// It was defined HERE and in test_publish_handle_hardening_phase59.cpp, and the two copies
// had drifted four ways by the time they were merged -- including the attack
// PAYLOAD, which is why the shared version takes it as a parameter. The cost was
// measured: a poll-race fix applied to one copy left the other broken, all 20
// TOCTTOU cases passed, and a stability run under load then failed 9 of 12 in the
// copy nobody had touched. See the header for the full account.
using pdfe::testing::AttackPayload;
using pdfe::testing::DirectoryAttacker;

// ===========================================================================
// Site #6 — ImageExporter::writeImageAtomic
// ===========================================================================

class ImageExportPublishTocttou : public ::testing::Test {
protected:
    void SetUp() override
    {
        const auto src = fixtures() / "test_single.pdf";
        if (!fs::exists(src)) GTEST_SKIP() << "test_single.pdf missing";
        dir_ = makeWorkDir("imgexp");
        outDir_ = dir_ / "out";
        fs::create_directories(outDir_);
        decoy_ = makeDecoy(dir_);
        ctx_ = std::make_shared<pdfe::core::MuPdfContext>();
        doc_ = pdfe::core::Document::open(ctx_, src);
        ASSERT_TRUE(doc_);
    }
    void TearDown() override
    {
        doc_.reset();
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }
    fs::path dir_, outDir_, decoy_;
    std::shared_ptr<pdfe::core::MuPdfContext> ctx_;
    std::unique_ptr<pdfe::core::Document> doc_;
};

// The staging artifact used to be "<finalPath>.part" — a name computable by
// anyone who knows where the user is exporting. Stamping image bytes into it
// between QImage::save() closing it and the rename got them published as the
// user's export.
TEST_F(ImageExportPublishTocttou,
       PoisoningAnyStagedExportArtifactCannotSubstituteThePublishedImage)
{
    pdfe::exports::ImageExporter ex(doc_.get());
    pdfe::exports::ImageExportOptions opts;
    opts.format = pdfe::exports::ImageFormat::Png;
    opts.dpi = 72;  // fast; the property under test is not resolution

    DirectoryAttacker attacker;
    // Attack every artifact in the output directory EXCEPT the finished .png
    // files — see skipExt for why that exemption is the honest scope rather
    // than a loophole. No staging name is skipped, because after phase-58 the
    // test cannot know what the staging artifact is called, which is the
    // property under test rather than an inconvenience.
    attacker.start(outDir_, decoy_, /*skip=*/{}, AttackPayload(kPayload),
                   /*skipExt=*/".png");

    attacker.beginOperation();
    auto result = ex.exportToFolder(outDir_, opts);
    attacker.endOperation();
    attacker.join();

    // VACUITY GUARD. A green run where the attacker saw nothing is equally
    // explained by an export that never happened.
    ASSERT_GT(result.successCount, 0)
        << "the export produced nothing, so this case exercised no publish";
    EXPECT_GT(attacker.saw.load(), 0)
        << "the attacker observed no artifact at all during an export that "
           "definitely staged one";

    // THE NAME IS PART OF THE PROPERTY. Every artifact the attacker saw must be
    // either a finished .png or an unpredictable staging residue — a fix that
    // swapped ".part" for another guessable suffix would fail here.
    for (const auto& name : attacker.observed()) {
        if (fs::path(name).extension() == ".png") continue;
        // A staging residue is "<target>.pdfe_part.<32 hex>", so the target is
        // everything before the marker. `fs::path::stem()` is WRONG here and
        // measured wrong: for "page_1.png.pdfe_part.<hex>" it returns
        // "page_1.png.pdfe_part", and the predicate then rejects a perfectly
        // good name — a red over working code, which is the same species as a
        // green over broken code.
        const std::string marker = pdfe::core::StagingFile::kSuffixMarker;
        const auto at = name.rfind(marker);
        ASSERT_NE(at, std::string::npos)
            << "the export staged under a name that is not a staging residue "
               "at all: " << name;
        const std::string target = name.substr(0, at);
        EXPECT_TRUE(pdfe::core::StagingFile::isStagingResidueName(target, name))
            << "the export staged under a predictable name: " << name;
    }

    EXPECT_EQ(attacker.openedForWrite.load(), 0)
        << "a staging artifact was writable across the publish window. Wins: ["
        << attacker.wins << "]";
    EXPECT_EQ(attacker.substituted.load(), 0)
        << "a decoy was substituted over a staging artifact. Wins: ["
        << attacker.wins << "]";
    EXPECT_EQ(attacker.deleted.load(), 0)
        << "a staging artifact was deletable across the publish window. Wins: ["
        << attacker.wins << "]";

    // No attacker bytes may appear in any published image, and every published
    // file must still be a PNG.
    int pngs = 0;
    for (const auto& e : fs::directory_iterator(outDir_)) {
        if (e.path().extension() != ".png") continue;
        ++pngs;
        const auto body = readAll(e.path());
        EXPECT_EQ(body.find(kPayload), std::string::npos)
            << "ATTACKER BYTES WERE PUBLISHED AS AN EXPORTED IMAGE: "
            << e.path().filename().string();
        ASSERT_GE(body.size(), 8u);
        EXPECT_EQ(body.compare(0, 8, "\x89PNG\r\n\x1a\n"), 0)
            << "the published file is not a PNG: "
            << e.path().filename().string();
    }
    EXPECT_GT(pngs, 0);

    // No residue of ANY name survives a completed export.
    std::set<std::string> left;
    std::error_code ec;
    for (fs::directory_iterator it(outDir_, ec), end; !ec && it != end;
         it.increment(ec)) {
        if (it->path().extension() == ".png") continue;
        left.insert(it->path().filename().string());
    }
    EXPECT_TRUE(left.empty()) << "export residue survived";

    // CONTROL: the attacker's technique has to work on an unprotected file in
    // the same directory, or nothing above is evidence about the hardening.
    const fs::path control = outDir_ / "control_unprotected.bin";
    { std::ofstream f(control, std::ios::binary); f << "CONTROL"; }
    EXPECT_TRUE(canTakeDeleteAccess(control))
        << "the attacker's technique does not work even unprotected here";
}

// ===========================================================================
// Site #7 — BatchEngine::processFile
// ===========================================================================

class BatchPublishTocttou : public ::testing::Test {
protected:
    void SetUp() override
    {
        const auto src = fixtures() / "test_single.pdf";
        if (!fs::exists(src)) GTEST_SKIP() << "test_single.pdf missing";
        dir_ = makeWorkDir("batch");
        input_ = dir_ / "input.pdf";
        fs::copy_file(src, input_);
        // BatchEngine writes into "<input parent>/batch_output".
        outDir_ = dir_ / "batch_output";
        fs::create_directories(outDir_);
        decoy_ = makeDecoy(dir_);
        ctx_ = std::make_shared<pdfe::core::MuPdfContext>();
    }
    void TearDown() override
    {
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }
    fs::path dir_, input_, outDir_, decoy_;
    std::shared_ptr<pdfe::core::MuPdfContext> ctx_;
};

// The intermediates used to be "temp_<n>_<inputfilename>" in a directory whose
// name also derives from the input, and EACH ACTION READS THE PREVIOUS ONE BY
// NAME. Substituting one had the next action parse attacker bytes and the chain
// publish them as the batch output.
TEST_F(BatchPublishTocttou,
       PoisoningAnyBatchIntermediateCannotSubstituteThePublishedOutput)
{
    pdfe::batch::BatchEngine engine(ctx_);
    std::vector<pdfe::batch::BatchAction> actions = {
        {pdfe::batch::BatchActionType::FlattenAnnotations, std::monostate{}},
    };

    DirectoryAttacker attacker;
    // Skip the finished output: the property is that PDFE never PUBLISHES
    // attacker bytes, and a process with write access to a finished file in a
    // directory it can reach is not a claim PDFE makes.
    attacker.start(outDir_, decoy_, /*skip=*/{"input.pdf"}, AttackPayload(kPayload));

    attacker.beginOperation();
    auto result = engine.process({input_}, actions);
    attacker.endOperation();
    attacker.join();

    ASSERT_EQ(result.fileResults.size(), 1u);
    const auto& fr = result.fileResults.front();

    // A refusal is an acceptable outcome under attack — WIN-87 trades
    // availability for integrity — but it must be DIAGNOSABLE, and the
    // published bytes must never be the attacker's either way.
    if (!fr.success) {
        EXPECT_FALSE(fr.errorMessage.empty())
            << "a failed batch item must say why; a silent false is not a "
               "diagnosable error";
    }

    EXPECT_GT(attacker.saw.load(), 0)
        << "the attacker observed no artifact during a batch run that "
           "definitely staged intermediates. Seen: [" << attacker.seenNames()
        << "]";

    // THE PROPERTY HERE IS UNPREDICTABILITY, NOT EXCLUSIVITY, AND SAYING SO IS
    // THE POINT — asserting the stronger one would be a test pinning a property
    // the code does not have, which is how a suite starts lying.
    //
    // The batch chain's intermediates CANNOT be held behind an exclusive handle:
    // each action opens its OUTPUT by name (Document::saveAs, QPDF), so a
    // StagingFile handle would lock the producer out of the file it is meant to
    // write — the self-inflicted-lock class this codebase has hit three times.
    // What phase-58 achieves for them is that an attacker cannot AIM: the names
    // are 128 bits of CSPRNG instead of "temp_<n>_<inputfilename>" beside a
    // directory whose name also derives from the input. An enumerating attacker
    // like this one still finds them, and MEASURED it does obtain write access
    // to an intermediate; a real attacker must guess the name instead.
    //
    // What IS exclusive is the final staged output, and what IS asserted
    // unconditionally is the only thing that matters to a user: PDFE never
    // publishes the attacker's bytes as the batch output.
    for (const auto& name : attacker.observed()) {
        if (name == input_.filename().string()) continue;
        if (name == "control_unprotected.bin") continue;
        EXPECT_NE(name.rfind("temp_", 0), 0u)
            << "a batch intermediate still uses the old predictable name: "
            << name;
        EXPECT_NE(name.find(pdfe::core::StagingFile::kSuffixMarker),
                  std::string::npos)
            << "a batch artifact appeared under a name that is not an "
               "unpredictable staging residue: " << name;
    }

    RecordProperty("attacker_opened_an_intermediate_for_write",
                   attacker.openedForWrite.load());
    RecordProperty("attacker_substituted_an_intermediate",
                   attacker.substituted.load());

    const fs::path published = outDir_ / input_.filename();
    if (fs::exists(published)) {
        const auto body = readAll(published);
        EXPECT_EQ(body.find(kPayload), std::string::npos)
            << "ATTACKER BYTES WERE PUBLISHED AS THE BATCH OUTPUT";
        ASSERT_GE(body.size(), 5u);
        EXPECT_EQ(body.compare(0, 5, "%PDF-"), 0)
            << "the published batch output is not a PDF";
    }

    // CONTROL, as above.
    const fs::path control = outDir_ / "control_unprotected.bin";
    { std::ofstream f(control, std::ios::binary); f << "CONTROL"; }
    EXPECT_TRUE(canTakeDeleteAccess(control))
        << "the attacker's technique does not work even unprotected here";
}

}  // namespace

#endif  // _WIN32
