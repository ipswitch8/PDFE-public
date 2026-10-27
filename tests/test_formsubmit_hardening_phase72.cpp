// -*- coding: utf-8 -*-
//
// Phase-19 (follow-up phase-72) — `src/formsubmit`.
//
// THIS CODE IS DEAD TODAY, AND THAT IS STATED UP FRONT RATHER THAN DISCOVERED.
// The only reference to `src/formsubmit` outside its own directory is a comment
// (`AppSettings.h:119`), and the UI's "Submit Form..." item is a stub whose
// accepted URL is never read. Current attack surface from a malicious PDF is
// therefore ZERO, consistent with the closed SEC-52. Everything below is
// PREVENTATIVE hardening for whenever this gets wired up — not the fix for a
// live exploit, and it must not be cited as one.
//
// Which raises the obvious question these tests have to answer: why write them
// at all? Because the day someone wires this in is exactly the day nobody
// re-audits it. The defects here are the ones that read as finished code — a
// scheme check that exists in the function next door, a redirect policy whose
// name contains the word "Safe", an allowlist that looks like it handles ports.

#include <gtest/gtest.h>

#include "formsubmit/FormSubmitter.h"
#include "formsubmit/HostAllowlist.h"
#include "formsubmit/FormEncoder.h"

#include <QCoreApplication>
#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrl>

#include <memory>
#include <string>

using pdfe::formsubmit::FormEncoder;
using pdfe::formsubmit::FormSubmitter;
using pdfe::formsubmit::HostAllowlist;
using pdfe::formsubmit::SubmitDecision;

// ─────────────────────────────────────────────────────────────────────────────
// Criterion 5 — HostAllowlist and IPv6
//
// The register says `resize(colon)` "breaks IPv6 literals". It is worse than
// that, and the difference matters. `QUrl::host()` returns an IPv6 address
// WITHOUT brackets, so the first colon sits at index 0 or 1 and every IPv6
// host collapsed to the same short prefix — usually the empty string.
// Allowlisting one IPv6 host allowlisted ALL of them.
// ─────────────────────────────────────────────────────────────────────────────

TEST(HostAllowlistIPv6, DistinctAddressesDoNotCollapseIntoOneEntry)
{
    HostAllowlist list({"::1"});

    EXPECT_TRUE(list.contains("::1"));
    EXPECT_FALSE(list.contains("2001:db8::42"))
        << "a different IPv6 address matched an allowlist built from ::1 — "
           "the normaliser truncated both to the same prefix, so approving "
           "one host approved every IPv6 host";
    EXPECT_FALSE(list.contains("fe80::1"));
}

TEST(HostAllowlistIPv6, AnIpv6HostDoesNotNormaliseToTheEmptyString)
{
    // The specific mechanism, pinned separately: if normalize() returns "",
    // then contains("") is true for a list containing any IPv6 host, and an
    // EMPTY host — which shouldSubmit is supposed to reject — starts matching.
    EXPECT_FALSE(HostAllowlist::normalize("::1").empty());
    EXPECT_FALSE(HostAllowlist::normalize("2001:db8::42").empty());
    EXPECT_EQ(HostAllowlist::normalize("::1"), "::1");
}

TEST(HostAllowlistIPv6, BracketedFormWithAndWithoutPortMatchesTheBareForm)
{
    HostAllowlist list({"::1"});
    EXPECT_TRUE(list.contains("[::1]"));
    EXPECT_TRUE(list.contains("[::1]:8080"))
        << "the port survived into the compared host";
    EXPECT_TRUE(list.contains("[::1]:443"));
}

TEST(HostAllowlistIPv6, Ipv6IsCaseInsensitiveLikeAnyOtherHost)
{
    HostAllowlist list({"2001:DB8::AB"});
    EXPECT_TRUE(list.contains("2001:db8::ab"));
}

TEST(HostAllowlistIPv4, PortStrippingAndCaseFoldingStillWork)
{
    // THE CONTROL for the IPv6 fix. A normaliser that stopped stripping ports
    // altogether would satisfy every IPv6 case above and break the ordinary
    // one, which is the shape this file is otherwise about.
    HostAllowlist list({"forms.example.com"});
    EXPECT_TRUE(list.contains("forms.example.com:8443"));
    EXPECT_TRUE(list.contains("FORMS.Example.COM"));
    EXPECT_TRUE(list.contains("forms.example.com."));      // trailing IDNA dot
    EXPECT_FALSE(list.contains("evil.example.com"));
    EXPECT_FALSE(list.contains("forms.example.com.evil.tld"));
}

// ─────────────────────────────────────────────────────────────────────────────
// Criterion 4 — SSRF
// ─────────────────────────────────────────────────────────────────────────────

TEST(FormSubmitSsrf, PrivateAndLoopbackDestinationsAreRefused)
{
    for (const char* u : {
             "http://127.0.0.1/submit",
             "http://127.13.9.2/submit",          // all of 127/8, not just .1
             "http://localhost/submit",
             "http://sub.localhost/submit",
             "http://10.0.0.5/submit",
             "http://172.16.0.1/submit",
             "http://172.31.255.254/submit",
             "http://192.168.1.1/submit",
             "http://100.64.0.1/submit",          // CGNAT
             "http://0.0.0.0/submit",
             "https://[::1]/submit",
             "https://[fe80::1]/submit",          // link-local
             "https://[fc00::1]/submit",          // unique-local
         }) {
        EXPECT_TRUE(FormSubmitter::isBlockedDestination(QUrl(u)))
            << "not blocked: " << u;
    }
}

TEST(FormSubmitSsrf, TheCloudMetadataAddressIsRefusedIncludingIpv4Mapped)
{
    // 169.254.169.254 is the single most valuable SSRF target there is: on the
    // major clouds it serves instance credentials to anything that asks.
    EXPECT_TRUE(FormSubmitter::isBlockedDestination(
        QUrl("http://169.254.169.254/latest/meta-data/")));

    // And the same address written as IPv4-mapped IPv6, which is the standard
    // way of walking past a blocklist that only looks at IPv4.
    EXPECT_TRUE(FormSubmitter::isBlockedDestination(
        QUrl("http://[::ffff:169.254.169.254]/latest/meta-data/")))
        << "the IPv4-mapped form of the metadata address was allowed; a "
           "blocklist that can be bypassed by rewriting the literal is not a "
           "blocklist";
    EXPECT_TRUE(FormSubmitter::isBlockedDestination(
        QUrl("http://[::ffff:127.0.0.1]/submit")));

    // THE MAPPED FORMS THAT QT DOES **NOT** COVER, and the reason this block
    // exists at all. Measured, not assumed (scratchpad probe, phase-19): with
    // the explicit IPv4 table removed, Qt's own predicates still block
    // ::ffff:169.254.169.254 and ::ffff:127.0.0.1 — but 10/8, 172.16/12,
    // 192.168/16 and 100.64/10 go straight through, in mapped form as well as
    // plain. So the IPv4-mapped re-check is load-bearing precisely for the
    // RFC1918 ranges, which is exactly what the first version of this test
    // failed to try: the mutation removing the re-check SURVIVED, because
    // every mapped address asserted here happened to be one Qt handles anyway.
    EXPECT_TRUE(FormSubmitter::isBlockedDestination(
        QUrl("http://[::ffff:10.0.0.5]/submit")))
        << "an RFC1918 address in IPv4-mapped form reached the network; Qt's "
           "predicates do not cover 10/8, so the mapped re-check is the only "
           "thing standing between a document and the internal network";
    EXPECT_TRUE(FormSubmitter::isBlockedDestination(
        QUrl("http://[::ffff:192.168.1.1]/submit")));
    EXPECT_TRUE(FormSubmitter::isBlockedDestination(
        QUrl("http://[::ffff:172.16.0.1]/submit")));
}

TEST(FormSubmitSsrf, OrdinaryPublicDestinationsAreStillAllowed)
{
    // THE CONTROL. "Block everything" passes every assertion above and makes
    // the feature useless the moment it is wired up.
    for (const char* u : {
             "https://forms.example.com/submit",
             "https://8.8.8.8/submit",
             "https://[2001:db8::42]/submit",     // documentation range, public
             "http://203.0.113.7/submit",
         }) {
        EXPECT_FALSE(FormSubmitter::isBlockedDestination(QUrl(u)))
            << "wrongly blocked: " << u;
    }
}

TEST(FormSubmitSsrf, AHostNameIsNotResolvedAndTheLimitIsDeliberate)
{
    // Pins the DOCUMENTED BOUNDARY rather than a behaviour we want. A name is
    // not resolved here, because resolving to decide and letting Qt resolve
    // again to connect is check-then-act across a resolver we do not control
    // (DNS rebinding). If someone later adds resolution here, this test should
    // FAIL and be rewritten together with connect-time enforcement — it must
    // not be "fixed" by deleting it.
    EXPECT_FALSE(FormSubmitter::isBlockedDestination(
        QUrl("http://rebind.example.com/submit")))
        << "a name is now being resolved at decision time; see the header "
           "comment on isBlockedDestination before changing this";
}

TEST(FormSubmitSsrf, ShouldSubmitReportsTheBlockDistinctlyFromAnInvalidUrl)
{
    HostAllowlist list({"127.0.0.1"});
    // Even an ALLOWLISTED private host is refused: the allowlist is about who
    // the user trusts, not about whether the address class is submittable.
    EXPECT_EQ(FormSubmitter::shouldSubmit(QUrl("http://127.0.0.1/s"), list),
              SubmitDecision::RejectBlockedDestination)
        << "an allowlist entry overrode the SSRF class check";
    EXPECT_EQ(FormSubmitter::shouldSubmit(QUrl("ftp://example.com/s"), list),
              SubmitDecision::RejectInvalidUrl)
        << "a bad scheme and a blocked address must be distinguishable, or "
           "the UI cannot tell the user which happened";
}

// ─────────────────────────────────────────────────────────────────────────────
// Criterion 2 — submit() enforces the scheme itself
// ─────────────────────────────────────────────────────────────────────────────

TEST(FormSubmitScheme, SubmitRefusesNonHttpSchemesWithoutConsultingShouldSubmit)
{
    // submit() is public. Nothing forces a caller through shouldSubmit, and
    // this class is dead code precisely because that caller has not been
    // written yet — so the first person to write it is the one who finds out
    // whether submit() defends itself.
    FormSubmitter sub;
    sub.setAllowPrivateDestinations(true);   // isolate the SCHEME rule

    for (const char* u : {"file:///etc/passwd",
                          "ftp://example.com/upload",
                          "javascript:alert(1)",
                          "data:text/plain,hi"}) {
        auto result = sub.submit(QUrl(u), QByteArray("secret=1"),
                                 "application/x-www-form-urlencoded", 500);
        EXPECT_FALSE(result.success) << u;
        EXPECT_TRUE(result.errorMessage.contains("http"))
            << "refused " << u << " but not for the scheme: "
            << result.errorMessage.toStdString();
    }
}

TEST(FormSubmitScheme, SubmitRefusesAPrivateDestinationUnlessExplicitlyAllowed)
{
    FormSubmitter sub;
    ASSERT_FALSE(sub.allowPrivateDestinations()) << "the default must be deny";

    auto blocked = sub.submit(QUrl("http://169.254.169.254/latest/meta-data/"),
                              QByteArray("x"), "text/plain", 500);
    EXPECT_FALSE(blocked.success);
    EXPECT_TRUE(blocked.errorMessage.contains("Refused"))
        << blocked.errorMessage.toStdString();

    // THE RECOVERY CONTROL, and the reason the opt-in is tested at all: a
    // submit() hard-wired to refuse would pass every assertion above. Opting
    // in must actually get past the destination check — the request then fails
    // on the network (nothing is listening), which is a DIFFERENT failure and
    // that difference is the whole point.
    sub.setAllowPrivateDestinations(true);
    auto allowed = sub.submit(QUrl("http://127.0.0.1:1/nope"),
                              QByteArray("x"), "text/plain", 1500);
    EXPECT_FALSE(allowed.success) << "port 1 should not be listening";
    EXPECT_FALSE(allowed.errorMessage.contains("Refused"))
        << "the opt-in did not take effect; still refused at the policy layer: "
        << allowed.errorMessage.toStdString();
}

// ─────────────────────────────────────────────────────────────────────────────
// Criterion 6 — xmlEscape and XML-illegal control characters
// ─────────────────────────────────────────────────────────────────────────────

TEST(FormEncoderXmlEscape, XmlIllegalControlCharactersAreRemoved)
{
    // XML 1.0 §2.2 allows exactly three characters below 0x20: TAB, LF, CR.
    // The rest cannot even be escaped — `&#1;` is itself illegal in XML 1.0 —
    // so they can only be dropped. The field value is document-controlled and
    // the output is an FDF/XFDF a receiving parser must read; a raw 0x01 makes
    // the whole submission not well-formed.
    std::string hostile = "before";
    hostile.push_back('\x01');
    hostile.push_back('\x0B');
    hostile.push_back('\x1F');
    hostile.push_back('\x7F');
    hostile += "after";

    const std::string out = FormEncoder::xmlEscape(hostile);
    EXPECT_EQ(out, "beforeafter");
    for (char c : out) {
        const unsigned char u = static_cast<unsigned char>(c);
        EXPECT_FALSE((u < 0x20 && u != '\t' && u != '\n' && u != '\r') || u == 0x7F)
            << "an XML-illegal control character survived escaping";
    }
}

TEST(FormEncoderXmlEscape, TheThreeLegalWhitespaceControlsAreKept)
{
    // THE CONTROL. Stripping everything below 0x20 would satisfy the case
    // above and silently destroy every multi-line form field, which is an
    // ordinary thing for a text area to contain.
    const std::string out = FormEncoder::xmlEscape("line1\nline2\r\n\tindented");
    EXPECT_NE(out.find('\n'), std::string::npos) << "LF was stripped";
    EXPECT_NE(out.find('\r'), std::string::npos) << "CR was stripped";
    EXPECT_NE(out.find('\t'), std::string::npos) << "TAB was stripped";
}

TEST(FormEncoderXmlEscape, TheFiveMarkupEntitiesStillEscape)
{
    // The pre-existing behaviour, pinned so the control-character work cannot
    // regress it.
    EXPECT_EQ(FormEncoder::xmlEscape("<a href=\"x\">&'</a>"),
              "&lt;a href=&quot;x&quot;&gt;&amp;&apos;&lt;/a&gt;");
}

TEST(FormEncoderXmlEscape, MultiByteUtf8IsNotMangled)
{
    // The other control: a byte-oriented "strip anything unusual" rule would
    // eat UTF-8 continuation bytes and destroy every non-ASCII field value.
    const std::string accented = "caf\xC3\xA9";                 // café
    const std::string japanese = "\xE6\x97\xA5\xE6\x9C\xAC";    // 日本
    EXPECT_EQ(FormEncoder::xmlEscape(accented), accented);
    EXPECT_EQ(FormEncoder::xmlEscape(japanese), japanese);
}

// ─────────────────────────────────────────────────────────────────────────────
// Criterion 3 — a redirect must not carry the POST body to another host
//
// This is the criterion with the widest blast radius, because the allowlist
// decision is made about the ORIGINAL url. A user approves
// `forms.example.com`; that host answers 302 to `attacker.example`; and with
// Qt's NoLessSafeRedirectPolicy — whose name contains the word "Safe", and
// which only refuses an https->http DOWNGRADE — the whole filled-in form is
// POSTed to a host nobody approved and the allowlist never saw.
//
// Proven end to end rather than by asserting the attribute value: asserting
// `RedirectPolicyAttribute == SameOriginRedirectPolicy` would be a test of my
// own assignment statement. Two real servers, and the oracle is whether the
// second one ever received the body.
// ─────────────────────────────────────────────────────────────────────────────

namespace {

/// Answers every request with a 302 to `location`. No Q_OBJECT: plain
/// QTcpServer plus lambdas, so this file needs no moc pass.
class RedirectingServer {
public:
    explicit RedirectingServer(const QByteArray& location)
    {
        server_.listen(QHostAddress::LocalHost, 0);
        QObject::connect(&server_, &QTcpServer::newConnection, [this, location]() {
            while (auto* sock = server_.nextPendingConnection()) {
                QObject::connect(sock, &QTcpSocket::readyRead, [this, sock, location]() {
                    seen_.append(sock->readAll());
                    if (!seen_.contains("\r\n\r\n")) return;
                    const QByteArray resp =
                        "HTTP/1.1 302 Found\r\n"
                        "Location: " + location + "\r\n"
                        "Content-Length: 0\r\n"
                        "Connection: close\r\n\r\n";
                    sock->write(resp);
                    sock->flush();
                    sock->disconnectFromHost();
                });
                QObject::connect(sock, &QTcpSocket::disconnected,
                                 sock, &QTcpSocket::deleteLater);
            }
        });
    }
    quint16 port() const { return server_.serverPort(); }

private:
    QTcpServer server_;
    QByteArray seen_;
};

/// Records anything it receives and answers 200. This is the attacker's
/// endpoint; the test passes only if it stays empty.
class RecordingServer {
public:
    RecordingServer()
    {
        server_.listen(QHostAddress::LocalHost, 0);
        QObject::connect(&server_, &QTcpServer::newConnection, [this]() {
            while (auto* sock = server_.nextPendingConnection()) {
                hits_++;
                QObject::connect(sock, &QTcpSocket::readyRead, [this, sock]() {
                    received_.append(sock->readAll());
                    const QByteArray resp =
                        "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n"
                        "Connection: close\r\n\r\nok";
                    sock->write(resp);
                    sock->flush();
                    sock->disconnectFromHost();
                });
                QObject::connect(sock, &QTcpSocket::disconnected,
                                 sock, &QTcpSocket::deleteLater);
            }
        });
    }
    quint16 port() const { return server_.serverPort(); }
    QByteArray received() const { return received_; }
    int hits() const { return hits_; }

private:
    QTcpServer server_;
    QByteArray received_;
    int hits_ = 0;
};

}  // namespace

TEST(FormSubmitRedirect, ACrossHostRedirectDoesNotCarryThePostBody)
{
    RecordingServer attacker;
    ASSERT_GT(attacker.port(), 0);

    // 127.0.0.1 -> localhost is a DIFFERENT ORIGIN even though both resolve to
    // the loopback interface: same-origin compares the host STRING, and that
    // is exactly the comparison the policy has to make.
    const QByteArray target =
        "http://localhost:" + QByteArray::number(attacker.port()) + "/steal";
    RedirectingServer victim(target);
    ASSERT_GT(victim.port(), 0);

    FormSubmitter sub;
    sub.setAllowPrivateDestinations(true);   // both servers are on loopback

    const QByteArray secret = "ssn=123-45-6789&salary=99000";
    const QUrl url(QStringLiteral("http://127.0.0.1:%1/submit").arg(victim.port()));
    auto result = sub.submit(url, secret,
                             "application/x-www-form-urlencoded", 3000);

    // THE ORACLE. Whether submit() reports success is secondary; what must be
    // true is that the form body never reached the other host.
    EXPECT_EQ(attacker.hits(), 0)
        << "the redirect was followed to another host at all";
    EXPECT_FALSE(attacker.received().contains(secret))
        << "THE FORM BODY WAS EXFILTRATED to a host the user never approved: "
        << attacker.received().toStdString();
    EXPECT_FALSE(result.success)
        << "a refused cross-host redirect must not be reported as a successful "
           "submission — the caller would tell the user their form was sent";
}

TEST(FormSubmitRedirect, ASameOriginRedirectIsStillFollowed)
{
    // THE CONTROL, and it is not optional. SameOriginRedirectPolicy is one
    // enum value away from ManualRedirectPolicy, which would pass the test
    // above by refusing EVERY redirect — including the ordinary
    // /submit -> /submit/ normalisation real endpoints emit constantly.
    //
    // The first draft of this control did not follow a redirect at all: it
    // POSTed directly and asserted that worked, under a name claiming
    // otherwise. That would have left "refuse every redirect" undetected while
    // reading like coverage — the same defect this phase is documenting in
    // other people's code.
    //
    // One server, two roles: the first request gets a 302 to /final on its own
    // origin, the second is recorded.
    QTcpServer server;
    ASSERT_TRUE(server.listen(QHostAddress::LocalHost, 0));
    const quint16 port = server.serverPort();

    int requests = 0;
    QByteArray finalBody;
    bool sawFinalPath = false;

    QObject::connect(&server, &QTcpServer::newConnection, [&]() {
        while (auto* sock = server.nextPendingConnection()) {
            auto buf = std::make_shared<QByteArray>();
            QObject::connect(sock, &QTcpSocket::readyRead, [&, sock, buf]() {
                buf->append(sock->readAll());
                const int hdrEnd = buf->indexOf("\r\n\r\n");
                if (hdrEnd < 0) return;
                int expected = 0;
                for (const auto& line : buf->left(hdrEnd).split('\n')) {
                    if (line.toLower().startsWith("content-length:"))
                        expected = line.mid(15).trimmed().toInt();
                }
                if (buf->size() - (hdrEnd + 4) < expected) return;

                QByteArray resp;
                if (buf->startsWith("POST /start")) {
                    ++requests;
                    resp = "HTTP/1.1 307 Temporary Redirect\r\n"
                           "Location: /final\r\n"
                           "Content-Length: 0\r\n"
                           "Connection: close\r\n\r\n";
                } else {
                    ++requests;
                    sawFinalPath = buf->startsWith("POST /final");
                    finalBody = buf->mid(hdrEnd + 4, expected);
                    resp = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n"
                           "Connection: close\r\n\r\nok";
                }
                sock->write(resp);
                sock->flush();
                sock->disconnectFromHost();
            });
            QObject::connect(sock, &QTcpSocket::disconnected,
                             sock, &QTcpSocket::deleteLater);
        }
    });

    FormSubmitter sub;
    sub.setAllowPrivateDestinations(true);

    // 307 preserves the method and body, so the redirected request is still a
    // POST carrying the form — which is what makes this the right control for
    // a policy about POST bodies.
    auto ok = sub.submit(QUrl(QStringLiteral("http://127.0.0.1:%1/start").arg(port)),
                         QByteArray("field=value"),
                         "application/x-www-form-urlencoded", 3000);

    EXPECT_TRUE(ok.success) << ok.errorMessage.toStdString();
    EXPECT_GE(requests, 2)
        << "the same-origin redirect was not followed; the policy is refusing "
           "every redirect, not just cross-origin ones";
    EXPECT_TRUE(sawFinalPath) << "the second request did not reach /final";
    EXPECT_TRUE(finalBody.contains("field=value"))
        << "the redirect was followed but the form body was lost";
}
