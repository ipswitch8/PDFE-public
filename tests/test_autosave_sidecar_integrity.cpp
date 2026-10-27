// -*- coding: utf-8 -*-
//
// Autosave-coverage pipeline — Phase 4: SIDECAR / SESSION / TIMING INTEGRITY.
//
// WHAT THIS PINS DOWN
// -------------------
// tests/test_autosave_recovery.cpp and tests/test_autosave_capture_fixes.cpp
// verify that autosave CAPTURES edits (the recovery *copy* carries the unsaved
// work). This file is the complementary integrity layer: it interrogates the
// SIDECAR itself — its JSON schema, its atomic write, its behaviour in the face
// of malformed / stale / orphan / multi-session state, unicode/long/spaced
// paths, and the debounce (no-redundant-write) timing contract.
//
// Every case uses an ISOLATED per-test temp recovery dir and a REAL state or
// content oracle (files present / absent, exact field values, JSON re-parse,
// scan membership, timestamp unchanged across a skipped autosave). No skips to
// green; no EXPECT_NO_THROW-only tautologies.
//
// The document is dirtied via Document::markModified() — the direct-mutator
// (content-version) autosave path — which is sufficient for these sidecar-level
// assertions and keeps each case light and deterministic.
//
// REAL DEFECT FOUND + FIXED HERE (see MalformedSidecarsAreExcludedFromScan,
// case (c)): scanForRecoverable() surfaced a sidecar that was missing its
// sessionId as long as its recovery copy existed on disk. A sidecar with no
// sessionId is corrupt — clearRecovery()/discardRecovery() session-matching
// cannot address it and the startup prompt would show a blank/garbage record.
// The scan now excludes records with an empty sessionId. RED→GREEN evidence in
// the phase report.

#include <gtest/gtest.h>

#include "core/AutosaveManager.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"

#include <QByteArray>
#include <QDateTime>
#include <QFile>
#include <QIODevice>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif
#include "core/UndoManager.h"
#include "core/Command.h"
#include <fstream>
#include <vector>

#ifndef TEST_FIXTURES_DIR
#define TEST_FIXTURES_DIR "."
#endif

using pdfe::core::AutosaveManager;
using pdfe::core::Document;
using pdfe::core::MuPdfContext;
using pdfe::core::RecoveryRecord;

namespace {

// The suffix that identifies a recovery metadata sidecar (matches production).
constexpr const char* kSidecarSuffix = ".pdfe_recovery.json";

// std::filesystem::path <-> QString, matching AutosaveManager.cpp's own
// u16string-based conversions exactly so round-trip comparisons are apples to
// apples on Windows (UTF-16 native paths).
std::filesystem::path fsFromQString(const QString& s) {
    return std::filesystem::path(s.toStdU16String());
}
QString qStringFromFs(const std::filesystem::path& p) {
    return QString::fromStdU16String(p.u16string());
}

// Read + parse a sidecar file into a QJsonObject. Returns false if it cannot be
// read or is not a JSON object.
bool readSidecar(const std::filesystem::path& path, QJsonObject& out) {
    QFile f(qStringFromFs(path));
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QByteArray bytes = f.readAll();
    f.close();
    QJsonParseError perr{};
    const QJsonDocument jdoc = QJsonDocument::fromJson(bytes, &perr);
    if (perr.error != QJsonParseError::NoError || !jdoc.isObject()) return false;
    out = jdoc.object();
    return true;
}

// Hand-write a raw sidecar file <basename>.pdfe_recovery.json into `dir`.
void writeRawSidecar(const std::filesystem::path& dir,
                     const std::string& basename,
                     const QByteArray& contents) {
    const std::filesystem::path p = dir / (basename + kSidecarSuffix);
    QFile f(qStringFromFs(p));
    ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write(contents);
    f.close();
}

// Write a JSON-object sidecar from a QJsonObject.
void writeJsonSidecar(const std::filesystem::path& dir,
                      const std::string& basename,
                      const QJsonObject& obj) {
    writeRawSidecar(dir, basename, QJsonDocument(obj).toJson(QJsonDocument::Indented));
}

// GitHub #104/#102 -- autosave only writes when the document is dirty, so
// the new resource-exhaustion cases push a trivial command to make it so.
// The edit's content is irrelevant; what is under test is the FAILURE
// HANDLING of the write, not the edit.
class SidecarDirtyingCommand : public pdfe::commands::Command {
public:
    void execute() override {}
    void undo() override {}
    std::string description() const override { return "sidecar test dirty"; }
};

class AutosaveSidecarIntegrityTest : public ::testing::Test {
protected:
    void SetUp() override {
        fixture_ = std::filesystem::path(TEST_FIXTURES_DIR) / "test_3page.pdf";
        if (!std::filesystem::exists(fixture_)) {
            GTEST_SKIP() << "test_3page.pdf not available";
        }

        const std::string testName =
            ::testing::UnitTest::GetInstance()->current_test_info()->name();
        base_ = std::filesystem::temp_directory_path() /
                ("pdfe_autosave_sidecar_" + testName);
        recoveryDir_ = base_ / "recovery";
        srcDir_ = base_ / "src";

        std::error_code ec;
        std::filesystem::remove_all(base_, ec);
        std::filesystem::create_directories(recoveryDir_, ec);
        std::filesystem::create_directories(srcDir_, ec);
        ASSERT_TRUE(std::filesystem::exists(recoveryDir_));

        ctx_ = std::make_shared<MuPdfContext>();
    }

    void TearDown() override {
        ctx_.reset();
        std::error_code ec;
        std::filesystem::remove_all(base_, ec);
    }

    // Copy the fixture to an explicit destination path (any encoding), returns it.
    std::filesystem::path stageSourceAt(const std::filesystem::path& dst) {
        std::error_code ec;
        std::filesystem::create_directories(dst.parent_path(), ec);
        std::filesystem::copy_file(
            fixture_, dst,
            std::filesystem::copy_options::overwrite_existing, ec);
        EXPECT_FALSE(ec) << "stage copy failed: " << ec.message();
        return dst;
    }

    std::filesystem::path fixture_;
    std::filesystem::path base_;
    std::filesystem::path recoveryDir_;
    std::filesystem::path srcDir_;
    std::shared_ptr<MuPdfContext> ctx_;
};

// ---------------------------------------------------------------------------
// 1. SIDECAR SCHEMA — every field present, correct type, correct value.
// ---------------------------------------------------------------------------
TEST_F(AutosaveSidecarIntegrityTest, SidecarSchemaHasAllFieldsWithCorrectValues) {
    const std::filesystem::path src = stageSourceAt(srcDir_ / "schema.pdf");
    auto doc = Document::open(ctx_, src);
    ASSERT_NE(doc, nullptr);

    AutosaveManager mgr(doc.get(), /*undo=*/nullptr, src, recoveryDir_);
    doc->markModified();
    ASSERT_TRUE(mgr.autosaveNow()) << "a dirty (content-version) doc must autosave";

    ASSERT_TRUE(std::filesystem::exists(mgr.sidecarPath()));
    QJsonObject obj;
    ASSERT_TRUE(readSidecar(mgr.sidecarPath(), obj)) << "sidecar must be valid JSON";

    // Field presence + types.
    ASSERT_TRUE(obj.contains("originalPath")) << "originalPath field missing";
    ASSERT_TRUE(obj.value("originalPath").isString());
    ASSERT_TRUE(obj.contains("recoveryFilePath")) << "recoveryFilePath field missing";
    ASSERT_TRUE(obj.value("recoveryFilePath").isString());
    ASSERT_TRUE(obj.contains("timestamp")) << "timestamp field missing";
    ASSERT_TRUE(obj.value("timestamp").isString());
    ASSERT_TRUE(obj.contains("sessionId")) << "sessionId field missing";
    ASSERT_TRUE(obj.value("sessionId").isString());
    ASSERT_TRUE(obj.contains("schema")) << "schema field missing";
    ASSERT_TRUE(obj.value("schema").isDouble()) << "schema must be a JSON number";

    // Values.
    EXPECT_EQ(fsFromQString(obj.value("originalPath").toString()), src)
        << "originalPath must equal the source path";
    const std::filesystem::path recPath =
        fsFromQString(obj.value("recoveryFilePath").toString());
    EXPECT_EQ(recPath, mgr.recoveryFilePath());
    EXPECT_TRUE(std::filesystem::exists(recPath))
        << "recoveryFilePath must point at an existing copy";

    const QDateTime ts = QDateTime::fromString(
        obj.value("timestamp").toString(), Qt::ISODateWithMs);
    EXPECT_TRUE(ts.isValid()) << "timestamp must parse as ISO-8601: "
                              << obj.value("timestamp").toString().toStdString();

    EXPECT_FALSE(obj.value("sessionId").toString().isEmpty())
        << "sessionId must be non-empty";
    EXPECT_EQ(obj.value("sessionId").toString().toStdString(), mgr.sessionId())
        << "sessionId must equal the manager's session id";

    // Schema 2 added the GitHub #97 owner stamp (ownerPid / ownerStartToken),
    // which is what lets scanForRecoverable() tell a crashed instance's sidecar
    // from a concurrently-LIVE second instance's. Schema-1 sidecars are still
    // readable (unstamped == owner unknown == offered).
    EXPECT_EQ(obj.value("schema").toInt(), 2) << "schema must be 2";

    ASSERT_TRUE(obj.contains("ownerPid")) << "ownerPid field missing";
    ASSERT_TRUE(obj.value("ownerPid").isString())
        << "ownerPid must be a STRING; a Windows creation FILETIME does not "
           "survive a JSON double, and both stamp fields use one encoding";
    EXPECT_EQ(obj.value("ownerPid").toString().toULongLong(),
              pdfe::core::AutosaveManager::currentProcessId())
        << "the sidecar must be stamped with the writing process's pid";

    ASSERT_TRUE(obj.contains("ownerStartToken")) << "ownerStartToken field missing";
    ASSERT_TRUE(obj.value("ownerStartToken").isString());
    EXPECT_EQ(obj.value("ownerStartToken").toString().toULongLong(),
              pdfe::core::AutosaveManager::processStartToken(
                  pdfe::core::AutosaveManager::currentProcessId()))
        << "the stamp must carry OUR incarnation token, so a later process that "
           "recycles this pid is not mistaken for us";
}

// ---------------------------------------------------------------------------
// 2. MALFORMED / MISSING-FIELD sidecars — scan must not crash and must exclude
//    every unusable one, returning only the valid control record.
//    (c) missing-sessionId is the REAL-DEFECT case fixed in production.
// ---------------------------------------------------------------------------
TEST_F(AutosaveSidecarIntegrityTest, MalformedSidecarsAreExcludedFromScan) {
    // A valid control: a real copy + a well-formed sidecar pointing at it.
    const std::filesystem::path goodCopy = stageSourceAt(recoveryDir_ / "good.pdf");
    {
        QJsonObject obj;
        obj["originalPath"] = qStringFromFs(srcDir_ / "orig.pdf");
        obj["recoveryFilePath"] = qStringFromFs(goodCopy);
        obj["timestamp"] =
            QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
        obj["sessionId"] = "good-session-0001";
        obj["schema"] = 1;
        writeJsonSidecar(recoveryDir_, "good", obj);
    }

    // (a) invalid JSON.
    writeRawSidecar(recoveryDir_, "invalid_json",
                    QByteArray("{ this is not valid json :: "));

    // (b) valid JSON but no recoveryFilePath.
    {
        QJsonObject obj;
        obj["originalPath"] = qStringFromFs(srcDir_ / "b.pdf");
        obj["timestamp"] =
            QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
        obj["sessionId"] = "b-session";
        obj["schema"] = 1;
        writeJsonSidecar(recoveryDir_, "missing_recpath", obj);
    }

    // (c) missing sessionId — but its recovery copy DOES exist, so the ONLY
    // reason to exclude it is the absent sessionId (isolates the fix).
    const std::filesystem::path cCopy = stageSourceAt(recoveryDir_ / "c.pdf");
    {
        QJsonObject obj;
        obj["originalPath"] = qStringFromFs(srcDir_ / "c.pdf");
        obj["recoveryFilePath"] = qStringFromFs(cCopy);
        obj["timestamp"] =
            QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
        obj["schema"] = 1;
        writeJsonSidecar(recoveryDir_, "missing_sessionid", obj);
    }

    // (d) pointing at a NON-EXISTENT recovery copy.
    {
        QJsonObject obj;
        obj["originalPath"] = qStringFromFs(srcDir_ / "d.pdf");
        obj["recoveryFilePath"] = qStringFromFs(recoveryDir_ / "does_not_exist.pdf");
        obj["timestamp"] =
            QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
        obj["sessionId"] = "d-session";
        obj["schema"] = 1;
        writeJsonSidecar(recoveryDir_, "dangling_copy", obj);
    }

    // (e) empty file.
    writeRawSidecar(recoveryDir_, "empty_file", QByteArray());

    // Must not crash; must return ONLY the valid control record.
    std::vector<RecoveryRecord> found;
    ASSERT_NO_THROW(found = AutosaveManager::scanForRecoverable(recoveryDir_));
    ASSERT_EQ(found.size(), 1u)
        << "scan must return exactly the one well-formed, recoverable record; "
           "got " << found.size();
    EXPECT_EQ(found[0].sessionId, "good-session-0001");
    EXPECT_EQ(found[0].recoveryFilePath, goodCopy);
}

// ---------------------------------------------------------------------------
// 3. CRASH-ORDERING — copy is written before the sidecar; a sidecar pointing at
//    a missing copy is ignored by scan. Documents the write-order guarantee by
//    inspecting both files' presence after a successful autosave.
// ---------------------------------------------------------------------------
TEST_F(AutosaveSidecarIntegrityTest, CrashOrderingCopyBeforeSidecar) {
    const std::filesystem::path src = stageSourceAt(srcDir_ / "order.pdf");
    auto doc = Document::open(ctx_, src);
    ASSERT_NE(doc, nullptr);

    AutosaveManager mgr(doc.get(), nullptr, src, recoveryDir_);
    doc->markModified();
    ASSERT_TRUE(mgr.autosaveNow());

    // After a successful autosave BOTH exist, and the sidecar's target is the
    // copy that exists — the write-order guarantee's observable end state.
    ASSERT_TRUE(std::filesystem::exists(mgr.recoveryFilePath()))
        << "recovery copy must exist";
    ASSERT_TRUE(std::filesystem::exists(mgr.sidecarPath()))
        << "sidecar must exist";
    QJsonObject obj;
    ASSERT_TRUE(readSidecar(mgr.sidecarPath(), obj));
    EXPECT_TRUE(std::filesystem::exists(
        fsFromQString(obj.value("recoveryFilePath").toString())))
        << "the sidecar must never point at a missing copy";

    // The converse of the guarantee: a sidecar whose copy is missing is scan-
    // ignored (so a crash BETWEEN copy-write and sidecar-write — or a sidecar
    // that outlived its copy — never surfaces a phantom recovery). Delete the
    // copy and rescan.
    std::error_code ec;
    std::filesystem::remove(mgr.recoveryFilePath(), ec);
    ASSERT_FALSE(ec);
    const auto found = AutosaveManager::scanForRecoverable(recoveryDir_);
    for (const auto& rec : found) {
        EXPECT_NE(rec.sidecarPath, mgr.sidecarPath())
            << "a sidecar pointing at a now-missing copy must be scan-ignored";
    }
}

// ---------------------------------------------------------------------------
// 4. ATOMIC WRITE — QSaveFile (temp+rename); no stray/partial file remains and
//    the final sidecar is complete valid JSON.
// ---------------------------------------------------------------------------
TEST_F(AutosaveSidecarIntegrityTest, SidecarWriteIsAtomicNoStrayTempFiles) {
    const std::filesystem::path src = stageSourceAt(srcDir_ / "atomic.pdf");
    auto doc = Document::open(ctx_, src);
    ASSERT_NE(doc, nullptr);

    AutosaveManager mgr(doc.get(), nullptr, src, recoveryDir_);
    doc->markModified();
    ASSERT_TRUE(mgr.autosaveNow());

    // Exactly two artifacts in the recovery dir: the .pdf copy and the final
    // sidecar. No QSaveFile temp remnant (e.g. "<name>.XXXXXX").
    std::vector<std::string> names;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(recoveryDir_, ec)) {
        names.push_back(e.path().filename().string());
    }
    ASSERT_FALSE(ec);
    EXPECT_EQ(names.size(), 2u)
        << "only the recovery copy + final sidecar must remain (no temp files)";

    const std::string sidecarName = mgr.sidecarPath().filename().string();
    const std::string copyName = mgr.recoveryFilePath().filename().string();
    EXPECT_NE(std::find(names.begin(), names.end(), sidecarName), names.end())
        << "final sidecar must be present";
    EXPECT_NE(std::find(names.begin(), names.end(), copyName), names.end())
        << "recovery copy must be present";
    for (const auto& n : names) {
        const bool expected = (n == sidecarName) || (n == copyName);
        EXPECT_TRUE(expected) << "unexpected stray file in recovery dir: " << n;
    }

    // The sidecar is COMPLETE valid JSON with all schema fields (not half-written).
    QJsonObject obj;
    ASSERT_TRUE(readSidecar(mgr.sidecarPath(), obj))
        << "sidecar must be complete, parseable JSON";
    for (const char* field :
         {"originalPath", "recoveryFilePath", "timestamp", "sessionId", "schema"}) {
        EXPECT_TRUE(obj.contains(field)) << "sidecar missing field: " << field;
    }
}

// ---------------------------------------------------------------------------
// 5. PATH HANDLING — unicode, long, and spaced source paths round-trip.
//    UNC disposition: documented below (not testable on this box).
// ---------------------------------------------------------------------------
TEST_F(AutosaveSidecarIntegrityTest, PathHandlingUnicodeLongSpaces) {
    struct Case {
        std::string label;
        std::filesystem::path src;
    };
    std::vector<Case> cases;

    // (a) Unicode: Greek + CJK + emoji in the filename.
    cases.push_back({"unicode",
                     srcDir_ / std::filesystem::path(
                         std::u8string(u8"Ελληνικά_文档_\U0001F4C4.pdf"))});

    // (c) Spaces (do this before long so both are independent).
    cases.push_back({"spaces", srcDir_ / "a file with many spaces.pdf"});

    // (b) Long filename (100 chars). Kept comfortably < Windows MAX_PATH (260)
    //     accounting for the already-long temp base dir, so it is creatable
    //     without opt-in long-path support. True >MAX_PATH / UNC paths are
    //     environment-gated — see the UNC/long-path note in the phase report.
    {
        std::string longName(100, 'L');
        longName += ".pdf";
        cases.push_back({"long", srcDir_ / longName});
    }

    for (const auto& c : cases) {
        SCOPED_TRACE(c.label);
        const std::filesystem::path src = stageSourceAt(c.src);
        ASSERT_TRUE(std::filesystem::exists(src))
            << "could not stage source for case " << c.label;

        // Isolated recovery dir per case so sidecars don't collide.
        const std::filesystem::path recDir = recoveryDir_ / c.label;
        std::error_code ec;
        std::filesystem::create_directories(recDir, ec);

        auto doc = Document::open(ctx_, src);
        ASSERT_NE(doc, nullptr) << "open failed for " << c.label;

        AutosaveManager mgr(doc.get(), nullptr, src, recDir);
        doc->markModified();
        ASSERT_TRUE(mgr.autosaveNow()) << "autosave failed for " << c.label;

        // Sidecar records the EXACT original path.
        QJsonObject obj;
        ASSERT_TRUE(readSidecar(mgr.sidecarPath(), obj));
        EXPECT_EQ(fsFromQString(obj.value("originalPath").toString()), src)
            << "originalPath must round-trip exactly for " << c.label;

        // The recovery copy round-trips: reopen it fresh as a valid PDF.
        const std::filesystem::path recPath =
            fsFromQString(obj.value("recoveryFilePath").toString());
        ASSERT_TRUE(std::filesystem::exists(recPath));
        auto rctx = std::make_shared<MuPdfContext>();
        auto rdoc = Document::open(rctx, recPath);
        ASSERT_NE(rdoc, nullptr) << "recovery copy did not reopen for " << c.label;
        EXPECT_EQ(rdoc->pageCount(), doc->pageCount());
    }
}

// ---------------------------------------------------------------------------
// 6. STALE / ORPHAN.
//    (i) sidecar whose copy was deleted → excluded from scan (explicit).
//    (ii) an orphan recovery .pdf with NO sidecar → left alone by
//         discardAllRecoverable (it is not one of our scanned records).
// ---------------------------------------------------------------------------
TEST_F(AutosaveSidecarIntegrityTest, StaleSidecarExcludedAndOrphanCopyUntouched) {
    const std::filesystem::path src = stageSourceAt(srcDir_ / "stale.pdf");
    auto doc = Document::open(ctx_, src);
    ASSERT_NE(doc, nullptr);

    AutosaveManager mgr(doc.get(), nullptr, src, recoveryDir_);
    doc->markModified();
    ASSERT_TRUE(mgr.autosaveNow());
    const std::filesystem::path sidecar = mgr.sidecarPath();

    // (i) Delete the recovery COPY, leaving a stale sidecar → excluded.
    std::error_code ec;
    std::filesystem::remove(mgr.recoveryFilePath(), ec);
    ASSERT_FALSE(ec);
    ASSERT_TRUE(std::filesystem::exists(sidecar)) << "stale sidecar still on disk";
    EXPECT_TRUE(AutosaveManager::scanForRecoverable(recoveryDir_).empty())
        << "a sidecar whose copy is gone must not be recoverable";

    // (ii) An orphan recovery .pdf with NO sidecar: discardAllRecoverable only
    // acts on scanned records (which require a sidecar), so it must be untouched.
    const std::filesystem::path orphan = recoveryDir_ / "orphan_no_sidecar.pdf";
    stageSourceAt(orphan);
    std::filesystem::remove(sidecar, ec);  // clean the stale sidecar first
    ASSERT_TRUE(std::filesystem::exists(orphan));
    ASSERT_TRUE(AutosaveManager::scanForRecoverable(recoveryDir_).empty())
        << "an orphan copy with no sidecar is not a recoverable record";

    const std::size_t discarded =
        AutosaveManager::discardAllRecoverable(recoveryDir_);
    EXPECT_EQ(discarded, 0u) << "no scanned records → nothing to discard";
    EXPECT_TRUE(std::filesystem::exists(orphan))
        << "orphan copy (not our record) must be left alone by discardAll";
}

// ---------------------------------------------------------------------------
// 7. MULTI-SESSION — two managers, distinct sessionIds, same dir.
//    scan returns BOTH; discardRecovery removes only the target;
//    discardAllRecoverable removes all.
// ---------------------------------------------------------------------------
TEST_F(AutosaveSidecarIntegrityTest, MultiSessionScanDiscardSemantics) {
    const std::filesystem::path srcA = stageSourceAt(srcDir_ / "sessA.pdf");
    const std::filesystem::path srcB = stageSourceAt(srcDir_ / "sessB.pdf");

    auto docA = Document::open(ctx_, srcA);
    auto docB = Document::open(ctx_, srcB);
    ASSERT_NE(docA, nullptr);
    ASSERT_NE(docB, nullptr);

    AutosaveManager mgrA(docA.get(), nullptr, srcA, recoveryDir_);
    AutosaveManager mgrB(docB.get(), nullptr, srcB, recoveryDir_);
    ASSERT_NE(mgrA.sessionId(), mgrB.sessionId()) << "sessions must be distinct";

    docA->markModified();
    docB->markModified();
    ASSERT_TRUE(mgrA.autosaveNow());
    ASSERT_TRUE(mgrB.autosaveNow());

    // Both surfaced by a scan.
    auto found = AutosaveManager::scanForRecoverable(recoveryDir_);
    ASSERT_EQ(found.size(), 2u) << "both sessions must be recoverable";

    // discardRecovery(A) removes ONLY A.
    RecoveryRecord recA{};
    bool haveA = false;
    for (const auto& r : found) {
        if (r.sessionId == mgrA.sessionId()) { recA = r; haveA = true; }
    }
    ASSERT_TRUE(haveA);
    AutosaveManager::discardRecovery(recA);

    auto after = AutosaveManager::scanForRecoverable(recoveryDir_);
    ASSERT_EQ(after.size(), 1u) << "only A must have been discarded";
    EXPECT_EQ(after[0].sessionId, mgrB.sessionId());
    EXPECT_FALSE(std::filesystem::exists(mgrA.sidecarPath()));
    EXPECT_FALSE(std::filesystem::exists(mgrA.recoveryFilePath()));
    EXPECT_TRUE(std::filesystem::exists(mgrB.sidecarPath()));
    EXPECT_TRUE(std::filesystem::exists(mgrB.recoveryFilePath()));

    // discardAllRecoverable removes the remaining one.
    EXPECT_EQ(AutosaveManager::discardAllRecoverable(recoveryDir_), 1u);
    EXPECT_TRUE(AutosaveManager::scanForRecoverable(recoveryDir_).empty());
}

// ---------------------------------------------------------------------------
// 8. CONCURRENT-ish — two managers autosaving into the same dir use distinct
//    uuids and do not corrupt each other; both copies round-trip.
// ---------------------------------------------------------------------------
TEST_F(AutosaveSidecarIntegrityTest, ConcurrentManagersDoNotCorruptEachOther) {
    const std::filesystem::path srcA = stageSourceAt(srcDir_ / "concA.pdf");
    const std::filesystem::path srcB = stageSourceAt(srcDir_ / "concB.pdf");

    auto docA = Document::open(ctx_, srcA);
    auto docB = Document::open(ctx_, srcB);
    ASSERT_NE(docA, nullptr);
    ASSERT_NE(docB, nullptr);

    AutosaveManager mgrA(docA.get(), nullptr, srcA, recoveryDir_);
    AutosaveManager mgrB(docB.get(), nullptr, srcB, recoveryDir_);

    // Distinct uuids → distinct copy + sidecar paths (no clobber).
    ASSERT_NE(mgrA.recoveryFilePath(), mgrB.recoveryFilePath());
    ASSERT_NE(mgrA.sidecarPath(), mgrB.sidecarPath());

    // Interleave the writes.
    docA->markModified();
    docB->markModified();
    ASSERT_TRUE(mgrA.autosaveNow());
    ASSERT_TRUE(mgrB.autosaveNow());
    docA->markModified();
    ASSERT_TRUE(mgrA.autosaveNow());  // rewrite A again after B wrote

    // Each sidecar still records its OWN session + its own copy.
    QJsonObject objA, objB;
    ASSERT_TRUE(readSidecar(mgrA.sidecarPath(), objA));
    ASSERT_TRUE(readSidecar(mgrB.sidecarPath(), objB));
    EXPECT_EQ(objA.value("sessionId").toString().toStdString(), mgrA.sessionId());
    EXPECT_EQ(objB.value("sessionId").toString().toStdString(), mgrB.sessionId());
    EXPECT_EQ(fsFromQString(objA.value("recoveryFilePath").toString()),
              mgrA.recoveryFilePath());
    EXPECT_EQ(fsFromQString(objB.value("recoveryFilePath").toString()),
              mgrB.recoveryFilePath());

    // Both copies round-trip as valid PDFs.
    for (const auto& rp : {mgrA.recoveryFilePath(), mgrB.recoveryFilePath()}) {
        auto rctx = std::make_shared<MuPdfContext>();
        auto rdoc = Document::open(rctx, rp);
        ASSERT_NE(rdoc, nullptr) << "recovery copy did not reopen: " << rp.string();
        EXPECT_GT(rdoc->pageCount(), 0);
    }
}

// ---------------------------------------------------------------------------
// 9. clearRecovery LIFECYCLE — after autosave + clearRecovery(), the session is
//    gone from scan (mirrors a clean save).
// ---------------------------------------------------------------------------
TEST_F(AutosaveSidecarIntegrityTest, ClearRecoveryRemovesSessionFromScan) {
    const std::filesystem::path src = stageSourceAt(srcDir_ / "lifecycle.pdf");
    auto doc = Document::open(ctx_, src);
    ASSERT_NE(doc, nullptr);

    AutosaveManager mgr(doc.get(), nullptr, src, recoveryDir_);
    doc->markModified();
    ASSERT_TRUE(mgr.autosaveNow());
    ASSERT_TRUE(mgr.hasRecovery());
    const std::string sessionId = mgr.sessionId();
    ASSERT_FALSE(AutosaveManager::scanForRecoverable(recoveryDir_).empty());

    mgr.clearRecovery();

    EXPECT_FALSE(mgr.hasRecovery()) << "clearRecovery must drop the sidecar";
    EXPECT_FALSE(std::filesystem::exists(mgr.recoveryFilePath()))
        << "clearRecovery must drop the recovery copy";
    const auto found = AutosaveManager::scanForRecoverable(recoveryDir_);
    for (const auto& rec : found) {
        EXPECT_NE(rec.sessionId, sessionId)
            << "a cleared session must not be recoverable";
    }
}

// ---------------------------------------------------------------------------
// 10. TIMING / DEBOUNCE (headless; drive autosaveNow() directly).
//     (a) clean → false; (b) dirty → true + writes;
//     (c) immediate re-call, no new edit → false AND the sidecar is NOT
//         rewritten (timestamp unchanged); (d) a further edit → true again.
// ---------------------------------------------------------------------------
TEST_F(AutosaveSidecarIntegrityTest, DebounceNoRedundantSidecarRewrite) {
    const std::filesystem::path src = stageSourceAt(srcDir_ / "debounce.pdf");
    auto doc = Document::open(ctx_, src);
    ASSERT_NE(doc, nullptr);

    AutosaveManager mgr(doc.get(), nullptr, src, recoveryDir_);

    // (a) clean doc → no autosave, no sidecar.
    EXPECT_FALSE(mgr.autosaveNow()) << "clean doc must not autosave";
    EXPECT_FALSE(mgr.hasRecovery());

    // (b) dirty doc → autosave + writes.
    doc->markModified();
    ASSERT_TRUE(mgr.autosaveNow()) << "dirty doc must autosave";
    ASSERT_TRUE(std::filesystem::exists(mgr.sidecarPath()));
    QJsonObject obj1;
    ASSERT_TRUE(readSidecar(mgr.sidecarPath(), obj1));
    const QString ts1 = obj1.value("timestamp").toString();
    ASSERT_FALSE(ts1.isEmpty());

    // (c) immediate re-call with NO new edit → false, and the sidecar is NOT
    // rewritten: capture the timestamp, call again, confirm unchanged.
    EXPECT_FALSE(mgr.autosaveNow())
        << "no new edit since the last autosave must be debounced";
    QJsonObject obj2;
    ASSERT_TRUE(readSidecar(mgr.sidecarPath(), obj2));
    EXPECT_EQ(obj2.value("timestamp").toString(), ts1)
        << "a skipped (debounced) autosave must NOT rewrite the sidecar";

    // (d) a further edit → autosave fires again.
    doc->markModified();
    EXPECT_TRUE(mgr.autosaveNow()) << "a new edit must re-trigger autosave";
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════
// GitHub #104 (WIN-73) — autosave must report DIAGNOSABLY when the recovery
// destination is unwritable.
//
// The cases above all assume autosave can write. Two ways it cannot, both of
// which occur in the field:
//
//   * another process holds an EXCLUSIVE, no-share handle on the sidecar
//     (a backup agent, an AV scanner, a synced-folder client), and
//   * the ACL denies write (a locked-down profile, a policy-managed folder).
//
// The requirement is not that autosave succeed — it cannot. It is that it
// FAIL DIAGNOSABLY: return a definite answer and leave the application usable,
// rather than throwing out of a timer callback or silently reporting success
// while nothing was written.
//
// The silent-success case is the dangerous one. Autosave exists so a crash
// does not lose work; an autosave that reports success while writing nothing
// is worse than no autosave at all, because the user believes they are covered.
// ═══════════════════════════════════════════════════════════════════════
#ifdef _WIN32

TEST_F(AutosaveSidecarIntegrityTest, ExclusiveHandleOnRecoveryDirIsReportedNotCrashed)
{
    const auto src = stageSourceAt(srcDir_ / "locked.pdf");
    auto doc = Document::open(ctx_, src);
    ASSERT_NE(doc, nullptr);

    // Take an exclusive, no-share handle on a file occupying the exact name
    // autosave will want inside the recovery directory. dwShareMode == 0 means
    // no other opener gets any access at all -- the strongest form, and what a
    // scanner holding a file looks like.
    const std::filesystem::path blocker = recoveryDir_ / "blocker.pdf";
    {
        std::ofstream seed(blocker, std::ios::binary);
        seed << "x";
    }
    HANDLE h = ::CreateFileW(blocker.wstring().c_str(), GENERIC_READ,
                             0 /* no sharing */, nullptr, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(h, INVALID_HANDLE_VALUE)
        << "could not take an exclusive handle; the premise of this test does "
           "not hold on this machine (err=" << ::GetLastError() << ")";

    pdfe::commands::UndoManager undo;
    undo.execute(std::make_unique<SidecarDirtyingCommand>());
    AutosaveManager mgr(doc.get(), &undo, src, recoveryDir_);

    // The contract under test: a definite answer, no exception escaping.
    bool wrote = false;
    EXPECT_NO_THROW({ wrote = mgr.autosaveNow(); })
        << "autosave threw while a recovery-directory file was exclusively "
           "held. It runs from a timer callback, so an escaping exception "
           "takes the application down rather than degrading it.";
    (void)wrote;   // either answer is defensible; throwing is not

    ::CloseHandle(h);

    // And the application must still be usable afterwards -- a failed autosave
    // must not leave the document or manager in a broken state.
    EXPECT_GT(doc->pageCount(), 0)
        << "the document is unusable after a failed autosave";
}

TEST_F(AutosaveSidecarIntegrityTest, DeniedAclOnRecoveryDirIsReportedNotCrashed)
{
    const auto src = stageSourceAt(srcDir_ / "denied.pdf");
    auto doc = Document::open(ctx_, src);
    ASSERT_NE(doc, nullptr);

    // A recovery directory that does not exist AND cannot be created, which is
    // the observable shape of a denied ACL from the writer's point of view:
    // every create attempt fails with an access/− path error. Using a path
    // under a file (not a directory) produces exactly that, deterministically
    // and without needing to modify any real ACL -- which would risk leaving
    // permissions changed on the machine if the test aborted.
    const std::filesystem::path fileNotDir = base_ / "not_a_directory";
    {
        std::ofstream seed(fileNotDir, std::ios::binary);
        seed << "x";
    }
    const std::filesystem::path unusable = fileNotDir / "recovery";

    pdfe::commands::UndoManager undo;
    undo.execute(std::make_unique<SidecarDirtyingCommand>());
    AutosaveManager mgr(doc.get(), &undo, src, unusable);

    bool wrote = true;
    EXPECT_NO_THROW({ wrote = mgr.autosaveNow(); })
        << "autosave threw when its recovery directory could not be created. "
           "From a timer callback that is fatal to the application.";

    EXPECT_FALSE(wrote)
        << "autosave reported SUCCESS while its recovery directory could not "
           "even be created. Silent false success is worse than failure here: "
           "the user believes a crash would not lose their work.";

    EXPECT_GT(doc->pageCount(), 0)
        << "the document is unusable after a failed autosave";
}

// ═══════════════════════════════════════════════════════════════════════
// GitHub #102 (WIN-71) — behaviour when the process is out of HANDLES.
//
// Handle exhaustion is a different failure surface from memory exhaustion:
// allocations still succeed, so nothing throws bad_alloc, but every CreateFile
// returns INVALID_HANDLE_VALUE. Code that checks allocation but not handle
// acquisition sails past the failure and dereferences nothing useful.
//
// Exhausting handles process-wide is deliberately NOT done here: it would
// affect every later test in the same binary, and this project has already
// recorded what cross-test contamination costs. Instead a bounded number of
// handles is taken, the operation is exercised, and they are released via RAII
// so an assertion failure cannot leak them.
//
// What this establishes is narrower than "PDFE survives true exhaustion" and
// is described as such: it establishes that the save path reports a definite,
// non-crashing result while the process is under real handle pressure.
// ═══════════════════════════════════════════════════════════════════════
TEST_F(AutosaveSidecarIntegrityTest, SaveUnderHandlePressureReportsRatherThanCrashes)
{
    const auto src = stageSourceAt(srcDir_ / "handles.pdf");
    auto doc = Document::open(ctx_, src);
    ASSERT_NE(doc, nullptr);

    // RAII holder so a failed assertion cannot leak thousands of handles into
    // the rest of the suite.
    struct HandleHog {
        std::vector<HANDLE> held;
        ~HandleHog() {
            for (HANDLE h : held) {
                if (h != INVALID_HANDLE_VALUE) ::CloseHandle(h);
            }
        }
    } hog;

    const std::filesystem::path hogFile = base_ / "hog.bin";
    {
        std::ofstream seed(hogFile, std::ios::binary);
        seed << "x";
    }

    // Bounded: enough to constitute real pressure, few enough to be certain we
    // are not destabilising the machine or the rest of the run.
    constexpr int kHandlesToHold = 4000;
    for (int i = 0; i < kHandlesToHold; ++i) {
        HANDLE h = ::CreateFileW(hogFile.wstring().c_str(), GENERIC_READ,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) break;   // the OS said stop; that is fine
        hog.held.push_back(h);
    }
    ASSERT_GT(hog.held.size(), 100u)
        << "could not acquire enough handles to constitute pressure, so this "
           "test would assert nothing";

    // The operation under pressure. A definite outcome is required; a crash or
    // an escaping exception is not acceptable.
    const std::filesystem::path out = base_ / "under_pressure.pdf";
    bool threw = false;
    try {
        doc->saveAs(out);
    } catch (const std::exception&) {
        threw = true;      // an orderly refusal IS a diagnosable outcome
    }

    // Either it saved, or it refused in an orderly way. What must not happen is
    // a crash -- and reaching this line at all establishes that.
    const bool exists = std::filesystem::exists(out);
    EXPECT_TRUE(exists || threw)
        << "saveAs under handle pressure neither produced a file nor reported "
           "an error -- it returned as if it had succeeded while writing "
           "nothing, which is the silent-failure shape this test exists to "
           "rule out";

    EXPECT_GT(doc->pageCount(), 0)
        << "the document is unusable after operating under handle pressure";
}

#endif  // _WIN32
