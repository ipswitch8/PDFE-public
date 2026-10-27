// -*- coding: utf-8 -*-
// Phase 8b (submit): FormSubmitter decision logic + live POST to a
// local QTcpServer-backed mock HTTP server. HTTPS cert validation
// behavior is inherited from Qt — outside PDFE's test scope; we
// verify that our wiring passes the right body/headers and returns
// a coherent SubmitResult.
#include <gtest/gtest.h>
#include "formsubmit/FormSubmitter.h"
#include "formsubmit/HostAllowlist.h"

#include <QByteArray>
#include <QEventLoop>
#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>

#include <atomic>
#include <memory>

using namespace pdfe::formsubmit;

namespace {

/// Minimal HTTP/1.1 server for a single POST-and-reply exchange.
/// Binds to 127.0.0.1:<random>, captures the first request, replies
/// 200 OK with "mock-ok".
class MockHttpServer : public QObject {
    Q_OBJECT
public:
    MockHttpServer() {
        server_.listen(QHostAddress::LocalHost, 0);
        connect(&server_, &QTcpServer::newConnection,
                this, &MockHttpServer::onNewConnection);
    }
    quint16 port() const { return server_.serverPort(); }
    QByteArray lastRequest() const { return lastRequest_; }
    QByteArray lastBody() const { return lastBody_; }

private slots:
    void onNewConnection() {
        while (auto* sock = server_.nextPendingConnection()) {
            connect(sock, &QTcpSocket::readyRead, this, [this, sock]() {
                lastRequest_.append(sock->readAll());
                const int hdrEnd = lastRequest_.indexOf("\r\n\r\n");
                if (hdrEnd < 0) return;
                const int bodyStart = hdrEnd + 4;
                // Content-Length header determines body size.
                const QByteArray headers = lastRequest_.left(hdrEnd);
                int expectedLen = 0;
                for (const auto& line : headers.split('\n')) {
                    if (line.toLower().startsWith("content-length:")) {
                        expectedLen = line.mid(15).trimmed().toInt();
                    }
                }
                if (lastRequest_.size() - bodyStart < expectedLen) return;

                lastBody_ = lastRequest_.mid(bodyStart, expectedLen);
                const QByteArray resp =
                    "HTTP/1.1 200 OK\r\n"
                    "Content-Length: 7\r\n"
                    "Content-Type: text/plain\r\n"
                    "Connection: close\r\n\r\n"
                    "mock-ok";
                sock->write(resp);
                sock->flush();
                sock->disconnectFromHost();
            });
            connect(sock, &QTcpSocket::disconnected,
                    sock, &QTcpSocket::deleteLater);
        }
    }

private:
    QTcpServer server_;
    QByteArray lastRequest_;
    QByteArray lastBody_;
};

} // namespace

// -------- Decision logic --------

TEST(FormSubmitterDecision, RejectsEmptyUrl)
{
    HostAllowlist list;
    EXPECT_EQ(FormSubmitter::shouldSubmit(QUrl(), list),
              SubmitDecision::RejectInvalidUrl);
}

TEST(FormSubmitterDecision, RejectsUnsupportedSchemes)
{
    HostAllowlist list({"example.com"});
    EXPECT_EQ(FormSubmitter::shouldSubmit(QUrl("file:///etc/hosts"), list),
              SubmitDecision::RejectInvalidUrl);
    EXPECT_EQ(FormSubmitter::shouldSubmit(QUrl("ftp://example.com/"), list),
              SubmitDecision::RejectInvalidUrl);
    EXPECT_EQ(FormSubmitter::shouldSubmit(QUrl("javascript:alert(1)"), list),
              SubmitDecision::RejectInvalidUrl);
}

TEST(FormSubmitterDecision, AcceptsHttpAndHttpsForAllowlistedHost)
{
    HostAllowlist list({"forms.example.com"});
    EXPECT_EQ(FormSubmitter::shouldSubmit(
                  QUrl("https://forms.example.com/submit"), list),
              SubmitDecision::Allow);
    EXPECT_EQ(FormSubmitter::shouldSubmit(
                  QUrl("http://forms.example.com/submit"), list),
              SubmitDecision::Allow);
}

TEST(FormSubmitterDecision, UnknownHostAsksForConfirmation)
{
    HostAllowlist list({"forms.example.com"});
    EXPECT_EQ(FormSubmitter::shouldSubmit(
                  QUrl("https://other.example.com/submit"), list),
              SubmitDecision::ConfirmNewHost);
}

TEST(FormSubmitterDecision, HostIsCaseInsensitive)
{
    HostAllowlist list({"example.com"});
    EXPECT_EQ(FormSubmitter::shouldSubmit(
                  QUrl("https://Example.COM/path"), list),
              SubmitDecision::Allow);
}

// -------- Live POST to mock server --------

TEST(FormSubmitterLive, PostsBodyAndContentTypeToMockServer)
{
    MockHttpServer mock;
    ASSERT_GT(mock.port(), 0);

    FormSubmitter sub;
    // Phase-19: loopback is now refused by default (SSRF). This test is the
    // legitimate exception the opt-in exists for — the only honest way to
    // exercise the real network path is a server we actually run. Written as
    // an explicit line here rather than a build flag so that "this test
    // deliberately reaches a normally-blocked address" is visible in review.
    sub.setAllowPrivateDestinations(true);
    QUrl url(QStringLiteral("http://127.0.0.1:%1/submit").arg(mock.port()));
    QByteArray body = "name=Alice&age=42";

    auto result = sub.submit(url, body,
                             "application/x-www-form-urlencoded",
                             3000);

    EXPECT_TRUE(result.success) << result.errorMessage.toStdString();
    EXPECT_EQ(result.httpStatus, 200);
    EXPECT_EQ(result.responseBody, QByteArray("mock-ok"));

    // Server saw our exact payload.
    EXPECT_EQ(mock.lastBody(), body);
    const QByteArray hdr = mock.lastRequest();
    EXPECT_TRUE(hdr.contains("POST /submit"));
    EXPECT_TRUE(hdr.toLower().contains("content-type: application/x-www-form-urlencoded"));
}

TEST(FormSubmitterLive, InvalidUrlReturnsErrorNotCrash)
{
    FormSubmitter sub;
    auto result = sub.submit(QUrl(), QByteArray("x"),
                             "application/octet-stream", 500);
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.errorMessage.isEmpty());
}

TEST(FormSubmitterLive, UnreachablePortReportsNetworkError)
{
    FormSubmitter sub;
    sub.setAllowPrivateDestinations(true);   // see the note above
    // Port 1 on localhost is almost certainly closed.
    auto result = sub.submit(QUrl("http://127.0.0.1:1/nope"),
                             QByteArray("x"),
                             "application/octet-stream", 1500);
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.errorMessage.isEmpty());
}

#include "test_form_submitter.moc"
