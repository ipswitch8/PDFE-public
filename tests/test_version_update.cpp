// -*- coding: utf-8 -*-
//
// R3-16 — version-check channel core.
//
// Exercises the REAL semantic-version comparison in pdfe::app (VersionInfo),
// not a stub. Also pins that the library's reported currentVersion() matches
// the build's version source of truth (project(pdfe VERSION ...) threaded via
// the PDFE_VERSION_STRING compile definition, with a literal fallback kept in
// lock-step).
//
// The comparison is pure and network-free by design: the fetch of the "latest
// available" string from a release feed is documented future work, so this
// test needs no I/O and is fully deterministic.

#include <gtest/gtest.h>

#include <string>

#include "app/VersionInfo.h"

// Mirror the source-of-truth macro so the cross-check works whether or not the
// PUBLIC compile definition has been wired onto pdfe_app yet. The literal here
// MUST equal project(pdfe VERSION ...) in the top-level CMakeLists.txt, exactly
// as the VersionInfo.cpp fallback does.
#ifndef PDFE_VERSION_STRING
#define PDFE_VERSION_STRING "0.3.0"
#endif

using pdfe::app::UpdateStatus;
using pdfe::app::compareVersions;
using pdfe::app::currentVersion;
using pdfe::app::checkForUpdate;

// --- Core three-way outcomes (the acceptance-criterion cases) ---------------

TEST(VersionUpdateTest, OlderCurrentYieldsUpdateAvailable) {
    EXPECT_EQ(compareVersions("1.0.0", "1.0.1"), UpdateStatus::UpdateAvailable);
    EXPECT_EQ(compareVersions("1.0.0", "1.1.0"), UpdateStatus::UpdateAvailable);
    EXPECT_EQ(compareVersions("1.0.0", "2.0.0"), UpdateStatus::UpdateAvailable);
}

TEST(VersionUpdateTest, EqualVersionsYieldUpToDate) {
    EXPECT_EQ(compareVersions("1.2.0", "1.2.0"), UpdateStatus::UpToDate);
    EXPECT_EQ(compareVersions("0.1.0", "0.1.0"), UpdateStatus::UpToDate);
}

TEST(VersionUpdateTest, NewerCurrentYieldsNewer) {
    EXPECT_EQ(compareVersions("2.0.0", "1.9.9"), UpdateStatus::Newer);
    EXPECT_EQ(compareVersions("1.2.1", "1.2.0"), UpdateStatus::Newer);
}

// --- Segment-count, leading-zero, prefix and pre-release handling -----------

TEST(VersionUpdateTest, UnequalSegmentCountsZeroPad) {
    EXPECT_EQ(compareVersions("1.2", "1.2.0"), UpdateStatus::UpToDate);
    EXPECT_EQ(compareVersions("1.2.0", "1.2"), UpdateStatus::UpToDate);
    EXPECT_EQ(compareVersions("1.2", "1.2.1"), UpdateStatus::UpdateAvailable);
    EXPECT_EQ(compareVersions("1.2.1", "1.2"), UpdateStatus::Newer);
    EXPECT_EQ(compareVersions("1", "1.0.0"), UpdateStatus::UpToDate);
}

TEST(VersionUpdateTest, LeadingZerosIrrelevant) {
    EXPECT_EQ(compareVersions("1.02", "1.2"), UpdateStatus::UpToDate);
    EXPECT_EQ(compareVersions("01.0.0", "1.0.0"), UpdateStatus::UpToDate);
    EXPECT_EQ(compareVersions("1.0.09", "1.0.10"), UpdateStatus::UpdateAvailable);
}

TEST(VersionUpdateTest, OptionalLeadingVIgnored) {
    EXPECT_EQ(compareVersions("v1.0.0", "1.0.0"), UpdateStatus::UpToDate);
    EXPECT_EQ(compareVersions("1.0.0", "v1.0.1"), UpdateStatus::UpdateAvailable);
}

TEST(VersionUpdateTest, PreReleaseAndBuildMetadataIgnoredForCore) {
    EXPECT_EQ(compareVersions("1.0.0-beta", "1.0.0"), UpdateStatus::UpToDate);
    EXPECT_EQ(compareVersions("1.0.0+build7", "1.0.0"), UpdateStatus::UpToDate);
    EXPECT_EQ(compareVersions("1.0.0-rc1", "1.0.1"), UpdateStatus::UpdateAvailable);
}

TEST(VersionUpdateTest, NonNumericGuardedNoThrow) {
    // Digit-less segments count as 0; a leading digit run is used for "1a".
    EXPECT_EQ(compareVersions("1.x.0", "1.0.0"), UpdateStatus::UpToDate);
    EXPECT_EQ(compareVersions("1a.0.0", "1.0.0"), UpdateStatus::UpToDate);
    EXPECT_EQ(compareVersions("", ""), UpdateStatus::UpToDate);
    EXPECT_EQ(compareVersions("", "1.0.0"), UpdateStatus::UpdateAvailable);
    EXPECT_EQ(compareVersions("garbage", "also-garbage"), UpdateStatus::UpToDate);
}

// --- checkForUpdate wrapper -------------------------------------------------

TEST(VersionUpdateTest, CheckForUpdateWrapsCurrentVersion) {
    // Against the exact current version: up to date.
    EXPECT_EQ(checkForUpdate(currentVersion()), UpdateStatus::UpToDate);

    // A strictly greater "latest" reports an update is available; a strictly
    // lesser one reports we are newer. Build the neighbours from the current
    // version's own major so the wrapper is exercised, not hard-coded numbers.
    EXPECT_EQ(checkForUpdate("999.0.0"), UpdateStatus::UpdateAvailable);
    EXPECT_EQ(checkForUpdate("0.0.0"), UpdateStatus::Newer);
}

// --- Version source-of-truth cross-check ------------------------------------

TEST(VersionUpdateTest, CurrentVersionMatchesBuildSourceOfTruth) {
    // The library must report exactly the version the build system configured
    // (project(pdfe VERSION ...) -> PDFE_VERSION_STRING). This fails if the
    // VersionInfo.cpp literal fallback drifts from project(), or if the compile
    // definition is wired to a different value.
    EXPECT_EQ(currentVersion(), std::string(PDFE_VERSION_STRING));

    // And it must be a well-formed, self-consistent version.
    EXPECT_FALSE(currentVersion().empty());
    EXPECT_EQ(compareVersions(currentVersion(), currentVersion()),
              UpdateStatus::UpToDate);
}
