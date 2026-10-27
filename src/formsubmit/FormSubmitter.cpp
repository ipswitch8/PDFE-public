#include "FormSubmitter.h"

#include "FormEncoder.h"

#include <QByteArray>
#include <QEventLoop>
#include <QHostAddress>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStringList>
#include <QTimer>

namespace pdfe::formsubmit {

namespace {

/// True if `scheme` is one this class is willing to POST a form body to.
///
/// Kept as a free function so `shouldSubmit` and `submit` cannot drift apart:
/// the whole point of criterion 2 is that they enforce the SAME rule, and two
/// copies of a rule is how they stop being the same rule.
bool isSubmittableScheme(const QString& scheme)
{
    const auto s = scheme.toLower();
    return s == QLatin1String("http") || s == QLatin1String("https");
}

/// True if `s` carries no user answer: empty, or nothing but ASCII whitespace.
///
/// A `value.empty()` test is defeated by a single space, and "type a space to
/// get past the required field" is precisely the non-answer the flag exists to
/// stop. Only ASCII whitespace is folded here: the value is a byte string from
/// the PDF, and treating arbitrary Unicode blanks would need a decode this
/// layer does not do.
bool isBlankValue(const std::string& s)
{
    for (unsigned char c : s) {
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n' && c != '\f' &&
            c != '\v') {
            return false;
        }
    }
    return true;
}

}  // namespace

std::vector<std::string> FormSubmitter::missingRequiredFields(
    const std::vector<FormField>& fields)
{
    std::vector<std::string> missing;
    for (const auto& f : fields) {
        if (f.required && isBlankValue(f.value)) {
            missing.push_back(f.name);
        }
    }
    return missing;
}

bool FormSubmitter::isBlockedDestination(const QUrl& url)
{
    const QString host = url.host();
    if (host.isEmpty()) return true;

    // Names that resolve to the loopback interface by definition. Checked
    // before the literal parse because "localhost" is not an address literal.
    const QString lower = host.toLower();
    if (lower == QLatin1String("localhost") ||
        lower.endsWith(QLatin1String(".localhost"))) {
        return true;
    }

    QHostAddress addr(host);
    if (addr.isNull()) {
        // A NAME, not a literal. We deliberately do NOT resolve it here.
        //
        // Resolving to decide, then letting Qt resolve again to connect, is a
        // check-then-act across a resolver we do not control — classic DNS
        // rebinding, where the second answer is the attacker's internal
        // address. A check that can be defeated by the thing it checks is
        // worse than an absent one, because it reads as protection.
        //
        // The honest boundary: literals are blocked here; name-based rebinding
        // needs enforcement at CONNECT time (a custom QNetworkAccessManager /
        // socket-level hook), which belongs with whatever wires this feature
        // up. Stated in the header so a future caller inherits the limit
        // rather than the impression of coverage.
        //
        // ALTERNATE IP ENCODINGS LAND HERE TOO (phase-19 `security-audit`).
        // `QHostAddress` accepts only strict dotted-decimal IPv4 and standard
        // IPv6 literals, so `http://2130706433/` (decimal 127.0.0.1) and
        // `http://0177.0.0.1/` (octal) are not recognised as literals and fall
        // into this branch as if they were names. That is not a hole today —
        // the Windows resolver does not accept those forms either, so nothing
        // connects — but it is the same category as rebinding: a string this
        // function declined to classify. Connect-time enforcement closes both
        // at once, which is why they are disclosed together rather than
        // patched with a second parser here.
        return false;
    }

    if (addr.isLoopback() || addr.isLinkLocal() || addr.isSiteLocal() ||
        addr.isMulticast() || addr.isBroadcast() ||
        addr == QHostAddress::AnyIPv4 || addr == QHostAddress::AnyIPv6) {
        return true;
    }

    // WHAT QT ACTUALLY COVERS, MEASURED RATHER THAN ASSUMED.
    //
    // This comment used to say `isSiteLocal()` covers "the IPv4 RFC1918 ranges
    // in Qt 6". That is FALSE, and the phase-19 mutation harness is what
    // exposed it: with this whole block deleted, `10.0.0.5`, `172.16.0.1`,
    // `172.31.255.254`, `192.168.1.1`, `100.64.0.1` and `fc00::1` all sailed
    // through, while `127.0.0.1`, `169.254.169.254`, `0.0.0.0`, `::1` and
    // `fe80::1` were still refused by the predicates above.
    //
    // So the split is:
    //   Qt's predicates    — loopback, link-local, unspecified, multicast,
    //                        broadcast, and the IPv4-MAPPED forms of those
    //   this explicit table — RFC1918, CGNAT, 0/8, and (below) fc00::/7
    //
    // The 169.254/16 row is therefore REDUNDANT with isLinkLocal(), and its
    // mutation row survives for that reason. It is kept deliberately: it is the
    // cloud instance-metadata address, the single most valuable SSRF target
    // there is, and one duplicated comparison is a cheap hedge against a
    // library predicate changing under us. Disclosed rather than quietly left
    // in, because a line no test can redden is otherwise indistinguishable
    // from a line that does nothing — see DCA 0029.
    if (addr.protocol() == QAbstractSocket::IPv4Protocol) {
        const quint32 v4 = addr.toIPv4Address();
        const quint8 a = static_cast<quint8>((v4 >> 24) & 0xFF);
        const quint8 b = static_cast<quint8>((v4 >> 16) & 0xFF);
        if (a == 10) return true;                                  // 10/8
        if (a == 127) return true;                                 // 127/8
        if (a == 169 && b == 254) return true;                     // 169.254/16
        if (a == 172 && b >= 16 && b <= 31) return true;           // 172.16/12
        if (a == 192 && b == 168) return true;                     // 192.168/16
        if (a == 100 && b >= 64 && b <= 127) return true;          // 100.64/10 CGNAT
        if (a == 0) return true;                                   // 0/8
    } else {
        bool v4mapped = false;
        const quint32 v4 = addr.toIPv4Address(&v4mapped);
        if (v4mapped) {
            // ::ffff:169.254.169.254 must not walk past the IPv4 rules above.
            return isBlockedDestination(
                QUrl(url.scheme() + QLatin1String("://") +
                     QHostAddress(v4).toString()));
        }
        if (addr.isUniqueLocalUnicast()) return true;              // fc00::/7
    }
    return false;
}

FormSubmitter::FormSubmitter(QObject* parent)
    : QObject(parent)
    , nam_(std::make_unique<QNetworkAccessManager>(this))
{
}

FormSubmitter::~FormSubmitter() = default;

SubmitDecision FormSubmitter::shouldSubmit(const QUrl& url,
                                           const HostAllowlist& allowlist)
{
    if (!url.isValid() || url.host().isEmpty()) {
        return SubmitDecision::RejectInvalidUrl;
    }
    if (!isSubmittableScheme(url.scheme())) {
        return SubmitDecision::RejectInvalidUrl;
    }
    if (isBlockedDestination(url)) {
        return SubmitDecision::RejectBlockedDestination;
    }
    if (allowlist.contains(url.host().toStdString())) {
        return SubmitDecision::Allow;
    }
    return SubmitDecision::ConfirmNewHost;
}

SubmitDecision FormSubmitter::shouldSubmit(const QUrl& url,
                                           const HostAllowlist& allowlist,
                                           const std::vector<FormField>& fields)
{
    // Destination rules FIRST, deliberately. If the completeness check came
    // first, a hostile URL would be reported as "form incomplete" until the
    // user filled the form in and only then as an SSRF refusal — the reason
    // given for a rejection would depend on unrelated state, which is how a
    // security refusal ends up looking like a validation nag and being clicked
    // through. The same ordering argument as submit()'s scheme-before-host.
    const SubmitDecision base = shouldSubmit(url, allowlist);
    if (base == SubmitDecision::RejectInvalidUrl ||
        base == SubmitDecision::RejectBlockedDestination) {
        return base;
    }
    if (!missingRequiredFields(fields).empty()) {
        return SubmitDecision::RejectMissingRequiredField;
    }
    return base;
}

SubmitResult FormSubmitter::submitForm(const QUrl& url,
                                       const std::vector<FormField>& fields,
                                       SubmitFormat format,
                                       int timeoutMs)
{
    SubmitResult result;

    const auto missing = missingRequiredFields(fields);
    if (!missing.empty()) {
        QStringList names;
        for (const auto& n : missing) {
            names << (n.empty() ? QStringLiteral("(unnamed field)")
                                : QString::fromStdString(n));
        }
        result.errorMessage =
            QStringLiteral("Refused: required field(s) not filled in: %1")
                .arg(names.join(QStringLiteral(", ")));
        return result;
    }

    return submit(url, FormEncoder::encode(fields, format),
                  FormEncoder::contentType(format), timeoutMs);
}

SubmitResult FormSubmitter::submit(const QUrl& url,
                                   const QByteArray& payload,
                                   const std::string& contentType,
                                   int timeoutMs)
{
    SubmitResult result;

    // SCHEME FIRST, and the ordering is load-bearing rather than cosmetic.
    //
    // The host check used to come first, and `file:///etc/passwd`,
    // `javascript:alert(1)` and `data:text/plain,hi` all have an EMPTY host —
    // so they were refused as "Invalid URL" and the scheme rule never ran. The
    // outcome was safe by accident: safe because those URLs happen to lack a
    // host, not because this function declined the scheme. Any scheme that
    // does carry a host (`ftp://`) reached the network layer.
    //
    // A guard whose effect is produced by a different check is a guard that
    // disappears the moment the other check moves, and it reports the wrong
    // reason in the meantime — which is what the phase-19 test caught.
    if (!isSubmittableScheme(url.scheme())) {
        result.errorMessage = QStringLiteral("Refused: only http and https may receive a form submission");
        return result;
    }
    if (!url.isValid() || url.host().isEmpty()) {
        result.errorMessage = QStringLiteral("Invalid URL");
        return result;
    }

    // INDEPENDENT ENFORCEMENT, not a duplicate of shouldSubmit's.
    //
    // submit() is public and has no compiler-enforced relationship to
    // shouldSubmit: nothing stops a future caller from reaching it directly,
    // and this class is currently dead code precisely because nobody has
    // written that caller yet. A guard that only runs when a *convention* is
    // followed is a guard the first new caller silently opts out of.
    //
    // Before this, submit() checked only validity and a non-empty host — so a
    // `file://`, `ftp://` or `data:` URL that shouldSubmit rejects outright
    // would have been handed straight to QNetworkAccessManager. The scheme
    // half of that enforcement is above, ahead of the host check; the
    // destination half is here.
    if (!allowPrivate_ && isBlockedDestination(url)) {
        result.errorMessage = QStringLiteral("Refused: destination is a loopback, link-local or private address");
        return result;
    }

    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader,
                  QString::fromStdString(contentType));
    // SameOrigin, NOT NoLessSafe. NoLessSafeRedirectPolicy only refuses a
    // DOWNGRADE (https -> http); it will happily follow https://approved.example
    // to https://attacker.example, carrying the POST body — the whole filled-in
    // form — to a host the user never approved and the allowlist never saw.
    // The allowlist decision is made about the ORIGINAL url, so any redirect
    // that changes origin escapes it by construction.
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::SameOriginRedirectPolicy);

    QNetworkReply* reply = nam_->post(req, payload);
    if (!reply) {
        result.errorMessage = QStringLiteral("Failed to start request");
        return result;
    }

    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    timeout.start(timeoutMs);
    loop.exec();

    if (!reply->isFinished()) {
        result.errorMessage = QStringLiteral("Request timed out");
        reply->abort();
        reply->deleteLater();
        return result;
    }

    if (reply->error() != QNetworkReply::NoError) {
        result.errorMessage = reply->errorString();
    } else {
        result.success = true;
    }
    const QVariant statusCode =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
    if (statusCode.isValid()) {
        result.httpStatus = statusCode.toInt();
    }
    result.responseBody = reply->readAll();
    reply->deleteLater();
    return result;
}

} // namespace pdfe::formsubmit
