#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>

#include "core/QpdfWrapper.h"

using namespace pdfe::core;
namespace fs = std::filesystem;

class QpdfWrapperTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        // Test fixtures directory
        fixturesDir_ = fs::path(TEST_FIXTURES_DIR);
    }

    void TearDown() override {}

    fs::path fixturesDir_;
};

TEST_F(QpdfWrapperTest, DefaultConstruction)
{
    QpdfWrapper wrapper;
    EXPECT_FALSE(wrapper.isOpen());
}

TEST_F(QpdfWrapperTest, OpenValidPdf)
{
    auto testPdf = fixturesDir_ / "test_3page.pdf";
    if (!fs::exists(testPdf)) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    QpdfWrapper wrapper;
    EXPECT_NO_THROW(wrapper.open(testPdf));
    EXPECT_TRUE(wrapper.isOpen());
}

TEST_F(QpdfWrapperTest, OpenInvalidPathThrows)
{
    QpdfWrapper wrapper;
    EXPECT_THROW(
        wrapper.open("nonexistent_file_that_does_not_exist.pdf"),
        QpdfException
    );
    EXPECT_FALSE(wrapper.isOpen());
}

TEST_F(QpdfWrapperTest, PageCountAfterOpen)
{
    auto testPdf = fixturesDir_ / "test_3page.pdf";
    if (!fs::exists(testPdf)) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    QpdfWrapper wrapper;
    wrapper.open(testPdf);
    EXPECT_EQ(wrapper.pageCount(), 3);
}

TEST_F(QpdfWrapperTest, PageCountWithoutOpenThrows)
{
    QpdfWrapper wrapper;
    EXPECT_THROW(wrapper.pageCount(), QpdfException);
}

TEST_F(QpdfWrapperTest, SaveWithoutOpenThrows)
{
    QpdfWrapper wrapper;
    EXPECT_THROW(wrapper.save("output.pdf"), QpdfException);
}

TEST_F(QpdfWrapperTest, SaveIncrementalWithoutOpenThrows)
{
    QpdfWrapper wrapper;
    EXPECT_THROW(wrapper.saveIncremental("output.pdf"), QpdfException);
}

TEST_F(QpdfWrapperTest, SaveToFile)
{
    auto testPdf = fixturesDir_ / "test_3page.pdf";
    if (!fs::exists(testPdf)) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    auto outputPdf = fixturesDir_ / "test_output.pdf";

    // Remove any leftover output from a previous run
    fs::remove(outputPdf);

    // Use an inner scope so that all QpdfWrapper instances (and the file
    // handles they hold) are destroyed via RAII before we attempt cleanup.
    {
        QpdfWrapper wrapper;
        wrapper.open(testPdf);
        EXPECT_NO_THROW(wrapper.save(outputPdf));
        EXPECT_TRUE(fs::exists(outputPdf));

        // Verify the saved file is a valid PDF
        QpdfWrapper verifier;
        EXPECT_NO_THROW(verifier.open(outputPdf));
        EXPECT_EQ(verifier.pageCount(), 3);
    }

    // Cleanup -- handles are now released
    fs::remove(outputPdf);
}

TEST_F(QpdfWrapperTest, MoveConstruction)
{
    auto testPdf = fixturesDir_ / "test_3page.pdf";
    if (!fs::exists(testPdf)) {
        GTEST_SKIP() << "Test fixture not available: " << testPdf;
    }

    QpdfWrapper wrapper1;
    wrapper1.open(testPdf);
    EXPECT_TRUE(wrapper1.isOpen());

    QpdfWrapper wrapper2(std::move(wrapper1));
    EXPECT_TRUE(wrapper2.isOpen());
}
