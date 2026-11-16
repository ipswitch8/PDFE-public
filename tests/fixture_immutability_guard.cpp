// -*- coding: utf-8 -*-
// Fixture immutability guard (GitHub #386). See fixture_immutability_guard.h.
#include "fixture_immutability_guard.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <sstream>
#include <system_error>

namespace pdfe_test {

FixtureSnapshot snapshotFixtures(const std::filesystem::path& root)
{
    FixtureSnapshot snap;
    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec)) return snap;
    std::filesystem::recursive_directory_iterator it(
        root, std::filesystem::directory_options::skip_permission_denied, ec);
    for (const std::filesystem::recursive_directory_iterator end; !ec && it != end;
         it.increment(ec)) {
        std::error_code fec;
        if (!it->is_regular_file(fec) || fec) continue;
        FixtureStamp st;
        st.size = it->file_size(fec);
        if (fec) continue;
        st.mtime = it->last_write_time(fec);
        if (fec) continue;
        snap.emplace(std::filesystem::relative(it->path(), root, fec).generic_string(), st);
    }
    return snap;
}

std::vector<std::string> diffFixtures(const FixtureSnapshot& before,
                                      const FixtureSnapshot& after)
{
    std::vector<std::string> out;
    for (const auto& [path, stamp] : before) {
        auto f = after.find(path);
        if (f == after.end())          out.push_back("deleted: " + path);
        else if (!(f->second == stamp)) out.push_back("modified: " + path);
    }
    for (const auto& [path, stamp] : after) {
        if (!before.count(path)) out.push_back("created: " + path);
    }
    std::sort(out.begin(), out.end());
    return out;
}

namespace {

class FixtureImmutabilityListener : public ::testing::EmptyTestEventListener {
public:
    explicit FixtureImmutabilityListener(std::filesystem::path root)
        : root_(std::move(root)), baseline_(snapshotFixtures(root_)) {}

    // gtest's TestEventRepeater delivers *End events to listeners in REVERSE
    // registration order (gtest.cc, TestEventRepeater::OnTestEnd), so this runs
    // before the default result printer, and TestInfo::Run clears
    // current_test_info() only after OnTestEnd returns. A failure added here is
    // therefore recorded against the test that just ran. That is pinned in a
    // real run by FixtureImmutabilityGuardInstall.RealRunFailsTheTestThatTouchedAFixture,
    // not just by calling OnTestEnd directly.
    void OnTestEnd(const ::testing::TestInfo&) override {
        FixtureSnapshot now = snapshotFixtures(root_);
        std::vector<std::string> diff = diffFixtures(baseline_, now);
        if (diff.empty()) return;
        std::ostringstream msg;
        msg << "this test changed tracked fixtures under " << root_.generic_string()
            << " (GitHub #386). Work on a temp copy instead:";
        for (const auto& d : diff) msg << "\n    " << d;
        ADD_FAILURE() << msg.str();
        // Re-baseline so only the test that caused the change fails, not every
        // test after it. The damage is still on disk; the message says where.
        baseline_ = std::move(now);
    }

private:
    std::filesystem::path root_;
    FixtureSnapshot baseline_;
};

}  // namespace

std::unique_ptr<::testing::TestEventListener>
makeFixtureImmutabilityListener(const std::filesystem::path& fixturesRoot)
{
    return std::make_unique<FixtureImmutabilityListener>(fixturesRoot);
}

namespace {
bool g_installed = false;
std::filesystem::path g_root;
}  // namespace

void installFixtureImmutabilityGuard(const std::filesystem::path& fixturesRoot)
{
    g_root = fixturesRoot;
    if (const char* over = std::getenv("PDFE_FIXTURE_GUARD_ROOT"); over && *over) {
        g_root = over;
    }
    // gtest takes ownership of the raw pointer.
    ::testing::UnitTest::GetInstance()->listeners().Append(
        makeFixtureImmutabilityListener(g_root).release());
    g_installed = true;
}

bool fixtureGuardInstalled() { return g_installed; }

std::filesystem::path fixtureGuardRoot() { return g_root; }

}  // namespace pdfe_test
