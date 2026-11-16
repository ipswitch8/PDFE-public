// -*- coding: utf-8 -*-
// Fixture immutability guard (GitHub #386).
//
// MaliciousPdfTest.RedactionClearsUndoEvidence opened the TRACKED fixture
// tests/fixtures/test_single.pdf and applied a rewriting redaction to it in
// place. Every later suite reading that fixture then saw a redacted page, so a
// broad run produced ten "regressions" in unrelated suites that all cleared on
// `git checkout -- tests/fixtures/test_single.pdf`. Nothing attributed the
// damage to the test that did it.
//
// This guard is the class-level control: a gtest listener that records the
// size and modification time of every regular file under the fixtures
// directory before the first test, re-checks them after EVERY test, and fails
// the test during which a fixture was modified, created or deleted. The
// failure lands on the offending test by name, not on whichever later suite
// happens to read the damaged file.
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace testing { class TestEventListener; }

namespace pdfe_test {

struct FixtureStamp {
    std::uintmax_t size = 0;
    std::filesystem::file_time_type mtime{};
    bool operator==(const FixtureStamp& o) const {
        return size == o.size && mtime == o.mtime;
    }
};

using FixtureSnapshot = std::map<std::string, FixtureStamp>;  // relative path -> stamp

/// Stamp every regular file under `root` (recursively). Unreadable entries are
/// skipped rather than thrown on; a missing root yields an empty snapshot.
FixtureSnapshot snapshotFixtures(const std::filesystem::path& root);

/// Human-readable differences between two snapshots ("modified: a.pdf",
/// "created: b.pdf", "deleted: c.pdf"), sorted. Empty when identical.
std::vector<std::string> diffFixtures(const FixtureSnapshot& before,
                                      const FixtureSnapshot& after);

/// The listener itself, for tests that drive it directly. installFixture-
/// ImmutabilityGuard appends one of these to gtest's listener list.
std::unique_ptr<::testing::TestEventListener>
makeFixtureImmutabilityListener(const std::filesystem::path& fixturesRoot);

/// Append the per-test listener to gtest's listener list. Call once, after
/// InitGoogleTest and before RUN_ALL_TESTS. If PDFE_FIXTURE_GUARD_ROOT is set,
/// that directory is watched instead of `fixturesRoot`; it exists only so the
/// guard's own end-to-end test can point a child run at a scratch directory.
void installFixtureImmutabilityGuard(const std::filesystem::path& fixturesRoot);

/// True once installFixtureImmutabilityGuard has run in this process, so a
/// test can fail if test_main.cpp ever stops installing the guard.
bool fixtureGuardInstalled();

/// The directory the installed guard watches (empty if not installed).
std::filesystem::path fixtureGuardRoot();

}  // namespace pdfe_test
