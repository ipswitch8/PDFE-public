// Standing controls that keep the build account name out of shipped binaries.
// GitHub #199 / #191. See docs/audit/dca/0111-*.md.
//
// WHAT THIS FILE IS FOR, AND WHAT IT DELIBERATELY DOES NOT DO
//
// The defect is a property of BUILD OUTPUT: compilers bake absolute source
// paths into binaries, and when the build tree sits under a user profile those
// paths carry the account name to every user who runs `strings` on the shipped
// product. #199 measured 418 occurrences across 6 shipped binaries.
//
// A gtest cannot observe that directly -- the occurrences exist in a .dll that
// may not have been built in this configuration, and asserting on it would make
// the test's result depend on which preset last ran. The scan over real bytes
// is therefore tools/gate/check_package_account_name.py, invoked on the
// extracted installer payload by scripts/verify-installer-no-account-name.ps1
// and wired into windows-validation.yml.
//
// What THIS file ratchets is the thing a byte-scan cannot: that each producer
// of such a path still has its mitigation, and that the scan is still wired in.
// Those are source-tree properties, so they are checkable here, cheaply, on
// every run -- and they are exactly what silently regresses. A reverted
// RUSTFLAGS line or a deleted workflow step reintroduces the whole defect while
// every existing test stays green, because no existing test's population
// includes build output (UiSuitePathHygiene and check_tracked_path_hygiene scan
// files under git control; .gitignore:8 excludes build/, so even #160's
// untracked-file widening cannot reach it).

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

fs::path repoRoot() { return fs::path(PDFE_REPO_ROOT); }

std::string readFile(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool contains(const std::string& hay, const std::string& needle)
{
    return hay.find(needle) != std::string::npos;
}

// Strip `#`-comments, line by line.
//
// WHY THIS IS NOT FUSSINESS. Every mitigation below sits under a comment block
// that NAMES the flag it is explaining -- deliberately, because the reasoning is
// worth more than the token. A guard that searches the whole file therefore
// passes on the comment alone, so deleting the functional line and leaving the
// explanation behind would keep this test green while the leak returned.
//
// That is not hypothetical: it was MEASURED. A mutation run against the first
// version of this file removed the real `OPENSSL_NO_FILENAMES` definition and
// the real `/PDBALTPATH:%_PDB%` flag, and both mutants SURVIVED, because each
// token also appears in the prose above its own line. "A grep hit is not a
// mechanism" is this project's standing phrasing for it.
//
// Both CMake and YAML use `#`, which is all this needs to handle. It is a
// deliberately dumb stripper: it does not know about `#` inside a quoted string,
// and it does not need to -- the markers it protects are flags and definitions,
// never string literals containing a hash.
std::string stripHashComments(const std::string& body)
{
    std::string out;
    out.reserve(body.size());
    std::istringstream in(body);
    std::string line;
    while (std::getline(in, line)) {
        const std::size_t h = line.find('#');
        if (h != std::string::npos) line.erase(h);
        out += line;
        out += '\n';
    }
    return out;
}

// One producer of an absolute build path inside a shipped artifact, and the
// mitigation that is supposed to stop it. Keyed by mechanism id from #199 so a
// reader can line this up against the issue.
struct Producer {
    const char* mechanism;   // #199's mechanism id
    const char* file;        // where the mitigation lives
    const char* marker;      // the token that must still be there
    const char* why;         // what breaks if it goes
};

const std::vector<Producer>& producers()
{
    static const std::vector<Producer> kProducers = {
        {"M4  rustc file!() under CARGO_HOME",
         "src/search/CMakeLists.txt",
         "--remap-path-prefix=",
         "rustc embeds file!() at every panic/unwrap site and this crate "
         "statically links 39 dependencies whose sources live under "
         "CARGO_HOME -- the build account's home directory. Measured 285 of "
         "the 418 occurrences, the single largest cluster, and the one a "
         "neutral build root cannot reach because the leak IS the home "
         "directory."},

        {"M1a OPENSSL_FILE in this project's own sources",
         "src/signatures/CMakeLists.txt",
         "OPENSSL_NO_FILENAMES",
         "OpenSSL 3 defines OPENSSL_malloc as "
         "CRYPTO_malloc(num, OPENSSL_FILE, OPENSSL_LINE), and OPENSSL_FILE is "
         "__FILE__ expanded in the CALLER's translation unit -- so each file "
         "here that allocates through those macros embeds its own absolute "
         "path. Measured 6 occurrences, exactly the 6 files in that target "
         "calling those macros."},

        {"M2  PDB path recorded in the image",
         "tools/vcpkg-triplets/x64-windows-repro.cmake",
         "/PDBALTPATH:%_PDB%",
         "link.exe records the PDB's ABSOLUTE path in the image. This is "
         "already closed (measured 0 occurrences) and stays closed only while "
         "the flag is in the triplet."},
    };
    return kProducers;
}

} // namespace

// --- Instance: the mitigations that closed pdfe.exe are still present -------
//
// RED BEFORE / GREEN AFTER: before this work, src/search/CMakeLists.txt invoked
// `cargo build --release` bare and src/signatures/CMakeLists.txt had no
// compile definition, so the first two rows below failed. Measured on the
// rebuilt binary afterwards: pdfe.exe went from 291 account-name strings to 0.
TEST(BuildOutputAccountName, EveryKnownProducerStillCarriesItsMitigation)
{
    for (const auto& p : producers()) {
        const fs::path abs = repoRoot() / p.file;
        const std::string body = readFile(abs);

        ASSERT_FALSE(body.empty())
            << "could not read " << p.file << " (resolved to " << abs.string()
            << "). A producer whose file cannot be read must not pass as "
               "mitigated -- that is how an unreadable input becomes a "
               "false green.";

        // Comments stripped: each of these files explains its own flag in the
        // prose above it, and matching that prose would credit a mitigation
        // that had been deleted. Measured -- see stripHashComments().
        EXPECT_TRUE(contains(stripHashComments(body), p.marker))
            << "mechanism " << p.mechanism << " has lost its mitigation.\n"
            << "  file   : " << p.file << "\n"
            << "  marker : " << p.marker << "\n"
            << "  why    : " << p.why << "\n"
            << "Removing this reintroduces GitHub #199 in shipped binaries "
               "while every other test stays green.";
    }
}

// --- Class-level: the byte-scan that catches the WHOLE family is wired in ---
//
// The family is "any build-time mechanism that embeds an absolute path into a
// shipped artifact". The instance test above enumerates the three mechanisms
// known TODAY, so by construction it cannot catch a fourth -- a newly added
// dependency, a new code generator, a port that starts embedding paths.
//
// What covers the open-ended family is a scan over the shipped bytes, which
// does not need to know the mechanism. This test asserts that scan still
// exists, still self-tests, and is still invoked on the installer payload --
// because a guard that is present but unexecuted is decoration, which this
// project has recorded happening (DCA 0106).
TEST(BuildOutputAccountName, ThePayloadScanExistsAndIsStillWiredIntoCi)
{
    const fs::path scanner = repoRoot() / "tools/gate/check_package_account_name.py";
    const fs::path driver  = repoRoot() / "scripts/verify-installer-no-account-name.ps1";
    const fs::path wf      = repoRoot() / ".github/workflows/windows-validation.yml";

    ASSERT_TRUE(fs::exists(scanner))
        << "the payload scanner is gone: " << scanner.string();
    ASSERT_TRUE(fs::exists(driver))
        << "the script that extracts the payload and runs the scanner is gone: "
        << driver.string();

    const std::string wfBody = readFile(wf);
    ASSERT_FALSE(wfBody.empty()) << "could not read " << wf.string();

    // Comments stripped here too: the step carries a comment block explaining
    // itself, and a commented-out invocation must not count as wired in.
    EXPECT_TRUE(contains(stripHashComments(wfBody),
                         "verify-installer-no-account-name.ps1"))
        << "windows-validation.yml no longer invokes the installer-payload "
           "account-name scan. The scanner can exist, pass its self-test, and "
           "still never run -- DCA 0106 is this project's record of guards "
           "that were present, referenced, and never executed.";

    // The scan must look at what SHIPS. Scoped to the build directory it would
    // fail permanently on pdfe_tests.exe (measured 850 occurrences, 531 of them
    // __FILE__ paths from this project's own test sources) -- correct for a
    // binary nobody distributes, and the fastest route to a disabled guard.
    const std::string driverBody = readFile(driver);
    ASSERT_FALSE(driverBody.empty()) << "could not read " << driver.string();
    EXPECT_TRUE(contains(driverBody, "check_package_account_name.py"))
        << "the driver script no longer calls the scanner.";
    EXPECT_TRUE(contains(driverBody, "bin\\pdfe.exe"))
        << "the driver no longer asserts the extracted payload actually "
           "contains pdfe.exe. Without that, a failed or partial extraction "
           "yields a near-empty tree and the scan passes trivially -- a "
           "0-occurrence report over a population of nothing.";
}

// --- The post-build scrub exists, is wired, and runs before packaging ------
//
// The scrub is what takes the last 2 occurrences out of Qt6Widgets.dll, which
// no rebuild on that host could reach (#272). Its POSITION is load-bearing in
// three separate ways, and all three are silent if broken:
//
//   * after the build, because the compiler is what bakes the strings in;
//   * before cpack, because cpack runs the install() rules that copy these
//     DLLs into the payload -- scrubbing afterwards leaves the packaged copies
//     dirty while the source tree looks clean;
//   * before any signing, because byte-patching a signed image invalidates it.
//
// So this asserts the invocation is present and that it precedes the cpack
// call in the same step, not merely that the file exists.
TEST(BuildOutputAccountName, TheScrubIsWiredAheadOfPackaging)
{
    const fs::path scrub = repoRoot() / "tools/gate/scrub_package_account_name.py";
    ASSERT_TRUE(fs::exists(scrub))
        << "the post-build scrub is gone: " << scrub.string();

    const fs::path wf = repoRoot() / ".github/workflows/windows-validation.yml";
    const std::string body = stripHashComments(readFile(wf));
    ASSERT_FALSE(body.empty()) << "could not read " << wf.string();

    const std::size_t scrubAt = body.find("scrub_package_account_name.py");
    ASSERT_NE(scrubAt, std::string::npos)
        << "windows-validation.yml no longer invokes the post-build scrub. "
           "Qt6Widgets.dll's 2 account-name paths return to the shipped "
           "package the moment this goes (GitHub #199 / #272), and the only "
           "thing that would notice is the payload scan.";

    const std::size_t cpackAt = body.find("cpack -C Release");
    ASSERT_NE(cpackAt, std::string::npos)
        << "could not find the cpack invocation, so the ordering this test "
           "exists to check cannot be evaluated. Do not treat that as a pass.";

    EXPECT_LT(scrubAt, cpackAt)
        << "the scrub runs AFTER cpack. cpack executes the install() rules "
           "that copy these DLLs into the payload, so a scrub that follows it "
           "cleans the source tree and ships the dirty copies -- a green "
           "source-tree scan over an unclean package.";
}

// --- The allowlist must not be able to wave the whole thing through ---------
//
// An allowlist is the obvious pressure valve, and an UNCAPPED entry is a
// permanent blind spot: the count behind it could go from 1 to 10,000 and the
// guard would keep passing. This asserts the file states that requirement, so
// the next person to add an entry meets it before the gate does.
TEST(BuildOutputAccountName, TheAllowlistIsEmptyOrEveryEntryIsCapped)
{
    const fs::path al = repoRoot() / "tools/gate/package_leak_allowlist.json";
    ASSERT_TRUE(fs::exists(al)) << "the allowlist file is gone: " << al.string();

    const std::string body = readFile(al);
    ASSERT_FALSE(body.empty()) << "could not read " << al.string();

    // Every non-underscore key is a real concession. Count them crudely: this
    // is a flat object, and the documentation keys are underscore-prefixed.
    std::size_t concessions = 0;
    std::size_t firstConcessionAt = std::string::npos;
    for (std::size_t i = 0; i + 1 < body.size(); ++i) {
        if (body[i] != '"') continue;
        // Start of a key is a quote preceded by '{' or ',' (modulo whitespace).
        std::size_t j = i;
        bool isKey = false;
        while (j-- > 0) {
            const char c = body[j];
            if (std::isspace(static_cast<unsigned char>(c))) continue;
            isKey = (c == '{' || c == ',');
            break;
        }
        if (!isKey) continue;
        if (body[i + 1] != '_') {
            ++concessions;
            if (firstConcessionAt == std::string::npos) firstConcessionAt = i;
        }
        // Skip to the end of this string so its contents are not rescanned.
        const std::size_t end = body.find('"', i + 1);
        if (end == std::string::npos) break;
        i = end;
    }

    if (concessions > 0) {
        // POSITIONAL, not match-anywhere. Asking only whether "max" appears
        // somewhere in the file is the same mistake the other case in this
        // file already had to have fixed: a token match that credits prose
        // rather than mechanism. The cap must appear AFTER the first real key,
        // i.e. plausibly inside a concession's own value -- otherwise one
        // sentence in the documentation keys above would silence this check,
        // and it only ever runs when a concession exists, which is precisely
        // when nobody is looking at it.
        //
        // LIMIT, stated rather than implied: this is positional, not a JSON
        // parse. A documentation key placed AFTER the first concession and
        // containing a bare "max" would still satisfy it. Closing that would
        // mean parsing each concession's own value object, which is more
        // machinery than a human-edited policy file of this size warrants --
        // but the limit is real, so it is written down instead of being left
        // for someone to discover. Verified by construction: an uncapped real
        // entry reddens this, a capped one does not, and an empty allowlist
        // passes.
        const std::size_t capAt = body.find("\"max\"", firstConcessionAt);
        EXPECT_NE(capAt, std::string::npos)
            << "the allowlist has " << concessions << " real entr(ies) and no "
               "\"max\" cap after the first of them. An uncapped allowlist "
               "entry concedes the disclosure AND hides any future increase in "
               "it: the count could go from 1 to 10,000 and this guard would "
               "keep passing.";
    }

    EXPECT_TRUE(contains(body, "_before_adding_an_entry"))
        << "the allowlist no longer states what adding an entry concedes. "
           "That note is the only thing standing between a failing scan and "
           "someone silencing it in one line.";
}
