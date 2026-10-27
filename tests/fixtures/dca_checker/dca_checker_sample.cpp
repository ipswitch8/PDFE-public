// dca_checker_sample.cpp -- FIXTURE, NOT A COMPILED TEST SOURCE.
//
// Meta-audit follow-up phase 41.  This file is the C++ side of the
// discriminating fixture pair that proves tools/gate/check_dca.py resolves a
// cited `Suite.Case` INSIDE the file, instead of merely checking the file
// exists (the R-26 blind spot).  It is deliberately NOT listed in
// tests/CMakeLists.txt: the checker parses it as text, and compiling it would
// inject fake suites into the real pdfe_tests inventory.
//
// It carries one instance of every GTest definition form that occurs in this
// repo, plus decoys that must NOT resolve.

#include <gtest/gtest.h>

// ---- Real definitions: these MUST resolve. ----------------------------------

TEST(DcaCheckerSample, PlainCase) { SUCCEED(); }

class DcaCheckerSampleFixture : public ::testing::Test {};

TEST_F(DcaCheckerSampleFixture, FixtureCase) { SUCCEED(); }

// Whitespace and newlines inside the macro call are legal C++ and must resolve.
TEST_F(
    DcaCheckerSampleFixture,
    MultiLineCase) {
  SUCCEED();
}

class DcaCheckerSampleParam : public ::testing::TestWithParam<int> {};

TEST_P(DcaCheckerSampleParam, ParameterisedCase) { SUCCEED(); }

INSTANTIATE_TEST_SUITE_P(SamplePrefix, DcaCheckerSampleParam,
                         ::testing::Values(0, 1));

// ---- Decoys: these MUST NOT resolve. ----------------------------------------

// TEST(DcaCheckerSample, CommentedOutCase) { SUCCEED(); }

/*
TEST_F(DcaCheckerSampleFixture, BlockCommentedCase) { SUCCEED(); }
*/

const char* kQuotedMacro = "TEST(DcaCheckerSample, StringLiteralCase)";

// INSTANTIATE_TEST_SUITE_P names a SUITE, never a case, so
// `DcaCheckerSampleParam.SamplePrefix` must not resolve either.
