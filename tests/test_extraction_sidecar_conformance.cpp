// -*- coding: utf-8 -*-
//
// PDF-79 (meta-audit-2026-07 gap register) — wire validate_sidecars.py into
// the suite.
//
// tests/fixtures/*.expected.json are the structural claims made about each
// fixture: page count, encryption, whether it has a form, its PDF version,
// its size. Tests read them. `tests/fixtures/validate_sidecars.py` opens each
// PDF and checks the claims are true. It has existed since pipeline a7c2e541
// phase-3 and NOTHING RAN IT, so the sidecars were unverified assertions —
// exactly the shape of the proxy-oracle problem this project has been working
// through, one level up: the fixtures' metadata was trusted rather than
// checked.
//
// WHAT WIRING IT UP IMMEDIATELY FOUND. On first run the script exited 1 with
// 197 of 278 fixtures reported "sidecar missing" — it had never been green.
// Two causes, and the second is the interesting one:
//
//   1. 180 fixtures added after phase-3 simply never had sidecars generated.
//   2. `generate_existing_sidecars.py` skipped the whole `realworld/`
//      directory under the comment "already has sidecars". That was true of
//      the twelve fixtures it was written about, all of which live in
//      SUBDIRECTORIES of realworld/, and false of the seventeen PDFs later
//      added at the top of it. The exclusion was keyed on a directory; the
//      fact it encoded was about a list of files. Both are fixed; the
//      generator now relies on the sidecar-exists check it already had.
//
// This file is deliberately TWO independent halves, because a shim that only
// shells out inherits every blind spot of the thing it shells out to:
//
//   * EverySidecarMatchesItsFixture runs the script and requires exit 0.
//   * EveryFixturePdfHasASidecar walks the fixture tree in C++ and requires
//     the pairing directly. If the script's own collector ever silently stops
//     yielding a directory — which is precisely how cause (2) above survived —
//     the script still exits 0 and this case is what notices.

#include <gtest/gtest.h>

#include <array>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::filesystem::path fixturesDir()
{
    return std::filesystem::path(TEST_FIXTURES_DIR);
}

// Run `cmd` with stderr folded into stdout; return the exit code and fill
// `output`. Uses _popen so the child's diagnostics land in the gtest failure
// message rather than in a console nobody reads during a sharded run.
int runCapturing(const std::string& cmd, std::string& output)
{
    output.clear();
#if defined(_WIN32)
    FILE* pipe = ::_popen(cmd.c_str(), "r");
#else
    FILE* pipe = ::popen(cmd.c_str(), "r");
#endif
    if (!pipe) return -1;

    std::array<char, 4096> buf{};
    while (std::fgets(buf.data(), static_cast<int>(buf.size()), pipe)) {
        output += buf.data();
    }
#if defined(_WIN32)
    return ::_pclose(pipe);
#else
    return ::pclose(pipe);
#endif
}

std::string quoted(const std::string& s)
{
    return "\"" + s + "\"";
}

// Directories under fixtures/ that hold working copies or generated output
// rather than corpus fixtures, and so are not expected to carry sidecars.
// Listed explicitly: an empty-by-default filter would let a whole tree drop
// out of coverage without anyone choosing that.
bool isExcludedDir(const std::filesystem::path& p)
{
    static constexpr std::string_view kExcluded[] = {
        "__pycache__",
        // BatchEngine writes its results to "<input parent>/batch_output",
        // and several suites point it at the fixtures tree, so PDFs land here
        // that are test OUTPUT rather than corpus. The directory is gitignored
        // (.gitignore:153) — nothing states what those files should contain
        // because nothing put them there deliberately.
        "batch_output",
        "output",
        "tmp",
        "work",
    };
    for (const auto& part : p) {
        const std::string name = part.string();
        for (auto ex : kExcluded) {
            if (name == ex) return true;
        }
    }
    return false;
}

} // namespace

// The script itself, as the register asked: run it, require it to pass.
TEST(ExtractionSidecarConformance, EverySidecarMatchesItsFixture)
{
    const std::string python = PDFE_PYTHON_EXECUTABLE;
    ASSERT_FALSE(python.empty())
        << "no Python 3 interpreter was found at configure time, so the "
           "sidecar conformance check cannot run. This is a hard failure "
           "rather than a skip: a build that cannot verify its fixtures' "
           "metadata must not report that the metadata is verified.";

    const auto script = fixturesDir() / "validate_sidecars.py";
    ASSERT_TRUE(std::filesystem::exists(script))
        << "validate_sidecars.py is missing: " << script.string();

    // PYTHONIOENCODING is set because fixture descriptions contain non-ASCII
    // and the child's default console codepage would otherwise abort the
    // script on a UnicodeEncodeError — a failure that looks like a
    // conformance failure and is not one.
    const std::string cmd =
        "set PYTHONIOENCODING=utf-8 && " +
        quoted(python) + " " + quoted(script.string()) + " 2>&1";

    std::string output;
    const int rc = runCapturing(cmd, output);

    EXPECT_EQ(rc, 0)
        << "validate_sidecars.py reported a mismatch between a fixture and "
           "its .expected.json. Each line below is fixture::field with the "
           "claimed and actual values.\n\n"
        << output;

    // Exit 0 is necessary but not sufficient: a collector that yielded
    // nothing would also exit 0 having validated nothing.
    EXPECT_NE(output.find("All sidecars match their fixtures."),
              std::string::npos)
        << "the script exited 0 without reaching its success line; it may "
           "have terminated early.\n\n" << output;

    const auto pos = output.find("Validating ");
    ASSERT_NE(pos, std::string::npos) << output;
    const int validated = std::atoi(output.c_str() + pos + 11);
    EXPECT_GT(validated, 200)
        << "the script validated only " << validated << " fixtures; the "
           "corpus is larger than that, so its collector is skipping a tree.";
}

// The independent half. Nothing here consults the script or its output.
TEST(ExtractionSidecarConformance, EveryFixturePdfHasASidecar)
{
    std::vector<std::string> orphans;
    int examined = 0;

    std::error_code ec;
    for (std::filesystem::recursive_directory_iterator
             it(fixturesDir(),
                std::filesystem::directory_options::skip_permission_denied, ec),
             end;
         it != end; it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        if (!it->is_regular_file(ec)) continue;

        const auto& p = it->path();
        if (p.extension() != ".pdf") continue;
        if (isExcludedDir(std::filesystem::relative(p, fixturesDir(), ec))) continue;

        ++examined;
        if (!std::filesystem::exists(p.parent_path() /
                                     (p.stem().string() + ".expected.json"))) {
            orphans.push_back(
                std::filesystem::relative(p, fixturesDir(), ec).string());
        }
    }

    EXPECT_GT(examined, 200)
        << "the fixture walk found only " << examined << " PDFs; this case "
           "would pass trivially on a tree it cannot see";

    std::string joined;
    for (const auto& o : orphans) joined += "  " + o + "\n";
    EXPECT_TRUE(orphans.empty())
        << orphans.size() << " fixture PDF(s) have no .expected.json "
           "sidecar, so nothing states what they are supposed to contain and "
           "validate_sidecars.py cannot check them:\n"
        << joined
        << "\nRegenerate with:\n"
           "  python tests/fixtures/coverage_inventory.py\n"
           "  python tests/fixtures/generate_existing_sidecars.py";
}
