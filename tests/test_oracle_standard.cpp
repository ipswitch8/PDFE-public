// tests/test_oracle_standard.cpp
//
// SC-b enforcement inside the gtest suite (meta-audit gap A3-41, with A3-55 and
// A3-56 registering the smell classes the phase-5 capstone dropped).
//
// `docs/harness-audit/standing-controls.md` §SC-b asked for "a written,
// lint-enforceable standard for what counts as an oracle". The meta-audit found
// it ASPIRATIONAL: no document, no linter, nothing in CI reading a test body — so
// R-03/R-14/R-16/R-18/R-19/R-20/R-21 all had zero recurrence prevention.
//
// The standard is docs/audit/oracle-standard.md. It is enforced twice, on
// purpose, because a control that only runs in one place is one deletion away
// from gone:
//   * tests/scripts/lint_oracle_standard.py — CTest target `lint_oracle_standard`
//     (pure Python, runnable per-commit with no C++ build);
//   * this file — the same rules re-derived INDEPENDENTLY in C++ inside the gtest
//     suite, so the standard is enforced by the suite the project actually runs.
//
// These guards read the TREE. They are not assertions about the document.

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace {

fs::path repoRoot() { return fs::path(PDFE_REPO_ROOT); }
fs::path testsDir() { return repoRoot() / "tests"; }

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

/// Blank out // comments, /* */ comments and "string literals", preserving
/// length and newlines so line numbers stay correct. Without this, a file that
/// QUOTES a bad shape in a comment to explain what it replaced would be reported
/// as a violation — which several files in this suite now legitimately do.
///
/// RAW STRING LITERALS are handled explicitly, and that is not a refinement —
/// without it this function DESYNCHRONISES. `R"(...)"` content is not escaped,
/// so a regex literal like R"([^\s"'<>]*)" contains a bare `"` that ends the
/// Str state early and a bare `'` that then opens Chr state, which runs to the
/// next apostrophe ANYWHERE in the file, blanking real code in between.
///
/// Measured on this tree when `test_ui_suite_path_hygiene.cpp` was added: 76
/// raw strings across the suite, **18 of them containing a `"` in 8 files and
/// 13 containing a `'` in 3 files.** Every one of those files was being scanned
/// against partially-blanked text, so OS-2 onward were reading corrupted input
/// for them. It surfaced as an OS-1 false positive on the new file only because
/// that file's apostrophes happened to swallow every one of its assertions —
/// a loud symptom of a fault that had been silent in seven other files.
std::string stripNoise(const std::string& src) {
    std::string out = src;
    enum class St { Code, Line, Block, Str, Chr };
    St st = St::Code;
    const auto isIdentChar = [](char ch) {
        return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
               (ch >= '0' && ch <= '9') || ch == '_';
    };
    for (size_t i = 0; i < out.size(); ++i) {
        const char c = out[i];
        const char n = (i + 1 < out.size()) ? out[i + 1] : '\0';
        switch (st) {
            case St::Code:
                if (c == '/' && n == '/') { st = St::Line; out[i] = out[i + 1] = ' '; ++i; }
                else if (c == '/' && n == '*') { st = St::Block; out[i] = out[i + 1] = ' '; ++i; }
                else if (c == 'R' && n == '"' &&
                         (i == 0 || !isIdentChar(out[i - 1]))) {
                    // R"delim( ... )delim" — delimiter is at most 16 chars and
                    // excludes whitespace, parens and backslash (C++ [lex.string]).
                    size_t j = i + 2;
                    std::string delim;
                    while (j < out.size() && out[j] != '(' && delim.size() < 16 &&
                           !std::isspace(static_cast<unsigned char>(out[j])) &&
                           out[j] != ')' && out[j] != '\\') {
                        delim.push_back(out[j]);
                        ++j;
                    }
                    if (j < out.size() && out[j] == '(') {
                        const std::string closer = ")" + delim + "\"";
                        const size_t endPos = out.find(closer, j + 1);
                        const size_t stop = (endPos == std::string::npos)
                                                ? out.size()
                                                : endPos + closer.size();
                        for (size_t k = i; k < stop; ++k) {
                            if (out[k] != '\n') out[k] = ' ';
                        }
                        i = stop - 1;  // loop's ++i lands just past the literal
                    } else {
                        // Not a raw string after all (e.g. an identifier ending
                        // in R followed by a normal string); fall back.
                        st = St::Str;
                        out[i + 1] = ' ';
                        ++i;
                    }
                }
                else if (c == '"') { st = St::Str; out[i] = ' '; }
                else if (c == '\'') { st = St::Chr; out[i] = ' '; }
                break;
            case St::Line:
                if (c == '\n') st = St::Code; else out[i] = ' ';
                break;
            case St::Block:
                if (c == '*' && n == '/') { out[i] = out[i + 1] = ' '; ++i; st = St::Code; }
                else if (c != '\n') out[i] = ' ';
                break;
            case St::Str:
                if (c == '\\') { out[i] = ' '; if (i + 1 < out.size() && out[i + 1] != '\n') out[++i] = ' '; }
                else if (c == '"') { out[i] = ' '; st = St::Code; }
                else if (c != '\n') out[i] = ' ';
                break;
            case St::Chr:
                if (c == '\\') { out[i] = ' '; if (i + 1 < out.size() && out[i + 1] != '\n') out[++i] = ' '; }
                else if (c == '\'') { out[i] = ' '; st = St::Code; }
                else if (c != '\n') out[i] = ' ';
                break;
        }
    }
    return out;
}

std::vector<fs::path> testSources() {
    std::vector<fs::path> v;
    for (const auto& e : fs::directory_iterator(testsDir())) {
        if (!e.is_regular_file()) continue;
        const std::string name = e.path().filename().string();
        if (name.rfind("test_", 0) == 0 && e.path().extension() == ".cpp")
            v.push_back(e.path());
    }
    std::sort(v.begin(), v.end());
    return v;
}

int lineOf(const std::string& s, size_t pos) {
    return 1 + static_cast<int>(std::count(s.begin(), s.begin() + pos, '\n'));
}

/// Names of every TEST/TEST_F/TEST_P whose body contains SUCCEED( and no
/// EXPECT_/ASSERT_ at all.
std::set<std::string> succeedOnlyBodies() {
    std::set<std::string> names;
    const std::regex testRe(
        R"(\bTEST(?:_F|_P)?\s*\(\s*([A-Za-z_0-9]+)\s*,\s*([A-Za-z_0-9]+)\s*\)\s*\{)");
    const std::regex assertRe(R"(\b(?:EXPECT|ASSERT)_)");
    for (const auto& p : testSources()) {
        const std::string src = stripNoise(readFile(p));
        for (auto it = std::sregex_iterator(src.begin(), src.end(), testRe);
             it != std::sregex_iterator(); ++it) {
            const size_t start = static_cast<size_t>(it->position()) +
                                 static_cast<size_t>(it->length());
            int depth = 1;
            size_t i = start;
            while (i < src.size() && depth) {
                if (src[i] == '{') ++depth;
                else if (src[i] == '}') --depth;
                ++i;
            }
            const std::string body = src.substr(start, i - start);
            if (body.find("SUCCEED(") != std::string::npos &&
                !std::regex_search(body, assertRe))
                names.insert((*it)[1].str() + "." + (*it)[2].str());
        }
    }
    return names;
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
// The scanner's own oracle. Every rule below reads stripNoise()'s output, so a
// silent fault in it degrades ALL of them at once — and that is not
// hypothetical: it happened, and went unnoticed in seven files.
//
// Raw string literals carry unescaped `"` and `'`. Before this was handled, a
// regex literal like R"([^\s"'<>]*)" ended the string state at its inner quote
// and opened a CHARACTER-literal state at its apostrophe, which then ran to the
// next apostrophe anywhere in the file, blanking real code between them.
// OS-1 through OS-4 were reading that corrupted text.
//
// It surfaced as an OS-1 false positive on one new file whose apostrophes
// happened to swallow every assertion it had. The other seven files were mis-
// scanned in silence. Re-running the full rule set after the fix produced NO
// new violations, so nothing real had been hidden — but that is a fact to be
// measured, not assumed, and it is why this case now exists.
// ═══════════════════════════════════════════════════════════════════════════
TEST(OracleStandard, StripNoiseHandlesRawStringLiteralsWithoutDesynchronising) {
    // The exact shape that broke it: a quote AND an apostrophe inside a raw
    // string, with an assertion after it and an apostrophe later in the file.
    const std::string src =
        "void f() {\n"
        "    const std::regex re(R\"([^\\s\"'<>|*?]*)\");\n"
        "    EXPECT_EQ(1, 1);\n"
        "}\n"
        "// don't let this apostrophe matter\n";
    const std::string out = stripNoise(src);

    EXPECT_NE(out.find("EXPECT_EQ"), std::string::npos)
        << "the assertion after a raw string was blanked, so every rule in "
           "this file would read the surrounding code as empty. Output was:\n"
        << out;

    // Line numbering is load-bearing: violations are reported by line.
    EXPECT_EQ(out.size(), src.size()) << "stripNoise must preserve length";
    EXPECT_EQ(std::count(out.begin(), out.end(), '\n'),
              std::count(src.begin(), src.end(), '\n'))
        << "stripNoise must preserve newlines or reported line numbers drift";

    // The literal's own content must still be gone — a scanner that stopped
    // blanking raw strings would report shapes that are quoted, not written.
    EXPECT_EQ(out.find("[^\\s"), std::string::npos)
        << "raw string content survived; quoted shapes would be misread as "
           "real code. Output was:\n"
        << out;

    // The neighbouring cases must not have regressed.
    EXPECT_EQ(stripNoise("EXPECT_TRUE(x); // EXPECT_FALSE(y)").find("FALSE"),
              std::string::npos)
        << "line comments must still be blanked";
    EXPECT_EQ(stripNoise("const char* s = \"EXPECT_FALSE(y)\";").find("FALSE"),
              std::string::npos)
        << "plain string literals must still be blanked";
    EXPECT_NE(stripNoise("const char* s = \"x\"; EXPECT_EQ(1, 1);")
                  .find("EXPECT_EQ"),
              std::string::npos)
        << "code after a plain string must survive";
}

// ═══════════════════════════════════════════════════════════════════════════
// F65 (follow-up phase-67) — a shared harness helper may have exactly ONE
// definition.
//
// `applyProtectedDacl`, `currentUserSidString` and `daclSddl` were each defined
// in tests/support/ AND copied into individual test files. Two implementations
// of "read/apply a DACL" in one suite is the sibling-drift shape DCA 0015
// records, and these had already drifted in TEXT.
//
// WHY THIS GUARD IS A COUNT AND NOT A CODE REVIEW. The register said TWO
// duplicated symbols. The reverification corrected it to THREE. Enumerating
// definitions across the whole tests/ tree found FOUR, in three files —
// including one in test_full_rewrite_staging_hardening.cpp that neither count
// knew about. Both numbers were arrived at by reading; neither by counting.
// So the count is mechanical now, and it is the count that is asserted.
//
// It reuses stripNoise() deliberately: a definition quoted inside a comment
// (this one quotes all three names) must not be counted, which is exactly the
// blind spot that made the phase-66 workflow guard match its own comment.
// ═══════════════════════════════════════════════════════════════════════════
/// Does `src` contain a column-0 DEFINITION of `name`? Two shapes, because this
/// detector used to know only one of them.
///
/// GitHub #275: it used to require `<name>(` AND a line beginning with one of
/// `std::string`/`bool`/`void`/`int`. A TYPE has neither — `struct
/// DirectoryAttacker {` has no parenthesis and no return type — so a duplicated
/// struct was invisible, and adding its name to `kShared` would have produced
/// GREEN. That is worse than the silence it replaced, because green reads as
/// coverage.
///
/// The function branch also lost that four-prefix allowlist. Enumerating the
/// return types somebody might use is the same hand-made-population defect one
/// level down; `readAll` returning `std::set<std::string>` would have been missed.
///
/// EXTRACTED TO NAMESPACE SCOPE so a test can drive it against a tree it builds
/// itself. It was a lambda inside the guard, which meant the guard could only ever
/// be exercised against the real `tests/` directory — i.e. it could be observed
/// green and never shown capable of anything else. The phase-5 karen gate failed
/// the phase for exactly that. See
/// TheSharedHelperDetectorFindsAReintroducedDuplicate below.
/// Starting at the `(` of a parameter list, balance parentheses ACROSS LINES
/// and report whether what follows opens a body.
///
/// GitHub #275 C5. The mirror of `_opens_a_body` in
/// tools/gate/check_duplicate_test_definitions.py. Checking the signature's
/// first line for `;` is not enough: a declaration whose parameters wrap puts
/// its `;` several lines down, so a first-line check calls it a definition.
///
/// `{` opens a body and `:` is a constructor initialiser list (also a
/// definition). `;` ends a declaration. Comments are skipped so a trailing
/// `// note` cannot be mistaken for the next significant token. The look-ahead
/// is bounded, because an unbalanced `(` in an unclosed `#if` arm must not run
/// to the end of the file.
bool opensABodyAfterParams(const std::vector<std::string>& lines, size_t start,
                           size_t openParen)
{
    const size_t limit = std::min(lines.size(), start + 40);
    int depth = 0;
    bool closed = false;
    for (size_t li = start; li < limit; ++li) {
        const std::string& l = lines[li];
        size_t ci = (li == start) ? openParen : 0;
        for (; ci < l.size(); ++ci) {
            // Stop at a line comment; nothing after it is a token.
            if (l[ci] == '/' && ci + 1 < l.size() && l[ci + 1] == '/') break;
            if (!closed) {
                if (l[ci] == '(') ++depth;
                else if (l[ci] == ')') {
                    if (--depth == 0) closed = true;
                }
                continue;
            }
            // Past the closing `)`: the first significant character decides.
            const char c = l[ci];
            if (c == ' ' || c == '\t' || c == '\r') continue;
            if (c == '{' || c == ':') return true;
            if (c == ';') return false;
            // Anything else (`const`, `noexcept`, a trailing return type, an
            // attribute) is part of the signature; keep looking.
        }
    }
    // Ran out of look-ahead without finding either. Treat as NOT a definition:
    // a guard that counts an unresolved shape as a duplicate fires on correct
    // code, which is the failure this whole comparison was written to stop.
    return false;
}

bool definesHelperAtColumnZero(const std::string& src, std::string_view name)
{
    const std::string fnDecl = std::string(name) + "(";
    std::vector<std::string> lines;
    {
        std::istringstream in(src);
        std::string l;
        while (std::getline(in, l)) {
            if (!l.empty() && l.back() == '\r') l.pop_back();
            lines.push_back(l);
        }
    }
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string& line = lines[i];
        if (line.empty() || line[0] == ' ' || line[0] == '\t') continue;

        // TYPE definition: `struct X {`, `class X : public Y {`,
        // `enum class X {`, `union X {`. A trailing `;` with no `{` is a
        // forward declaration, which every header is entitled to have.
        for (const char* kw : {"struct ", "class ", "union ",
                               "enum class ", "enum "}) {
            if (line.rfind(kw, 0) != 0) continue;
            std::string rest = line.substr(std::strlen(kw));
            const size_t ns = rest.find_first_not_of(" \t");
            if (ns == std::string::npos) continue;
            rest = rest.substr(ns);
            if (rest.rfind(name, 0) != 0) continue;
            // The name must END here, or `DirectoryAttackerHelper` would match
            // `DirectoryAttacker`.
            const char after = rest.size() > name.size()
                                   ? rest[name.size()] : ' ';
            if (std::isalnum(static_cast<unsigned char>(after))
                || after == '_') {
                continue;
            }
            if (line.find('{') == std::string::npos
                && line.find(';') != std::string::npos) {
                continue;           // forward declaration
            }
            return true;
        }

        // FUNCTION definition.
        const size_t fnPos = line.find(fnDecl);
        if (fnPos == std::string::npos) continue;
        // GitHub #275 C5. This used to be `if (line.find(';') != npos) continue;`
        // -- i.e. look for a `;` on the FIRST line only. That reads a
        // multi-line DECLARATION as a definition, because its `;` is several
        // lines below the name:
        //
        //     KilledSaveResult killChildInsideSaveWindow(
        //         const std::filesystem::path& sourcePdf,
        //         ...
        //         std::chrono::milliseconds waitForWindow = ...);
        //
        // That is not hypothetical -- it is tests/support/SaveFailureInjection.h
        // :312-316. A helper declared in a header and defined in a .cpp would
        // then be counted TWICE and the guard would fire on correct code, which
        // is how a guard loses its audience.
        //
        // The PYTHON twin hit this on its first run and was fixed by balancing
        // the parameter list (_opens_a_body). This mirror never got that fix,
        // and nothing compared the two until the shared corpus did. So: balance
        // the parameters, then look at what actually FOLLOWS -- `{` opens a
        // body, `:` is a constructor initialiser list (also a definition), `;`
        // ends a declaration.
        if (!opensABodyAfterParams(lines, i, fnPos + fnDecl.size() - 1)) {
            continue;
        }
        return true;
    }
    return false;
}

TEST(OracleStandard, SharedHarnessHelpersHaveExactlyOneDefinition)
{
    static constexpr std::string_view kShared[] = {
        "applyProtectedDacl", "currentUserSidString", "daclSddl",
        // GitHub #275. A ~155-line thread-spawning Win32 attacker that backed 20
        // TOCTTOU publish tests from TWO definitions. Adding the name was not
        // enough on its own: see the TYPE branch below for why this guard could
        // not have seen it even when named.
        "DirectoryAttacker",
    };

    int filesScanned = 0;
    for (auto name : kShared) {
        auto definesIt = [name](const std::string& src) {
            return definesHelperAtColumnZero(src, name);
        };

        std::vector<std::string> found;
        for (const auto& p : testSources()) {
            const std::string src = stripNoise(readFile(p));
            if (definesIt(src)) {
                found.push_back(p.filename().string());
            }
        }
        // tests/support/ is not under testsDir()'s direct iteration, so scan it
        // explicitly — the single legitimate home must be counted, or "one
        // definition" would be satisfied by having none at all.
        //
        // GitHub #275: this used to name ONE file, support/SaveFailureInjection.cpp.
        // That was enough while every shared helper was a function in that .cpp, and
        // wrong the moment one was a TYPE: a struct's home is a HEADER, and seven of
        // the thirteen files in tests/support/ are header-only. A hardcoded filename
        // here would have reported DirectoryAttacker as having ZERO definitions
        // after it was moved to support/DirectoryAttacker.h — failing for the
        // opposite reason, which is no better.
        const auto supportDir = testsDir() / "support";
        if (std::filesystem::is_directory(supportDir)) {
            std::vector<std::filesystem::path> supportFiles;
            for (const auto& e : std::filesystem::directory_iterator(supportDir)) {
                if (!e.is_regular_file()) continue;
                const auto ext = e.path().extension().string();
                if (ext == ".cpp" || ext == ".h" || ext == ".hpp") {
                    supportFiles.push_back(e.path());
                }
            }
            // Sorted so the failure message is stable run to run.
            std::sort(supportFiles.begin(), supportFiles.end());
            for (const auto& sp : supportFiles) {
                const std::string src = stripNoise(readFile(sp));
                if (definesIt(src)) {
                    found.push_back("support/" + sp.filename().string());
                }
            }
            EXPECT_FALSE(supportFiles.empty())
                << "tests/support/ holds no .cpp/.h at all, so the legitimate-home "
                   "half of this guard is enumerating nothing and would accept a "
                   "helper that lives nowhere.";
        }

        EXPECT_EQ(found.size(), 1u)
            << "`" << name << "` has " << found.size()
            << " definition(s); a shared harness helper must have exactly one, "
               "in tests/support/. Found in:"
            << [&] {
                   std::string s;
                   for (const auto& f : found) s += "\n    " + f;
                   return s.empty() ? std::string("\n    (none)") : s;
               }();
        // GitHub #273: this was `if (found.size() == 1)` gating the location
        // check, so when a helper had TWO definitions — the very state this
        // guard exists to catch — neither of them was checked for being in
        // the shared harness. The count assertion above already reports that
        // case, but the location of each copy went unexamined.
        //
        // Deliberately NOT ASSERT_: this sits inside a loop over every helper
        // name, and a fatal assertion would abort on the first bad one and
        // stop reporting the rest. Iterating instead checks EVERY definition
        // found, on every path — zero, one or several — with no condition on
        // a count.
        //
        // GitHub #275: the required location was the literal
        // "support/SaveFailureInjection.cpp". The contract is "it lives in the
        // shared harness", not "it lives in that one file" — and a TYPE belongs in
        // a header, so pinning one .cpp would make a correctly-placed struct fail.
        for (const auto& f : found) {
            EXPECT_EQ(f.rfind("support/", 0), 0u)
                << "`" << name << "` has a definition in " << f
                << ", not under tests/support/. One copy in the wrong place is "
                   "the state this guard exists to leave behind, not reach.";
        }
    }

    filesScanned = static_cast<int>(testSources().size());
    EXPECT_GT(filesScanned, 100)
        << "only " << filesScanned << " test sources enumerated; the sweep is "
           "not reaching the tree and this guard would pass vacuously.";
}

// ───────────────────────────────────────────────────────────────────────────
// The guard above, shown capable of failing. (GitHub #275)
//
// WHY THIS EXISTS. #275's second acceptance criterion asks for "a new guard test
// [proving] the shared-helper duplicate-detection guard fails when a duplicate is
// reintroduced". That was first done BY HAND — a duplicate struct pasted into
// test_publish_handle_hardening_phase59.cpp, both guards observed to redden, then
// reverted. The phase-5 karen gate failed the phase for it, correctly: a
// hand-performed mutation proves the guard worked ONCE, on one afternoon, and
// leaves nothing in the tree to repeat the check against a future regression.
//
// So the detector is exercised here against trees these cases build themselves.
// That is also why `definesHelperAtColumnZero` is at namespace scope rather than a
// lambda inside the guard: as a lambda it could only ever be pointed at the real
// tests/ directory, which means it could be observed green and never shown capable
// of anything else.
//
// WHAT THIS DOES NOT CLAIM. These cases exercise the DETECTOR, not the guard's file
// enumeration. The enumeration has its own non-vacuity assertions above (>100 test
// sources, and a non-empty tests/support/). Both halves need to hold; neither
// subsumes the other.
// ───────────────────────────────────────────────────────────────────────────
TEST(OracleStandard, TheSharedHelperDetectorFindsAReintroducedDuplicate)
{
    // The exact shape that was invisible before #275: a TYPE, which has neither a
    // `(` nor one of the four return-type prefixes the old matcher required.
    const std::string copy =
        "struct DirectoryAttacker {\n"
        "    std::atomic<bool> stop{false};\n"
        "    void start() {}\n"
        "};\n";

    EXPECT_TRUE(definesHelperAtColumnZero(copy, "DirectoryAttacker"))
        << "the detector cannot see a column-0 `struct DirectoryAttacker {` at all, "
           "so the guard above would report ZERO definitions for it and pass — the "
           "state GitHub #275 was filed about.";

    // Two files each carrying it is the state the guard exists to catch. Counting
    // the way the guard counts, so this case fails if the detector stops agreeing
    // with itself across sources.
    const std::vector<std::string> sources = {copy, copy};
    int defining = 0;
    for (const auto& src : sources) {
        if (definesHelperAtColumnZero(src, "DirectoryAttacker")) ++defining;
    }
    EXPECT_EQ(defining, 2)
        << "a duplicate reintroduced into two sources was counted " << defining
        << " time(s), not 2, so the guard's `found.size() == 1` assertion would not "
           "fire on it.";
}

TEST(OracleStandard, TheSharedHelperDetectorIsNotSimplyAlwaysTrue)
{
    // Without this, the case above is satisfied by a detector that returns true for
    // everything — which would make the guard permanently red rather than
    // permanently green, and equally useless. DCA 0047's lesson: a check that can
    // ONLY redden is as dead as one that can only pass.
    EXPECT_FALSE(definesHelperAtColumnZero(
        "struct SomethingElse {\n};\n", "DirectoryAttacker"))
        << "a differently-named struct matched";

    EXPECT_FALSE(definesHelperAtColumnZero(
        "struct DirectoryAttacker;\n", "DirectoryAttacker"))
        << "a forward DECLARATION was counted as a definition; every header is "
           "entitled to one, and counting them makes the guard fire on correct code";

    EXPECT_FALSE(definesHelperAtColumnZero(
        "    struct DirectoryAttacker {\n    };\n", "DirectoryAttacker"))
        << "an INDENTED definition matched. That is outside this guard's stated "
           "scope (column 0 only) and the limit is deliberate — but it must be the "
           "limit it claims, not an accident.";

    // The boundary case the name check exists for.
    EXPECT_FALSE(definesHelperAtColumnZero(
        "struct DirectoryAttackerHelper {\n};\n", "DirectoryAttacker"))
        << "`DirectoryAttackerHelper` matched `DirectoryAttacker` — a prefix match "
           "would make the guard fire on unrelated types that happen to share a "
           "leading substring";

    // And the FUNCTION shape still works, without the old return-type allowlist.
    EXPECT_TRUE(definesHelperAtColumnZero(
        "std::set<std::string> seenNames(int n) {\n    return {};\n}\n",
        "seenNames"))
        << "a function whose return type is not one of std::string/bool/void/int was "
           "missed — that allowlist was the hand-made-population defect one level "
           "down, and removing it is half of what #275 asked for";

    EXPECT_FALSE(definesHelperAtColumnZero(
        "bool applyProtectedDacl(const char* p);\n", "applyProtectedDacl"))
        << "a function DECLARATION was counted as a definition";
}

// ───────────────────────────────────────────────────────────────────────────
// GitHub #275 C5 — this detector and its PYTHON twin must not drift apart.
//
// The same question ("does this source DEFINE <name> at column 0?") is answered
// twice, in two languages:
//
//   * here, by definesHelperAtColumnZero;
//   * in tools/gate/check_duplicate_test_definitions.py, by definition_sizes()
//     — TYPE_DEF / FUNC_DEF / NOT_A_DEF plus _opens_a_body().
//
// Nothing compared them. #275 exists because a ~155-line helper was defined
// TWICE and this guard could not see it; the issue's deeper point is that two
// copies of a thing drift. That applies to the DETECTOR as much as to the
// helper, and a drift here is worse: both sides stay green, each against its
// own tests.
//
// So both read tests/fixtures/column_zero_definition_corpus.txt and must agree
// with it. The corpus is the single source of truth — neither implementation
// is — which is how "the two disagree with each other" gets detected as "at
// least one disagrees with the corpus", with no language calling the other.
// The Python half is tools/gate/test_duplicate_definition_mirror_agreement.py,
// and it asserts that THIS test and that corpus path are both named here, so a
// corpus only one side reads cannot pass for a cross-language control.
//
// WHAT IT FOUND. `multiline-function-declaration`. The Python side hit that
// shape on its first run against the real tree — tests/support/
// SaveFailureInjection.h:312-316 declares killChildInsideSaveWindow across five
// lines with the `;` four lines below the name — wrongly reported a DECLARATION
// as a second definition, and was fixed by balancing the parameter list. This
// mirror checked only the first line for `;` and never got that fix.
// ───────────────────────────────────────────────────────────────────────────
namespace {

struct CorpusCase {
    std::string id;
    std::string name;
    std::string why;
    std::string src;
    bool expect = false;
};

/// Parse the shared corpus. Strict on purpose: a silently dropped case would
/// shrink the corpus while leaving this test green, which is the same
/// population-is-part-of-the-spec failure #275 is about.
std::vector<CorpusCase> parseCorpus(const std::string& text,
                                    std::string& error) {
    std::vector<CorpusCase> cases;
    std::istringstream in(text);
    std::string line;
    CorpusCase cur;
    bool haveCase = false, inSrc = false;
    std::vector<std::string> srcLines;
    int lineno = 0;

    auto startsWith = [](const std::string& s, const char* p) {
        return s.rfind(p, 0) == 0;
    };

    while (std::getline(in, line)) {
        ++lineno;
        if (!line.empty() && line.back() == '\r') line.pop_back();

        if (inSrc) {
            if (line == "@end") {
                std::string joined;
                for (const auto& l : srcLines) joined += l + "\n";
                cur.src = joined;
                cases.push_back(cur);
                cur = CorpusCase{};
                srcLines.clear();
                haveCase = false;
                inSrc = false;
                continue;
            }
            if (startsWith(line, "@")) {
                error = "corpus line " + std::to_string(lineno) +
                        ": a source line may not begin with '@'";
                return {};
            }
            srcLines.push_back(line);
            continue;
        }

        if (line.empty() || line[0] == '#') continue;

        if (startsWith(line, "@case ")) {
            cur = CorpusCase{};
            cur.id = line.substr(6);
            haveCase = true;
        } else if (startsWith(line, "@name ")) {
            cur.name = line.substr(6);
        } else if (startsWith(line, "@expect ")) {
            const std::string v = line.substr(8);
            if (v != "0" && v != "1") {
                error = "corpus line " + std::to_string(lineno) +
                        ": @expect must be 0 or 1";
                return {};
            }
            cur.expect = (v == "1");
        } else if (startsWith(line, "@why ")) {
            cur.why = line.substr(5);
        } else if (line == "@src") {
            inSrc = true;
        } else {
            error = "corpus line " + std::to_string(lineno) +
                    ": unrecognised line: " + line;
            return {};
        }
        (void)haveCase;
    }
    if (inSrc) {
        error = "corpus ends inside an unterminated @case";
        return {};
    }
    return cases;
}

}  // namespace

TEST(OracleStandard, TheColumnZeroDetectorAgreesWithTheSharedCorpus)
{
    const fs::path corpusPath =
        repoRoot() / "tests" / "fixtures" / "column_zero_definition_corpus.txt";
    ASSERT_TRUE(fs::exists(corpusPath))
        << "the shared corpus is missing: " << corpusPath.string()
        << " -- without it this test cannot hold the C++ detector to the same "
           "answers as its Python twin (GitHub #275 C5).";

    std::string error;
    const std::vector<CorpusCase> cases =
        parseCorpus(readFile(corpusPath), error);
    ASSERT_TRUE(error.empty()) << "corpus did not parse: " << error;

    // Non-vacuity. A corpus that parsed to nothing, or that leans entirely one
    // way, is satisfied by a detector that always returns the same answer.
    ASSERT_GE(cases.size(), 12u)
        << "only " << cases.size() << " case(s) parsed; the corpus should hold "
           "at least #275's own seven plus the drift probes.";
    int positives = 0, negatives = 0;
    for (const auto& c : cases) (c.expect ? positives : negatives)++;
    EXPECT_GE(positives, 4) << "too few DEFINES cases to catch an always-false "
                               "detector";
    EXPECT_GE(negatives, 4) << "too few NOT-A-DEFINITION cases to catch an "
                               "always-true detector";

    std::vector<std::string> wrong;
    for (const auto& c : cases) {
        const bool got = definesHelperAtColumnZero(c.src, c.name);
        if (got != c.expect) {
            wrong.push_back(
                "  " + c.id + ": corpus says defines=" +
                (c.expect ? "true" : "false") +
                ", definesHelperAtColumnZero says " +
                (got ? "true" : "false") + "\n      why: " + c.why);
        }
    }

    EXPECT_TRUE(wrong.empty())
        << "definesHelperAtColumnZero disagrees with the shared corpus on "
        << wrong.size() << " case(s) -- which means it also disagrees with the "
           "Python detector wherever that one agrees with the corpus, and "
           "neither side's own tests would notice (GitHub #275 C5):\n"
        << [&] {
               std::string s;
               for (const auto& w : wrong) s += w + "\n";
               return s;
           }();
}

// ═══════════════════════════════════════════════════════════════════════════
// OS-1 — no test source file may be assertion-free.
// ═══════════════════════════════════════════════════════════════════════════
TEST(OracleStandard, NoTestSourceFileIsAssertionFree) {
    const std::regex assertRe(R"(\b(?:EXPECT|ASSERT)_)");
    std::vector<std::string> offenders;
    for (const auto& p : testSources()) {
        // test_main.cpp is the harness entry point and declares no test cases.
        if (p.filename() == "test_main.cpp") continue;
        const std::string src = stripNoise(readFile(p));
        if (!std::regex_search(src, assertRe))
            offenders.push_back(p.filename().string());
    }
    EXPECT_TRUE(offenders.empty())
        << "OS-1 (zero-assertion-file): these files contain no EXPECT_/ASSERT_ "
           "at all, so nothing in them can fail: "
        << [&] {
               std::string s;
               for (const auto& o : offenders) s += o + " ";
               return s;
           }();
}

// ═══════════════════════════════════════════════════════════════════════════
// OS-2 — self-negating tautologies. Enforced at ZERO, no allowlist.
// ═══════════════════════════════════════════════════════════════════════════
TEST(OracleStandard, NoSelfNegatingTautologies) {
    const std::regex re(
        R"(EXPECT_(?:TRUE|THAT)\(\s*([A-Za-z_0-9.\->()\[\]]+)\s*\|\|\s*!\s*([A-Za-z_0-9.\->()\[\]]+)\s*\))");
    std::vector<std::string> hits;
    for (const auto& p : testSources()) {
        const std::string src = stripNoise(readFile(p));
        for (auto it = std::sregex_iterator(src.begin(), src.end(), re);
             it != std::sregex_iterator(); ++it) {
            if ((*it)[1].str() != (*it)[2].str()) continue;  // not `x || !x`
            hits.push_back(p.filename().string() + ":" +
                           std::to_string(lineOf(src, static_cast<size_t>(it->position()))) +
                           " EXPECT_TRUE(" + (*it)[1].str() + " || !" +
                           (*it)[1].str() + ")");
        }
    }
    EXPECT_TRUE(hits.empty())
        << "OS-2 (self-negating-tautology): true for every value of the operand, "
           "so the assertion cannot fail: "
        << [&] {
               std::string s;
               for (const auto& h : hits) s += "\n  " + h;
               return s;
           }();
}

// ═══════════════════════════════════════════════════════════════════════════
// OS-3 — unsigned lower-bound tautologies. Enforced at ZERO, no allowlist.
// ═══════════════════════════════════════════════════════════════════════════
TEST(OracleStandard, NoUnsignedLowerBoundTautologies) {
    const std::regex re(
        R"((?:EXPECT|ASSERT)_GE\(\s*([^,;]*(?:\.size\(\)|\.length\(\)|\.count\(\))[^,;]*)\s*,\s*0u?\s*\))");
    std::vector<std::string> hits;
    for (const auto& p : testSources()) {
        const std::string src = stripNoise(readFile(p));
        for (auto it = std::sregex_iterator(src.begin(), src.end(), re);
             it != std::sregex_iterator(); ++it)
            hits.push_back(p.filename().string() + ":" +
                           std::to_string(lineOf(src, static_cast<size_t>(it->position()))) +
                           " " + (*it)[1].str() + " >= 0");
    }
    EXPECT_TRUE(hits.empty())
        << "OS-3 (over-tolerant-threshold): an unsigned quantity is always >= 0: "
        << [&] {
               std::string s;
               for (const auto& h : hits) s += "\n  " + h;
               return s;
           }();
}

// ═══════════════════════════════════════════════════════════════════════════
// OS-4 — the SUCCEED-only ratchet. This guard cross-checks the Python baseline
// against a C++ re-derivation, so neither can drift alone: if the suite gains a
// SUCCEED-only body, or loses one without the baseline being updated, the two
// counts stop matching and this test fails.
// ═══════════════════════════════════════════════════════════════════════════
TEST(OracleStandard, SucceedOnlyBaselineMatchesTheTree) {
    const fs::path lint = testsDir() / "scripts" / "lint_oracle_standard.py";
    ASSERT_TRUE(fs::exists(lint)) << "the SC-b enforcement point is missing: "
                                  << lint;
    const std::string py = readFile(lint);

    // Parse the OS4_BASELINE = { "A.B", ... } block out of the linter.
    const size_t open = py.find("OS4_BASELINE = {");
    ASSERT_NE(open, std::string::npos) << "OS4_BASELINE not found in " << lint;
    const size_t close = py.find("\n}", open);
    ASSERT_NE(close, std::string::npos);
    const std::string block = py.substr(open, close - open);

    std::set<std::string> baseline;
    const std::regex entryRe(R"(\"([A-Za-z_0-9]+\.[A-Za-z_0-9]+)\")");
    for (auto it = std::sregex_iterator(block.begin(), block.end(), entryRe);
         it != std::sregex_iterator(); ++it)
        baseline.insert((*it)[1].str());
    ASSERT_FALSE(baseline.empty()) << "failed to parse OS4_BASELINE";

    const std::set<std::string> actual = succeedOnlyBodies();

    std::vector<std::string> unlisted, stale;
    std::set_difference(actual.begin(), actual.end(), baseline.begin(),
                        baseline.end(), std::back_inserter(unlisted));
    std::set_difference(baseline.begin(), baseline.end(), actual.begin(),
                        actual.end(), std::back_inserter(stale));

    EXPECT_TRUE(unlisted.empty())
        << "OS-4 (succeed-only-body): NEW test bodies assert nothing but "
           "SUCCEED(). Give them a real oracle — do not extend the baseline: "
        << [&] {
               std::string s;
               for (const auto& u : unlisted) s += "\n  " + u;
               return s;
           }();
    EXPECT_TRUE(stale.empty())
        << "OS-4 ratchet: these names are pinned in OS4_BASELINE but no longer "
           "violate OS-4. Delete them from the baseline so it keeps shrinking: "
        << [&] {
               std::string s;
               for (const auto& t : stale) s += "\n  " + t;
               return s;
           }();
}

// ═══════════════════════════════════════════════════════════════════════════
// OS-5 — the standard document exists, registers every class (including the
// five A3-56 novel classes and the A3-55 class the capstone dropped), and its
// quoted baseline size matches the enforcement point.
// ═══════════════════════════════════════════════════════════════════════════
TEST(OracleStandard, StandardDocumentRegistersEverySmellClass) {
    const fs::path doc = repoRoot() / "docs" / "audit" / "oracle-standard.md";
    ASSERT_TRUE(fs::exists(doc))
        << "SC-b's standard document does not exist: " << doc;
    const std::string text = readFile(doc);

    const std::vector<std::string> required = {
        "zero-assertion-file", "self-negating-tautology",
        "over-tolerant-threshold", "succeed-only-body",
        // The five NOVEL smell classes phase-1 found and the phase-5 capstone
        // dropped without a register entry (gap A3-56).
        "misnamed-test", "source-grep-proxy", "partial-contract-assert",
        "no-removal-check", "weak-positive-detection"};
    for (const auto& cls : required)
        EXPECT_NE(text.find(cls), std::string::npos)
            << "smell class '" << cls
            << "' is not registered in the oracle standard";
}

TEST(OracleStandard, StandardDocumentQuotesTheRealBaselineSize) {
    const fs::path doc = repoRoot() / "docs" / "audit" / "oracle-standard.md";
    ASSERT_TRUE(fs::exists(doc));
    const std::string text = readFile(doc);

    const size_t n = succeedOnlyBodies().size();
    const std::string quoted = std::to_string(n) + " pre-existing SUCCEED-only";
    EXPECT_NE(text.find(quoted), std::string::npos)
        << "the standard must quote the CURRENT number of pinned SUCCEED-only "
           "bodies (" << n << "), so the document cannot drift away from the "
           "tree it describes; looked for the phrase \"" << quoted << "\"";
}

// The enforcement point must actually be wired, or it is a script nobody runs.
TEST(OracleStandard, LintIsRegisteredAsACTestStep) {
    const std::string cmake = readFile(testsDir() / "CMakeLists.txt");
    EXPECT_NE(cmake.find("lint_oracle_standard"), std::string::npos)
        << "tests/CMakeLists.txt does not register the SC-b lint as a CTest "
           "step, so nothing runs it";
}
