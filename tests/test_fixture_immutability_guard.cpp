// -*- coding: utf-8 -*-
// Self-tests for the fixture immutability guard (GitHub #386). The guard is
// installed for the whole pdfe_tests run by test_main.cpp; these pin that it
// actually notices each kind of change, and stays quiet when nothing changed,
// using a scratch directory rather than the real fixtures.
#include "fixture_immutability_guard.h"

#include <gtest/gtest.h>
#include <gtest/gtest-spi.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace fs = std::filesystem;
using pdfe_test::diffFixtures;
using pdfe_test::snapshotFixtures;

namespace {

class FixtureImmutabilityGuard : public ::testing::Test {
protected:
    void SetUp() override {
        root_ = fs::temp_directory_path() /
                ("pdfe_fixture_guard_" + std::string(
                    ::testing::UnitTest::GetInstance()->current_test_info()->name()));
        std::error_code ec;
        fs::remove_all(root_, ec);
        fs::create_directories(root_ / "sub");
        write("a.pdf", "%PDF-1.7 a");
        write("sub/b.pdf", "%PDF-1.7 b");
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(root_, ec);
    }
    void write(const std::string& rel, const std::string& bytes) {
        std::ofstream(root_ / rel, std::ios::binary) << bytes;
    }
    fs::path root_;
};

}  // namespace

TEST_F(FixtureImmutabilityGuard, IdenticalTreeHasNoDiff) {
    EXPECT_TRUE(diffFixtures(snapshotFixtures(root_), snapshotFixtures(root_)).empty());
}

TEST_F(FixtureImmutabilityGuard, SnapshotRecursesIntoSubdirectories) {
    const auto snap = snapshotFixtures(root_);
    EXPECT_EQ(snap.size(), 2u);
    EXPECT_EQ(snap.count("sub/b.pdf"), 1u);
}

TEST_F(FixtureImmutabilityGuard, RewriteWithDifferentSizeIsModified) {
    const auto before = snapshotFixtures(root_);
    write("a.pdf", "%PDF-1.7 a, redacted and rewritten");
    EXPECT_EQ(diffFixtures(before, snapshotFixtures(root_)),
              std::vector<std::string>{"modified: a.pdf"});
}

TEST_F(FixtureImmutabilityGuard, SameSizeRewriteIsCaughtByModificationTime) {
    const auto before = snapshotFixtures(root_);
    write("a.pdf", "%PDF-1.7 A");  // same length, different bytes
    // Force a distinct timestamp even on a coarse-grained filesystem clock.
    fs::last_write_time(root_ / "a.pdf",
                        before.at("a.pdf").mtime + std::chrono::seconds(2));
    EXPECT_EQ(diffFixtures(before, snapshotFixtures(root_)),
              std::vector<std::string>{"modified: a.pdf"});
}

TEST_F(FixtureImmutabilityGuard, CreatedAndDeletedFilesAreReported) {
    const auto before = snapshotFixtures(root_);
    write("new.pdf", "x");
    fs::remove(root_ / "sub/b.pdf");
    EXPECT_EQ(diffFixtures(before, snapshotFixtures(root_)),
              (std::vector<std::string>{"created: new.pdf", "deleted: sub/b.pdf"}));
}

TEST_F(FixtureImmutabilityGuard, MissingRootIsAnEmptySnapshot) {
    EXPECT_TRUE(snapshotFixtures(root_ / "does-not-exist").empty());
}

// The listener end to end: a change made during a test must produce a
// non-fatal failure attributed to that test, and the next unchanged test must
// stay green (the listener re-baselines after reporting).
TEST_F(FixtureImmutabilityGuard, ListenerFailsTheTestThatChangedAFixture) {
    auto listener = pdfe_test::makeFixtureImmutabilityListener(root_);
    const auto& self = *::testing::UnitTest::GetInstance()->current_test_info();

    write("sub/b.pdf", "%PDF-1.7 b, rewritten in place by a careless test");
    EXPECT_NONFATAL_FAILURE(listener->OnTestEnd(self), "modified: sub/b.pdf");

    // Nothing changed since: the listener must not keep failing later tests.
    listener->OnTestEnd(self);
}

TEST_F(FixtureImmutabilityGuard, ListenerIsSilentWhenNothingChanged) {
    auto listener = pdfe_test::makeFixtureImmutabilityListener(root_);
    listener->OnTestEnd(*::testing::UnitTest::GetInstance()->current_test_info());
}

// test_main.cpp must install the guard for the whole run, watching the real
// fixtures. The self-tests above build their own listener, so without this a
// deleted install line would leave every one of them green.
TEST(FixtureImmutabilityGuardInstall, IsInstalledForThisRun) {
    ASSERT_TRUE(pdfe_test::fixtureGuardInstalled())
        << "test_main.cpp no longer installs the fixture immutability guard";
    if (!std::getenv("PDFE_FIXTURE_GUARD_ROOT")) {
        EXPECT_EQ(fs::weakly_canonical(pdfe_test::fixtureGuardRoot()),
                  fs::weakly_canonical(fs::path(TEST_FIXTURES_DIR)))
            << "the installed guard is watching the wrong directory";
    }
}

namespace {

void setEnv(const char* name, const std::string& value) {
#ifdef _WIN32
    _putenv_s(name, value.c_str());
#else
    ::setenv(name, value.c_str(), 1);
#endif
}

void unsetEnv(const char* name) {
#ifdef _WIN32
    _putenv_s(name, "");
#else
    ::unsetenv(name);
#endif
}

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

}  // namespace

// The guard in a REAL run, through gtest's own event delivery. This test is
// both halves of the experiment:
//  - in the child (PDFE_FIXTURE_GUARD_PROBE set) it creates a file inside the
//    directory the installed guard watches, which the child's guard was told
//    to point at a scratch directory, and otherwise passes;
//  - in the parent it re-runs this binary on just this test and asserts the
//    child FAILED this test by name, citing the file it created.
// So it fails if the install, the repeater ordering, or the attribution to the
// test that did the damage ever stops holding.
TEST(FixtureImmutabilityGuardInstall, RealRunFailsTheTestThatTouchedAFixture) {
    if (std::getenv("PDFE_FIXTURE_GUARD_PROBE")) {
        // Never write outside the scratch directory: with no guard installed
        // the root is empty, and "probe.pdf" would land in the working dir.
        ASSERT_FALSE(pdfe_test::fixtureGuardRoot().empty())
            << "child run has no installed guard to probe";
        std::ofstream(pdfe_test::fixtureGuardRoot() / "probe.pdf") << "touched";
        return;
    }

    const fs::path scratch = fs::temp_directory_path() / "pdfe_fixture_guard_realrun";
    std::error_code ec;
    fs::remove_all(scratch, ec);
    fs::create_directories(scratch);
    { std::ofstream(scratch / "existing.pdf") << "%PDF-1.7"; }
    const fs::path out = scratch.string() + ".log";

    const std::string self = ::testing::internal::GetArgvs().at(0);
    const std::string cmd = "\"" + self + "\" --gtest_filter="
        "FixtureImmutabilityGuardInstall.RealRunFailsTheTestThatTouchedAFixture"
        " > \"" + out.string() + "\" 2>&1";

    setEnv("PDFE_FIXTURE_GUARD_PROBE", "1");
    setEnv("PDFE_FIXTURE_GUARD_ROOT", scratch.string());
#ifdef _WIN32
    // cmd.exe strips one outer pair of quotes from a command line that starts
    // with one, so wrap the whole line once more.
    const int rc = std::system(("\"" + cmd + "\"").c_str());
#else
    const int rc = std::system(cmd.c_str());
#endif
    unsetEnv("PDFE_FIXTURE_GUARD_PROBE");
    unsetEnv("PDFE_FIXTURE_GUARD_ROOT");

    const std::string log = readFile(out);
    EXPECT_NE(rc, 0) << "the child run passed although its test created a "
                        "fixture; child output:\n" << log;
    EXPECT_NE(log.find("[  FAILED  ] FixtureImmutabilityGuardInstall."
                       "RealRunFailsTheTestThatTouchedAFixture"),
              std::string::npos)
        << "the failure was not attributed to the test that touched the "
           "fixture; child output:\n" << log;
    EXPECT_NE(log.find("created: probe.pdf"), std::string::npos)
        << "the failure did not name the file; child output:\n" << log;

    fs::remove_all(scratch, ec);
    fs::remove(out, ec);
}
