// WIN-18 / GitHub #76 — two PDFE instances saving the same file at the same
// time must produce a DEFINED outcome, and the file that survives must be
// byte-level intact.
//
// WHAT WAS AND WAS NOT ALREADY COVERED. tests/test_full_rewrite_staging_-
// hardening.cpp has FullRewriteSaveTocttou.TwoDocumentsSavingTheSameTargetDoNot-
// CorruptIt, which runs two Documents on two THREADS of one process. That case
// is explicitly limited by its own comment: two in-process Documents cannot both
// complete, because after the winner publishes, save() reopens the target and
// the CRT "rb" handle held IN THIS PROCESS blocks the loser's replace-existing
// rename. That is a same-process artefact — FileRenameInfoEx's POSIX semantics
// relaxes exactly that restriction for handles held by OTHER processes. So the
// cross-process case, which is the one a user hits by opening one PDF in two
// PDFE windows, was never exercised, and it is the case where both saves can
// genuinely reach the publish step at the same moment. Its integrity check was
// also only `after.compare(0, 5, "%PDF-") == 0` — a five-byte prefix, which a
// file truncated at byte 6 would pass.
//
// SO THIS TEST IS CROSS-PROCESS, AND THE INSTANCES ARE REAL. Each child builds
// a real SingleInstanceGuard. PDFE holds a MACHINE-GLOBAL single-instance
// discriminator, so two coexisting instances need the opt-out; both env vars
// named in the issue exist in the product today
// (src/main.cpp:126 reads PDFE_DISABLE_SINGLE_INSTANCE,
//  src/app/SingleInstanceGuard.cpp:57 reads PDFE_INSTANCE_DOMAIN). This uses
// PDFE_INSTANCE_DOMAIN rather than the blanket opt-out, because it KEEPS the
// single-instance guarantee and merely partitions it — so the children are two
// instances that each really claimed primacy, which the blanket opt-out would
// not have proved. Each child reports its resolved server name and its
// isPrimary() result, and the parent asserts the names differ and both are
// primary; otherwise the "two instances" in this test would be a claim.
//
// THE OVERLAP IS FORCED, NOT HOPED FOR. Both children open the document, stage
// their edit, then block on one shared manual-reset event. The parent waits for
// both to report ready and only then opens the gate, so the two save() calls
// start within microseconds of each other. Without that, two sequential saves
// would pass every assertion here and measure nothing.
//
// THE ORACLE — "defined outcome" means exactly three things:
//   (1) Each instance ended in one of the product's three DEFINED terminal
//       states: SAVED, PUBLISHED_REOPEN_FAILED (Document.cpp:3745 — the
//       revision reached disk but the file could not be re-opened because the
//       other instance holds it), or REFUSED WITH A DIAGNOSABLE ERROR. A silent
//       failure, a hang, or a process death is none of those.
//   (2) At least one SAVED. "Both refused" would be defined but useless, and is
//       a data-loss defect in its own right.
//   (3) The surviving file carries EXACTLY ONE writer's edit, WHOLE. Each child
//       applies a distinct, readable marker — page 0 rotated to 90 or to 180 —
//       so last-writer-wins is checkable rather than assumed. A surviving
//       rotation of 0 would mean a save reported success and published pre-edit
//       bytes; any other value would mean the two writers' output interleaved.
//
// BYTE-LEVEL INTEGRITY is checked at the byte level and then above it, because
// MuPDF SILENTLY RECONSTRUCTS a damaged xref — so "Document::open succeeded"
// alone cannot distinguish an intact file from a repaired wreck. The raw bytes
// are therefore parsed directly: %PDF- header, a trailing %%EOF, and a
// `startxref` offset that is in range and lands on a real cross-reference
// section. Only then is the file reopened, its page count and marker checked,
// and round-tripped through another save to prove the structure survives being
// read AND rewritten.

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#endif

#include <gtest/gtest.h>

#include "app/SingleInstanceGuard.h"
#include "core/AtomicFileReplace.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"

#include <QString>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

using pdfe::app::SingleInstanceGuard;
using pdfe::core::Document;
using pdfe::core::MuPdfContext;
using pdfe::core::StagingNameKind;

namespace {

constexpr const char* kTargetVar = "PDFE_CONCSAVE_TARGET";
constexpr const char* kRotationVar = "PDFE_CONCSAVE_ROTATION";
constexpr const char* kModeVar = "PDFE_CONCSAVE_MODE";
constexpr const char* kGateVar = "PDFE_CONCSAVE_GATE";
constexpr const char* kReadyVar = "PDFE_CONCSAVE_READY";
constexpr const char* kResultVar = "PDFE_CONCSAVE_RESULT";

std::filesystem::path fixturesDir() {
    return std::filesystem::path(TEST_FIXTURES_DIR);
}

std::filesystem::path sourceFixture() {
    for (const char* rel : {"test_3page.pdf", "realworld/arxiv_attention.pdf",
                            "realworld/irs_i1040.pdf"}) {
        const auto p = fixturesDir() / rel;
        if (std::filesystem::exists(p)) return p;
    }
    return {};
}

std::string readAll(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f),
                       std::istreambuf_iterator<char>());
}

}  // namespace

#ifdef _WIN32

// ---------------------------------------------------------------------------
// THE CHILD ROLE — one PDFE instance.
// ---------------------------------------------------------------------------
TEST(ConcurrentInstanceSaveChild, RunRole) {
    const char* target = std::getenv(kTargetVar);
    if (target == nullptr || *target == '\0') {
        GTEST_SKIP() << "not a concurrent-save child (" << kTargetVar << " unset)";
    }
    const std::filesystem::path targetPath(target);
    const int rotation = std::atoi(std::getenv(kRotationVar));
    const std::string mode = std::getenv(kModeVar) ? std::getenv(kModeVar) : "";
    const char* resultPath = std::getenv(kResultVar);
    ASSERT_NE(resultPath, nullptr) << "child spawned without " << kResultVar;

    std::string outcome = "NONE";
    std::string error;

    // BE AN INSTANCE, not merely a process that writes to a file. PDFE_INSTANCE_-
    // DOMAIN was set by the parent to a value unique to this child, so this
    // guard claims primacy in its own partition of the machine-global domain.
    SingleInstanceGuard guard;
    const bool primary = guard.isPrimary();
    const std::string serverName = guard.serverName().toStdString();

    auto ctx = std::make_shared<MuPdfContext>();
    std::unique_ptr<Document> doc;
    try {
        doc = Document::open(ctx, targetPath);
    } catch (const std::exception& e) {
        error = std::string("open threw: ") + e.what();
    }

    if (doc) {
        try {
            // The writer's marker. Distinct per child and readable back out of
            // the saved file, so "which writer won" is an observation.
            doc->rotatePage(0, rotation);
            if (mode == "full") {
                doc->markRequiresFullRewrite();
            }
        } catch (const std::exception& e) {
            error = std::string("edit threw: ") + e.what();
            doc.reset();
        }
    }

    // Ready → wait for the gate → save. Both children resume from one
    // manual-reset event, so the two save() calls overlap.
    if (const char* readyName = std::getenv(kReadyVar)) {
        HANDLE ready = ::OpenEventA(EVENT_MODIFY_STATE, FALSE, readyName);
        if (ready) { ::SetEvent(ready); ::CloseHandle(ready); }
    }
    if (const char* gateName = std::getenv(kGateVar)) {
        HANDLE gate = ::OpenEventA(SYNCHRONIZE, FALSE, gateName);
        if (gate) {
            ::WaitForSingleObject(gate, 60000);
            ::CloseHandle(gate);
        }
    }

    if (doc) {
        try {
            doc->save(targetPath);
            outcome = "SAVED";
        } catch (const std::exception& e) {
            error = e.what();
            // THE THIRD DEFINED OUTCOME, and it is a throw. Document.cpp:3745
            // distinguishes "the revision WAS published, but the document could
            // not be re-opened afterwards because another process holds the
            // file" from a genuine publish failure — which is exactly what the
            // OTHER instance holding this file produces. Reporting it as
            // REFUSED would be a lie in the direction that matters: the caller
            // would believe its edit was lost when it is on disk. Observed on
            // this machine on the default save route.
            outcome = (error.find("WAS published successfully") != std::string::npos)
                          ? "PUBLISHED_REOPEN_FAILED"
                          : "REFUSED";
        } catch (...) {
            outcome = "REFUSED";
            error = "<non-std exception>";
        }
    } else if (outcome == "NONE") {
        outcome = "REFUSED";
        if (error.empty()) error = "document could not be opened or edited";
    }

    // Drop the document BEFORE reporting, so the parent never races this
    // process's own file handles when it reopens the target.
    doc.reset();
    ctx.reset();

    {
        std::ofstream out(resultPath, std::ios::binary | std::ios::trunc);
        out << "primary=" << (primary ? 1 : 0) << "\n"
            << "server=" << serverName << "\n"
            << "rotation=" << rotation << "\n"
            << "outcome=" << outcome << "\n"
            << "error=" << error << "\n";
        out.flush();
    }
}

// ---------------------------------------------------------------------------
// THE PARENT SIDE.
// ---------------------------------------------------------------------------

namespace {

std::filesystem::path selfExe() {
    wchar_t buf[MAX_PATH];
    const DWORD n = ::GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    return std::filesystem::path(std::wstring(buf, n));
}

std::filesystem::path makeWorkDir(const char* tag) {
    static std::atomic<unsigned> counter{0};
    std::error_code ec;
    auto d = std::filesystem::temp_directory_path()
             / (std::string("pdfe_win18_") + tag + "_"
                + std::to_string(::GetCurrentProcessId()) + "_"
                + std::to_string(counter.fetch_add(1)));
    std::filesystem::create_directories(d, ec);
    return ec ? std::filesystem::path{} : d;
}

std::map<std::string, std::string> parseResult(const std::filesystem::path& p) {
    std::map<std::string, std::string> kv;
    std::ifstream in(p);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        kv[line.substr(0, eq)] = line.substr(eq + 1);
    }
    return kv;
}

struct Instance {
    std::string domain;
    int rotation = 0;
    std::filesystem::path resultFile;
    PROCESS_INFORMATION pi{};
    bool spawned = false;
    DWORD exitCode = 0xFFFFFFFFu;
    bool exited = false;
    std::map<std::string, std::string> result;
};

// A named event, closed on scope exit.
struct Event {
    HANDLE h = nullptr;
    explicit Event(const std::string& name, bool manualReset) {
        h = ::CreateEventA(nullptr, manualReset ? TRUE : FALSE, FALSE,
                           name.c_str());
    }
    ~Event() { if (h) ::CloseHandle(h); }
    Event(const Event&) = delete;
    Event& operator=(const Event&) = delete;
};

std::vector<std::filesystem::path> stagingResidue(
    const std::filesystem::path& target) {
    std::vector<std::filesystem::path> out;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(target.parent_path(), ec), end;
         !ec && it != end; it.increment(ec)) {
        if (pdfe::core::classifyStagingName(target.filename(),
                                            it->path().filename())
            != StagingNameKind::NotStaging) {
            out.push_back(it->path());
        }
    }
    return out;
}

// BYTE-LEVEL STRUCTURAL CHECK, done on the raw bytes and NOT through MuPDF.
// MuPDF reconstructs a broken xref without complaint, so a reopen alone cannot
// tell an intact file from a repaired one; a truncated or interleaved save
// would sail past it. This walks the trailer the way a conforming reader must:
// the header, the final %%EOF, the last `startxref`, and that the offset it
// names is in range and lands on a cross-reference section (a classic `xref`
// table, or the "N G obj" header of an xref stream).
::testing::AssertionResult isStructurallyIntactPdf(const std::string& bytes) {
    if (bytes.size() < 32) {
        return ::testing::AssertionFailure()
               << "file is only " << bytes.size() << " bytes";
    }
    if (bytes.compare(0, 5, "%PDF-") != 0) {
        return ::testing::AssertionFailure() << "no %PDF- header";
    }
    const auto eof = bytes.rfind("%%EOF");
    if (eof == std::string::npos) {
        return ::testing::AssertionFailure()
               << "no %%EOF marker — the file is truncated";
    }
    // Nothing but whitespace may follow the final %%EOF. Trailing garbage is
    // the signature of two writers' bytes landing in one file.
    for (size_t i = eof + 5; i < bytes.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(bytes[i]);
        if (!std::isspace(c)) {
            return ::testing::AssertionFailure()
                   << "non-whitespace byte 0x" << std::hex << int(c) << std::dec
                   << " at offset " << i << ", after the final %%EOF";
        }
    }
    const auto sx = bytes.rfind("startxref", eof);
    if (sx == std::string::npos) {
        return ::testing::AssertionFailure()
               << "no startxref before the final %%EOF";
    }
    size_t i = sx + 9;
    while (i < bytes.size() && std::isspace(static_cast<unsigned char>(bytes[i]))) ++i;
    size_t digits = 0;
    unsigned long long offset = 0;
    while (i < bytes.size() && std::isdigit(static_cast<unsigned char>(bytes[i]))) {
        offset = offset * 10 + static_cast<unsigned>(bytes[i] - '0');
        ++i;
        ++digits;
        if (digits > 18) break;
    }
    if (digits == 0) {
        return ::testing::AssertionFailure() << "startxref has no offset";
    }
    if (offset == 0 || offset >= bytes.size()) {
        return ::testing::AssertionFailure()
               << "startxref offset " << offset << " is outside a file of "
               << bytes.size() << " bytes — the xref points past the end";
    }
    const std::string at = bytes.substr(static_cast<size_t>(offset),
                                        std::min<size_t>(24, bytes.size() - offset));
    const bool classicTable = at.compare(0, 4, "xref") == 0;
    const bool xrefStream =
        !at.empty() && std::isdigit(static_cast<unsigned char>(at[0]))
        && at.find(" obj") != std::string::npos;
    if (!classicTable && !xrefStream) {
        return ::testing::AssertionFailure()
               << "startxref offset " << offset
               << " does not land on a cross-reference section; bytes there: \""
               << at << "\"";
    }
    return ::testing::AssertionSuccess();
}

struct WorkDirCleaner {
    std::filesystem::path dir;
    ~WorkDirCleaner() {
        if (dir.empty()) return;
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
};

// The whole scenario, parameterised only by which save path the children take.
// `mode == "full"` forces the full-rewrite branch (one predictable
// "<target>.pdfe_tmp" staging name shared by both writers — the contended case
// AtomicFileReplace.cpp's F2 note is about). `mode == "default"` lets the
// product route the save itself, which is what a real user's rotate-and-save
// gets.
void runTwoInstanceSaveScenario(const char* tag, const char* mode) {
    const auto src = sourceFixture();
    ASSERT_FALSE(src.empty()) << "no source fixture found under " << fixturesDir();

    const auto dir = makeWorkDir(tag);
    ASSERT_FALSE(dir.empty()) << "could not create a work directory";
    WorkDirCleaner cleaner{dir};

    const auto target = dir / "shared.pdf";
    std::error_code ec;
    std::filesystem::copy_file(src, target, ec);
    ASSERT_FALSE(ec) << ec.message();

    // The baseline the markers are measured against. If the fixture's page 0
    // already carried a rotation, "the surviving rotation is 90" would not
    // witness anything.
    int basePageCount = 0;
    {
        auto ctx = std::make_shared<MuPdfContext>();
        auto doc = Document::open(ctx, target);
        ASSERT_NE(doc, nullptr);
        basePageCount = doc->pageCount();
        ASSERT_GE(basePageCount, 1);
        ASSERT_EQ(doc->currentRotation(0), 0)
            << "fixture page 0 is already rotated, so the writers' markers "
               "would not be distinguishable from the starting state";
    }

    const auto exe = selfExe();
    ASSERT_FALSE(exe.empty());

    const std::string uniq = std::to_string(::GetCurrentProcessId()) + "_" + tag;
    const std::string gateName = "Local\\pdfe_win18_gate_" + uniq;
    Event gate(gateName, /*manualReset=*/true);
    ASSERT_NE(gate.h, nullptr) << "could not create the start gate";

    Instance inst[2];
    inst[0] = Instance{"win18a-" + uniq, 90, dir / "a.result"};
    inst[1] = Instance{"win18b-" + uniq, 180, dir / "b.result"};

    std::string readyNames[2] = {"Local\\pdfe_win18_ready_a_" + uniq,
                                 "Local\\pdfe_win18_ready_b_" + uniq};
    Event readyA(readyNames[0], /*manualReset=*/true);
    Event readyB(readyNames[1], /*manualReset=*/true);
    ASSERT_NE(readyA.h, nullptr);
    ASSERT_NE(readyB.h, nullptr);
    HANDLE readies[2] = {readyA.h, readyB.h};

    for (int i = 0; i < 2; ++i) {
        ::SetEnvironmentVariableA(kTargetVar, target.string().c_str());
        ::SetEnvironmentVariableA(kRotationVar,
                                  std::to_string(inst[i].rotation).c_str());
        ::SetEnvironmentVariableA(kModeVar, mode);
        ::SetEnvironmentVariableA(kGateVar, gateName.c_str());
        ::SetEnvironmentVariableA(kReadyVar, readyNames[i].c_str());
        ::SetEnvironmentVariableA(kResultVar,
                                  inst[i].resultFile.string().c_str());
        ::SetEnvironmentVariableA("PDFE_INSTANCE_DOMAIN", inst[i].domain.c_str());

        std::wstring cmd = L"\"" + exe.wstring() +
                           L"\" --gtest_filter=ConcurrentInstanceSaveChild.RunRole "
                           L"--gtest_brief=1";
        std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
        cmdBuf.push_back(L'\0');

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        inst[i].spawned = ::CreateProcessW(exe.wstring().c_str(), cmdBuf.data(),
                                           nullptr, nullptr, FALSE,
                                           CREATE_NO_WINDOW, nullptr, nullptr,
                                           &si, &inst[i].pi) != FALSE;
    }

    // Cleared before any assertion can leave the parent poisoned: a leaked
    // PDFE_CONCSAVE_TARGET would make every later test in this process run the
    // child role, and a leaked PDFE_INSTANCE_DOMAIN would silently repartition
    // the single-instance tests.
    for (const char* v : {kTargetVar, kRotationVar, kModeVar, kGateVar,
                          kReadyVar, kResultVar, "PDFE_INSTANCE_DOMAIN"}) {
        ::SetEnvironmentVariableA(v, nullptr);
    }

    // Tear the children down however this test exits.
    struct ChildReaper {
        Instance* inst;
        ~ChildReaper() {
            for (int i = 0; i < 2; ++i) {
                if (!inst[i].spawned) continue;
                if (::WaitForSingleObject(inst[i].pi.hProcess, 0) != WAIT_OBJECT_0) {
                    ::TerminateProcess(inst[i].pi.hProcess, 1);
                    ::WaitForSingleObject(inst[i].pi.hProcess, 10000);
                }
                ::CloseHandle(inst[i].pi.hThread);
                ::CloseHandle(inst[i].pi.hProcess);
            }
        }
    } reaper{inst};

    ASSERT_TRUE(inst[0].spawned && inst[1].spawned)
        << "could not spawn both instances";

    // Both instances have opened the document and applied their edit; releasing
    // the gate is what makes the two saves CONCURRENT rather than sequential.
    // GitHub #215. A flat 120-second deadline here conflated two unrelated
    // failures:
    //
    //   a child DIED before reaching the gate      -- a real defect
    //   a child is alive and merely slow to start  -- setup latency, because
    //                                                 the machine is running a
    //                                                 16-shard suite
    //
    // Both surfaced identically, as this assertion failing after ~120014 ms,
    // and the second is what made this test's membership in the in-shard
    // failure set move with the partition.
    //
    // Poll instead, and separate the two. A child that has exited fails
    // IMMEDIATELY and says which one; a child that still exists is waited on
    // for as long as it keeps existing. The outer bound is generous and is
    // there only so a genuine hang still terminates rather than running to the
    // harness timeout.
    //
    // This weakens nothing: reaching the save gate is still REQUIRED, and a
    // wait that ends any other way is still a failure. The events are
    // manual-reset (see `Event` above), so re-waiting cannot lose a signal
    // that arrived between polls.
    DWORD ready = WAIT_TIMEOUT;
    int diedIndex = -1;
    for (int poll = 0; poll < 300 && diedIndex < 0; ++poll) {   // <= ~10 min
        ready = ::WaitForMultipleObjects(2, readies, TRUE, 2000);
        if (ready == WAIT_OBJECT_0) break;
        for (int i = 0; i < 2; ++i) {
            if (::WaitForSingleObject(inst[i].pi.hProcess, 0) == WAIT_OBJECT_0) {
                diedIndex = i;
                break;
            }
        }
    }
    ASSERT_EQ(diedIndex, -1)
        << "instance " << diedIndex << " exited before reaching the save gate, "
           "so the two saves were never concurrent. This is a real failure, "
           "NOT start-up latency — the child is gone, not slow.";
    ASSERT_EQ(ready, WAIT_OBJECT_0)
        << "one or both instances never reached the save gate (wait returned "
        << ready << ") — nothing concurrent happened. Both children were still "
           "alive throughout, so this is a genuine hang rather than the "
           "start-up latency #215 describes.";
    ASSERT_TRUE(::SetEvent(gate.h));

    for (int i = 0; i < 2; ++i) {
        if (::WaitForSingleObject(inst[i].pi.hProcess, 240000) == WAIT_OBJECT_0) {
            inst[i].exited = true;
            ::GetExitCodeProcess(inst[i].pi.hProcess, &inst[i].exitCode);
        }
    }

    // ---- (1) NEITHER INSTANCE MAY DIE OR HANG -----------------------------
    for (int i = 0; i < 2; ++i) {
        ASSERT_TRUE(inst[i].exited)
            << "instance " << i
            << " never exited — a concurrent save that hangs is not a defined "
               "outcome";
        EXPECT_EQ(inst[i].exitCode, 0u)
            << "instance " << i << " exited with 0x" << std::hex
            << inst[i].exitCode << std::dec
            << " — a concurrent save must fail cleanly, never crash";
        inst[i].result = parseResult(inst[i].resultFile);
        ASSERT_FALSE(inst[i].result.empty())
            << "instance " << i << " wrote no result file";
    }

    // Two REAL instances, not two processes wearing the name.
    EXPECT_EQ(inst[0].result["primary"], "1")
        << "instance 0 did not claim single-instance primacy";
    EXPECT_EQ(inst[1].result["primary"], "1")
        << "instance 1 did not claim single-instance primacy";
    EXPECT_NE(inst[0].result["server"], inst[1].result["server"])
        << "both instances resolved the SAME single-instance domain, so "
           "PDFE_INSTANCE_DOMAIN did not partition them and this test did not "
           "run two coexisting instances";

    // ---- (2) EACH OUTCOME IS ONE OF THE TWO DEFINED ONES ------------------
    int saved = 0;
    for (int i = 0; i < 2; ++i) {
        const std::string outcome = inst[i].result["outcome"];
        EXPECT_TRUE(outcome == "SAVED" || outcome == "PUBLISHED_REOPEN_FAILED"
                    || outcome == "REFUSED")
            << "instance " << i << " reported outcome \"" << outcome
            << "\", which is none of the three defined ones (completed save, "
               "published-but-not-reopened, or refusal)";
        if (outcome == "SAVED" || outcome == "PUBLISHED_REOPEN_FAILED") {
            ++saved;
        } else {
            EXPECT_FALSE(inst[i].result["error"].empty())
                << "instance " << i
                << " refused the save without saying why — an undiagnosable "
                   "refusal is indistinguishable from silent data loss";
        }
    }
    EXPECT_GE(saved, 1) << "BOTH instances failed to save; A=\""
                        << inst[0].result["error"] << "\" B=\""
                        << inst[1].result["error"] << "\"";

    // Recorded, not merely asserted: WHICH defined outcome occurred is the
    // interesting datum here and it can legitimately differ between runs and
    // between the two save routes. A green run that never says which branch it
    // took cannot be told apart from one that stopped exercising contention.
    for (int i = 0; i < 2; ++i) {
        const std::string key = std::string("instance_") + char('a' + i);
        ::testing::Test::RecordProperty(key + "_outcome",
                                        inst[i].result["outcome"]);
        ::testing::Test::RecordProperty(key + "_error", inst[i].result["error"]);
        std::printf("[WIN-18] %s: rotation=%d outcome=%s error=%s\n",
                    key.c_str(), inst[i].rotation,
                    inst[i].result["outcome"].c_str(),
                    inst[i].result["error"].c_str());
    }
    std::fflush(stdout);

    // ---- (3) NO RESIDUE ----------------------------------------------------
    EXPECT_TRUE(stagingResidue(target).empty())
        << "a contended cross-process save left staging residue beside the "
           "target";

    // ---- (4) BYTE-LEVEL INTEGRITY OF THE SURVIVOR --------------------------
    const std::string bytes = readAll(target);
    ASSERT_FALSE(bytes.empty()) << "the shared target was destroyed";
    EXPECT_TRUE(isStructurallyIntactPdf(bytes));

    // ---- (5) EXACTLY ONE WRITER'S EDIT, WHOLE ------------------------------
    {
        auto ctx = std::make_shared<MuPdfContext>();
        std::unique_ptr<Document> doc;
        ASSERT_NO_THROW(doc = Document::open(ctx, target));
        ASSERT_NE(doc, nullptr) << "the surviving file will not open";
        EXPECT_EQ(doc->pageCount(), basePageCount)
            << "the surviving file lost or gained pages";

        const int rot = doc->currentRotation(0);
        EXPECT_TRUE(rot == 90 || rot == 180)
            << "page 0 rotation is " << rot
            << ", which is neither writer's marker. 0 means a save reported "
               "success and published pre-edit bytes; anything else means the "
               "two writers' output was combined.";

        // ROUND TRIP. Being readable is not being intact — write the document
        // back out and read it again. A structure MuPDF quietly reconstructed
        // on open would not survive this with the marker still attached.
        const auto roundTrip = dir / "roundtrip.pdf";
        ASSERT_NO_THROW(doc->saveAs(roundTrip))
            << "the surviving file could not be re-saved";
        doc.reset();
        ctx.reset();

        auto ctx2 = std::make_shared<MuPdfContext>();
        auto rt = Document::open(ctx2, roundTrip);
        ASSERT_NE(rt, nullptr) << "the round-tripped file will not open";
        EXPECT_EQ(rt->pageCount(), basePageCount);
        EXPECT_EQ(rt->currentRotation(0), rot)
            << "the winner's marker did not survive a round trip";
        EXPECT_TRUE(isStructurallyIntactPdf(readAll(roundTrip)));
    }
}

}  // namespace

// The contended-staging-name case: both writers forced onto the full-rewrite
// path, which stages at the single predictable "<target>.pdfe_tmp".
TEST(ConcurrentInstanceSave, TwoInstancesFullRewriteSameFileDefinedOutcome) {
    runTwoInstanceSaveScenario("full", "full");
}

// What a user actually does: open one PDF in two windows, rotate a page in
// each, hit Save in both. The product chooses the save route.
TEST(ConcurrentInstanceSave, TwoInstancesDefaultRouteSameFileDefinedOutcome) {
    runTwoInstanceSaveScenario("default", "default");
}

#else   // !_WIN32

TEST(ConcurrentInstanceSave, WindowsOnly) {
    GTEST_SKIP() << "the single-instance domain and this harness are Windows-only";
}

#endif  // _WIN32
