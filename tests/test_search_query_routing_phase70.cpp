// -*- coding: utf-8 -*-
//
// Phase-17 (follow-up phase-70), criterion 6 — the ReDoS that cannot happen.
//
// THE CRITERION'S PREMISE IS WRONG, and this file is the measurement that says
// so rather than a test of an impossible event. The criterion asks that
// "RegexQuery::from_pattern receives a bounded/time-limited match ... preventing
// a ReDoS from a raw user query string; a test with a catastrophic-backtracking
// pattern asserts bounded execution time."
//
// A raw user query string never reaches RegexQuery as a regex. Three
// independent reasons, any one of which is sufficient:
//
//   1. THE INPUT IS NOT RAW. lib.rs builds the pattern as
//      `format!("{}.*", regex_escape(&prefix.to_lowercase()))`. regex_escape
//      escapes . + * ? ( ) [ ] { } | ^ $ and backslash, so the user's text is
//      matched LITERALLY. The only regex construct in the final pattern is the
//      trailing `.*` the code appends itself.
//
//   2. THAT PATH IS GATED. is_simple_prefix requires the query to end in `*`
//      and to contain no whitespace and none of " + - : ( ) * — which excludes
//      every classic catastrophic construction, `(a+)+` and friends included.
//      Anything else is routed to QueryParser, which does not build a regex.
//
//   3. NO BACKTRACKING ENGINE IS ON THIS PATH. RegexQuery compiles the pattern
//      to a DFA and intersects it with the term dictionary's FST, so the work is
//      bounded by the dictionary rather than by the input, and catastrophic
//      backtracking is structurally impossible rather than merely unlikely.
//
//      Stating the dependency edge precisely, because an earlier draft of this
//      comment did not: Cargo.lock shows `tantivy 0.26.1 -> [regex 1.12.3,
//      tantivy-fst 0.5.0]`, `tantivy-fst -> [regex-syntax]`, and `regex 1.12.3
//      -> [regex-automata 0.4.14, regex-syntax]`. There is NO direct
//      `tantivy -> regex-automata` edge, which the earlier wording implied. It
//      does not change the conclusion — both candidate engines are
//      non-backtracking by construction: tantivy-fst builds its own automaton
//      from a regex-syntax AST, and the Rust `regex` crate guarantees
//      linear-time matching with no backtracking mode to fall into.
//
// Writing "a test with a catastrophic-backtracking pattern asserts bounded
// execution time" would therefore be a test that passes because the hazard does
// not exist, in a suite where a passing test is read as evidence a hazard was
// closed. Reasons 1 and 2 ARE testable and are tested below; reason 3 is a
// property of a dependency and is cited, not asserted.
//
// AND THE PROPOSED MITIGATION POINTS THE WRONG WAY. An input-length cap was
// floated as worth adding anyway. It is not, and the last case here shows why:
// on this path cost rises as queries get SHORTER, because a one-character
// prefix matches more of the term dictionary. A length cap constrains the cheap
// end and leaves the expensive end untouched. It is recorded here as measured
// and rejected, not silently dropped.

#include <gtest/gtest.h>

#include "search/TantivyBridge.h"
#include "search/TantivyIndex.h"

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

using namespace pdfe::search;

namespace {

std::filesystem::path freshDir(const char* name)
{
    auto dir = std::filesystem::temp_directory_path() / "pdfe_query_routing" / name;
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir);
    return dir;
}

std::string prefixPatternFor(const char* query)
{
    const size_t needed = pdfe_tantivy_prefix_pattern(query, nullptr, 0);
    if (needed == 0) return {};
    std::vector<char> buf(needed);
    pdfe_tantivy_prefix_pattern(query, buf.data(), buf.size());
    return std::string(buf.data());
}

// The classic catastrophic-backtracking shapes. Each would be pathological on a
// backtracking engine given a non-matching tail.
const char* kRedosShapes[] = {
    "(a+)+$",
    "(a|a)+$",
    "(a*)*b",
    "(x+x+)+y",
    "a{1,32000}{1,32000}",
    "((((((((((a))))))))))+$",
};

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Reason 2 — routing
// ─────────────────────────────────────────────────────────────────────────────

TEST(SearchQueryRouting, CatastrophicBacktrackingShapesNeverReachTheRegexPath)
{
    for (const char* q : kRedosShapes) {
        EXPECT_EQ(pdfe_tantivy_query_uses_regex_path(q), 0)
            << "query \"" << q << "\" was routed to RegexQuery. is_simple_prefix "
            << "has been widened and a user-supplied regex construct can now "
            << "reach the regex engine.";
        EXPECT_TRUE(prefixPatternFor(q).empty())
            << "a regex pattern was built for \"" << q << "\"";
    }
}

// THE CONTROL. Without it, a routing function that returned 0 for everything —
// or a fast path that had been deleted outright — would satisfy the case above.
TEST(SearchQueryRouting, AnOrdinaryPrefixQueryDoesTakeTheRegexPath)
{
    EXPECT_EQ(pdfe_tantivy_query_uses_regex_path("run*"), 1)
        << "the prefix fast path is not being taken at all, which makes the "
           "refusal cases above vacuous";
    EXPECT_EQ(prefixPatternFor("run*"), "run.*");
}

TEST(SearchQueryRouting, TheGateRejectsEachOperatorIndividually)
{
    // Asserted one operator at a time. The composite shapes above contain
    // several apiece, so any single character still being accepted would hide
    // behind the others.
    const char* rejected[] = {"a b*", "a\"b*", "a+b*", "a-b*",
                              "a:b*", "a(b*",  "a)b*", "a*b*"};
    for (const char* q : rejected) {
        EXPECT_EQ(pdfe_tantivy_query_uses_regex_path(q), 0)
            << "\"" << q << "\" reached the regex path";
    }

    // And the two structural requirements.
    EXPECT_EQ(pdfe_tantivy_query_uses_regex_path("noasterisk"), 0);
    EXPECT_EQ(pdfe_tantivy_query_uses_regex_path("*"), 0)
        << "a bare asterisk must not build the pattern \".*\", which matches "
           "every term in the dictionary";
}

// ─────────────────────────────────────────────────────────────────────────────
// Reason 1 — escaping
// ─────────────────────────────────────────────────────────────────────────────

TEST(SearchQueryRouting, MetacharactersThatSurviveTheGateAreEscapedLiterally)
{
    // is_simple_prefix does not reject these, so they DO reach the pattern
    // builder — which is precisely why the escaping has to be asserted rather
    // than assumed covered by the gate. `.` `[` `]` `{` `}` `|` `^` `$` `?` and
    // backslash all pass the gate.
    EXPECT_EQ(prefixPatternFor("a.b*"), "a\\.b.*");
    EXPECT_EQ(prefixPatternFor("a[b*"), "a\\[b.*");
    EXPECT_EQ(prefixPatternFor("a{2}*"), "a\\{2\\}.*");
    EXPECT_EQ(prefixPatternFor("a|b*"), "a\\|b.*");
    EXPECT_EQ(prefixPatternFor("^a$*"), "\\^a\\$.*");
    EXPECT_EQ(prefixPatternFor("a?*"), "a\\?.*");
    EXPECT_EQ(prefixPatternFor("a\\b*"), "a\\\\b.*");

    // The whole point, stated as one assertion: the built pattern's ONLY
    // unescaped regex construct is the trailing `.*`.
    const std::string p = prefixPatternFor("a.b[c]{2}|d*");
    ASSERT_FALSE(p.empty());
    EXPECT_EQ(p.substr(p.size() - 2), ".*");
    const std::string body = p.substr(0, p.size() - 2);
    for (size_t i = 0; i < body.size(); ++i) {
        const char c = body[i];
        const bool meta = (c == '.' || c == '+' || c == '*' || c == '?' ||
                           c == '(' || c == ')' || c == '[' || c == ']' ||
                           c == '{' || c == '}' || c == '|' || c == '^' ||
                           c == '$' || c == '\\');
        if (!meta) continue;
        if (c == '\\') { ++i; continue; }   // escape plus the char it escapes
        ADD_FAILURE() << "unescaped metacharacter '" << c << "' at " << i
                      << " in pattern \"" << p << "\" — a user's regex "
                         "construct is reaching the engine as a construct";
    }
}

TEST(SearchQueryRouting, AnUnescapedDotWouldMatchAndTheEscapedOneDoesNot)
{
    // The escaping has to be observable in BEHAVIOUR, not just in the pattern
    // string, or the assertions above are only checking that one function
    // agrees with another.
    //
    // The oracle runs in this direction because of how tantivy tokenizes: the
    // default tokenizer splits on everything non-alphanumeric, so no term in
    // the dictionary ever CONTAINS a '.'. (My first attempt at this case
    // indexed "a.bravo" and expected to find it — it does not exist as a term,
    // and the case failed for a reason that had nothing to do with escaping.)
    //
    // So: the user types "a.b*". Escaped, the pattern is `a\.b.*`, which can
    // match no term at all — zero hits. UNescaped it would be `a.b.*`, where
    // `.` means "any character", and that matches the perfectly ordinary term
    // "axbravo". Zero hits is therefore the signature of working escaping, and
    // a hit is the signature of a user's metacharacter reaching the engine as a
    // metacharacter.
    auto dir = freshDir("escape_behaviour");
    TantivyIndex idx(dir);
    ASSERT_TRUE(idx.upsertPage(0, "axbravo charlie delta"));
    ASSERT_TRUE(idx.commit());

    // The control FIRST: prove the index and the prefix path are working, so
    // the zero-hit assertion below cannot pass because search is simply broken.
    auto control = idx.search("axb*", 10);
    ASSERT_EQ(control.size(), 1u)
        << "the prefix path returns nothing even for a literal match — the "
           "assertion below would pass vacuously";
    EXPECT_EQ(control[0].pageIndex, 0);

    auto hits = idx.search("a.b*", 10);
    EXPECT_TRUE(hits.empty())
        << "\"a.b*\" matched the term \"axbravo\", so the '.' was passed to the "
           "regex engine as \"any character\" rather than escaped — a user's "
           "query is being interpreted as a regex";
}

// ─────────────────────────────────────────────────────────────────────────────
// The measurement that rejects an input-length cap
// ─────────────────────────────────────────────────────────────────────────────

TEST(SearchQueryRouting, CostFallsAsTheQueryGrowsSoALengthCapWouldBeBackwards)
{
    // Build a dictionary with a lot of distinct terms sharing a first letter,
    // so a one-character prefix matches broadly and a longer one narrowly.
    auto dir = freshDir("length_cost");
    TantivyIndex idx(dir);
    for (int page = 0; page < 40; ++page) {
        std::string text;
        for (int t = 0; t < 200; ++t) {
            text += "z" + std::to_string(page * 200 + t) + " ";
        }
        ASSERT_TRUE(idx.upsertPage(page, text));
    }
    ASSERT_TRUE(idx.commit());

    auto timeQuery = [&](const std::string& q) {
        // Best-of-3: this box is a shared terminal server and a single sample
        // picks up whatever else was scheduled. Only the floor is meaningful.
        auto best = std::chrono::steady_clock::duration::max();
        for (int i = 0; i < 3; ++i) {
            const auto t0 = std::chrono::steady_clock::now();
            auto hits = idx.search(q, 10);
            const auto dt = std::chrono::steady_clock::now() - t0;
            if (dt < best) best = dt;
            (void)hits;
        }
        return std::chrono::duration_cast<std::chrono::microseconds>(best).count();
    };

    const auto shortQuery = timeQuery("z*");          // matches ~8000 terms
    const auto longQuery  = timeQuery("z7999*");      // matches ~1

    // Recorded so the disposition quotes a measurement rather than an argument.
    std::cout << "[phase-70] prefix-query cost: \"z*\" (broad) " << shortQuery
              << "us vs \"z7999*\" (narrow) " << longQuery << "us" << std::endl;

    EXPECT_GE(shortQuery, longQuery)
        << "the SHORT query was cheaper than the long one, which would be the "
           "only condition under which an input-length cap bounded anything on "
           "this path. If this ever fires, re-open the length-cap question.";

    // A generous ceiling, not a benchmark: this is a "did it hang" oracle. Both
    // queries are DFA-over-FST work bounded by the term dictionary, so both
    // should be in the low milliseconds; 10s means something structural changed.
    EXPECT_LT(shortQuery, 10'000'000)
        << "the broad prefix query took over ten seconds; the regex path is no "
           "longer bounded by the term dictionary";
}
