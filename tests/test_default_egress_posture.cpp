// -*- coding: utf-8 -*-
//
// Meta-audit 2026-07 follow-up, phase F13 (SEC-46 / SEC-48): make the
// "PDFE makes no outbound connections by default" claim FALSIFIABLE.
//
// Phase-4 of the meta audit verified by inspection that the default posture is
// no network egress — signature validation fetching is opt-in, form submission
// is not reachable from any UI dispatcher, and nothing on the open/render/save
// path opens a socket. It also recorded that NO TEST ASSERTED IT and that no
// socket observer existed anywhere in the tree. A posture nobody measures
// silently decays: the first feature to add an OCSP fetch, a font lookup or a
// telemetry ping on the open path would break it with every test still green.
//
// This file supplies the missing assertion, using the observation seam at
// tests/security_net_oracles.h (NetworkEgressGuard).
//
// THE CREDIBILITY PROBLEM, AND HOW IT IS ANSWERED HERE
// -----------------------------------------------------
// "Zero connections observed" is worthless if the observer cannot observe.
// Three tests together establish that it can:
//
//   1. ObserverDetectsALoopbackConnection — the POSITIVE CONTROL. The test
//      itself opens a listening socket on 127.0.0.1, connects to it from this
//      same process, and asserts the guard SAW that connection. If the guard
//      were blind (wrong pid filter, table read failing, sampler never
//      running) this test fails, and the zero-egress result below becomes
//      meaningless-by-construction rather than falsely reassuring.
//   2. ExternalEndpointClassification — the loopback-vs-routable classifier is
//      a pure function and is asserted directly against known addresses, so
//      "saw it" (1) plus "would have called it external" (2) is a complete
//      argument that a real outbound connection is caught, WITHOUT this test
//      ever touching a real network.
//   3. OpenRenderSaveMakesNoOutboundConnection — the actual posture assertion.
//
// The guard's blind spots (DNS via the Windows DNS Client service, wildcard-
// bound UDP sendto, egress by a child process) are documented in
// tests/security_net_oracles.h and in the phase-13 dispositions rather than
// glossed over. Child-process egress is covered by ProcessSpawnGuard.
#include <gtest/gtest.h>

#include "security_net_oracles.h"
#include "security_oracles.h"

#include "app/AppSettings.h"
#include "core/Document.h"
#include "core/MuPdfContext.h"
#include "core/Page.h"

#include <QImage>
#include <QRectF>
#include <QTemporaryDir>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#endif

namespace {

std::filesystem::path fixturesDir()
{
    return std::filesystem::path(TEST_FIXTURES_DIR);
}

// The open → render → save corpus. Deliberately spans the document classes
// most likely to *want* the network: a signed document (revocation checking),
// a form (submit actions), an image-bearing document (external resources).
const char* kCorpus[] = {
    "test_3page.pdf",
    "test_form.pdf",
    "test_pdf_with_images.pdf",
    "test_signed.pdf",
};

} // namespace

// ---------------------------------------------------------------------------
// Fresh profile: the security preferences at their factory defaults. Saved and
// restored so this file cannot perturb any other test in the process.
// ---------------------------------------------------------------------------
class DefaultEgressPostureTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        settings_ = std::make_unique<pdfe::app::AppSettings>();
        priorJs_       = settings_->javaScriptEnabled();
        priorExternal_ = settings_->externalActionsAllowed();
        priorJpx_      = settings_->jpxRenderingEnabled();

        // Factory defaults == the shipped fresh-profile posture.
        settings_->setJavaScriptEnabled(false);
        settings_->setExternalActionsAllowed(false);
        settings_->setJpxRenderingEnabled(false);

        tempDir_ = std::make_unique<QTemporaryDir>();
        ASSERT_TRUE(tempDir_->isValid());
    }

    void TearDown() override
    {
        if (settings_) {
            settings_->setJavaScriptEnabled(priorJs_);
            settings_->setExternalActionsAllowed(priorExternal_);
            settings_->setJpxRenderingEnabled(priorJpx_);
        }
    }

    std::filesystem::path tempPath(const std::string& name) const
    {
        return std::filesystem::path(tempDir_->path().toStdString()) / name;
    }

    std::unique_ptr<pdfe::app::AppSettings> settings_;
    std::unique_ptr<QTemporaryDir> tempDir_;
    bool priorJs_ = false;
    bool priorExternal_ = false;
    bool priorJpx_ = false;
};

// ---------------------------------------------------------------------------
// (2) Classification — pure function, no OS state involved.
// ---------------------------------------------------------------------------
TEST(NetworkEgressGuardOracle, ExternalEndpointClassification)
{
    using pdfe_test::isExternalEndpoint;

    // Routable peers: these MUST be classified as egress, otherwise the
    // zero-egress assertion could never fail.
    EXPECT_TRUE(isExternalEndpoint("8.8.8.8"));
    EXPECT_TRUE(isExternalEndpoint("1.1.1.1"));
    EXPECT_TRUE(isExternalEndpoint("93.184.216.34"));
    EXPECT_TRUE(isExternalEndpoint("10.0.0.5"));          // RFC1918 still leaves the host
    EXPECT_TRUE(isExternalEndpoint("[2001:4860:4860:0000:0000:0000:0000:8888]"));

    // Not egress.
    EXPECT_FALSE(isExternalEndpoint("127.0.0.1"));
    EXPECT_FALSE(isExternalEndpoint("127.1.2.3"));
    EXPECT_FALSE(isExternalEndpoint("::1"));
    EXPECT_FALSE(isExternalEndpoint("[::1]"));
    EXPECT_FALSE(isExternalEndpoint("0.0.0.0"));
    EXPECT_FALSE(isExternalEndpoint("[::]"));
    EXPECT_FALSE(isExternalEndpoint(""));
}

// ---------------------------------------------------------------------------
// (1) POSITIVE CONTROL — prove the observer is not blind.
// ---------------------------------------------------------------------------
#if defined(_WIN32)
TEST(NetworkEgressGuardOracle, ObserverDetectsALoopbackConnection)
{
    WSADATA wsa{};
    ASSERT_EQ(::WSAStartup(MAKEWORD(2, 2), &wsa), 0)
        << "WSAStartup failed; the positive control cannot run";

    SOCKET listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ASSERT_NE(listener, INVALID_SOCKET);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;  // ephemeral
    ASSERT_EQ(::bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
    int addrLen = static_cast<int>(sizeof(addr));
    ASSERT_EQ(::getsockname(listener, reinterpret_cast<sockaddr*>(&addr), &addrLen), 0);
    ASSERT_EQ(::listen(listener, 1), 0);

    // Arm the guard AFTER the listener exists, so only the client connection
    // is new. (LISTEN-state rows are excluded by the guard anyway.)
    pdfe_test::NetworkEgressGuard guard;
    ASSERT_TRUE(guard.supported())
        << "the TCP/UDP tables could not be read; the observer is blind and "
           "no zero-egress result from this file may be trusted";

    SOCKET client = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    ASSERT_NE(client, INVALID_SOCKET);
    ASSERT_EQ(::connect(client, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
    SOCKET accepted = ::accept(listener, nullptr, nullptr);
    EXPECT_NE(accepted, INVALID_SOCKET);

    // Give the sampler a couple of intervals; the final snapshot in stop()
    // would catch it regardless (TIME_WAIT), but this keeps the test quick.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    guard.stop();

    EXPECT_GT(guard.newConnectionCount(), 0u)
        << "the guard failed to observe a connection this process definitely "
           "made — it cannot be relied on to observe a real one.\n"
        << guard.report();
    // A loopback connection is observed but is NOT egress.
    EXPECT_EQ(guard.externalConnectionCount(), 0u) << guard.report();

    if (accepted != INVALID_SOCKET) ::closesocket(accepted);
    ::closesocket(client);
    ::closesocket(listener);
    ::WSACleanup();
}
#endif // _WIN32

// ---------------------------------------------------------------------------
// (3) THE POSTURE ASSERTION.
// ---------------------------------------------------------------------------
TEST_F(DefaultEgressPostureTest, OpenRenderSaveMakesNoOutboundConnection)
{
    std::vector<std::string> present;
    for (const char* name : kCorpus) {
        if (std::filesystem::exists(fixturesDir() / name)) present.push_back(name);
    }
    // The fixture floor in tests/CMakeLists.txt pins all four at configure
    // time, so an empty corpus is a real defect, not a reason to skip.
    ASSERT_FALSE(present.empty())
        << "no corpus fixture present; the posture assertion would be vacuous";

    pdfe_test::NetworkEgressGuard guard;
    ASSERT_TRUE(guard.supported())
        << "the TCP/UDP tables could not be read — this test observed NOTHING "
           "and must not be read as proving zero egress";
    pdfe_test::ProcessSpawnGuard spawnGuard;

    for (const std::string& name : present) {
        const std::filesystem::path src = fixturesDir() / name;
        const std::filesystem::path work = tempPath("egress_" + name);
        std::filesystem::copy_file(src, work,
                                   std::filesystem::copy_options::overwrite_existing);

        auto ctx = std::make_shared<pdfe::core::MuPdfContext>();
        auto doc = pdfe::core::Document::open(ctx, work);
        ASSERT_NE(doc, nullptr) << name;

        const int pages = doc->pageCount();
        ASSERT_GT(pages, 0) << name;
        for (int i = 0; i < pages && i < 3; ++i) {
            auto page = doc->page(i);
            ASSERT_NE(page, nullptr) << name << " page " << i;
            QImage img = page->render(QRectF(), 0.5f);
            EXPECT_FALSE(img.isNull()) << name << " page " << i;
        }

        // Verify the save actually produced a file: a saveAs that quietly did
        // nothing would make "zero outbound connections" vacuously true for
        // the save leg of open -> render -> save (GitHub #163).
        const std::filesystem::path out = tempPath("egress_out_" + name);
        doc->saveAs(out);
        ASSERT_TRUE(std::filesystem::exists(out))
            << name << ": saveAs wrote no file, so the save leg of this "
                       "posture check exercised nothing";
        EXPECT_GT(std::filesystem::file_size(out), 0u) << name;
    }

    guard.stop();

    EXPECT_EQ(guard.externalConnectionCount(), 0u)
        << "PDFE opened an OUTBOUND connection during open -> render -> save "
           "on a fresh profile. The documented default posture is zero "
           "egress; either the posture changed or a dependency now phones "
           "home.\n"
        << guard.report();

    EXPECT_EQ(spawnGuard.newlySpawned(), std::string())
        << "open -> render -> save spawned an attack-class child process; "
           "egress by a child is outside NetworkEgressGuard's scope and is "
           "ProcessSpawnGuard's job";
}
