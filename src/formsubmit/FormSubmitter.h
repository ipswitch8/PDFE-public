#pragma once

#include "FormEncoder.h"
#include "HostAllowlist.h"

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QUrl>

#include <memory>
#include <string>
#include <vector>

class QNetworkAccessManager;

namespace pdfe::formsubmit {

/// Decision returned by FormSubmitter::shouldSubmit — lets the UI layer
/// decide whether to POST immediately, prompt the user, or bail out.
enum class SubmitDecision {
    Allow,             // host is in the allowlist
    ConfirmNewHost,    // host isn't allowlisted — UI should prompt
    RejectInvalidUrl,  // URL empty, malformed, or unsupported scheme
    RejectBlockedDestination,  // loopback / link-local / private address (SSRF)
    RejectMissingRequiredField, // a field flagged Required carries no value
};

/// Result of an actual submit() call.
struct SubmitResult {
    bool        success = false;
    int         httpStatus = 0;        // 0 if no HTTP response
    QByteArray  responseBody;          // (may be truncated by caller)
    QString     errorMessage;          // populated on failure
};

/// Posts a form payload to a URL via QNetworkAccessManager. Uses Qt's
/// default HTTPS cert chain — invalid certs cause the submit to fail
/// with an errorMessage describing the mismatch.
///
/// This class is the thin network-layer wrapper; FormEncoder produces
/// the payload and HostAllowlist provides the allow policy.
class FormSubmitter : public QObject {
    Q_OBJECT
public:
    explicit FormSubmitter(QObject* parent = nullptr);
    ~FormSubmitter() override;

    /// Decide whether a submit to `url` should proceed given the current
    /// allowlist. Pure, synchronous; no network I/O.
    static SubmitDecision shouldSubmit(const QUrl& url,
                                       const HostAllowlist& allowlist);

    /// PDF-48 / GitHub #18. As above, but ALSO refuses when a field flagged
    /// Required carries no value. Ordered so the destination rules still win:
    /// a submit aimed at 169.254.169.254 must be reported as an SSRF refusal
    /// whether or not the form is complete, otherwise filling the form in
    /// changes the reason a hostile URL is rejected.
    static SubmitDecision shouldSubmit(const QUrl& url,
                                       const HostAllowlist& allowlist,
                                       const std::vector<FormField>& fields);

    /// Names of the fields flagged Required that carry no value.
    ///
    /// "No value" is EMPTY-OR-WHITESPACE, not merely empty. A single space
    /// satisfies a `value.empty()` test while being exactly the non-answer the
    /// Required flag exists to prevent; a check that a space defeats is not a
    /// check. Names are returned (not a bool) so the UI can tell the user
    /// WHICH field is missing — "this form is incomplete" with no further
    /// information is a dead end on a 40-field form.
    ///
    /// Pure and static: no network, no dialogs.
    static std::vector<std::string> missingRequiredFields(
        const std::vector<FormField>& fields);

    /// True if `url`'s host is an address a form submission must never reach:
    /// loopback, link-local (incl. 169.254.169.254, cloud instance metadata),
    /// RFC1918 / unique-local, CGNAT, multicast, broadcast or unspecified —
    /// plus `localhost` by name. IPv4-mapped IPv6 is re-checked as IPv4 so
    /// `::ffff:169.254.169.254` cannot walk past the IPv4 rules.
    ///
    /// **LIMIT, stated so a caller inherits it rather than an impression of
    /// coverage:** a host NAME is not resolved here, so this does not stop DNS
    /// rebinding. Resolving to decide and then letting Qt resolve again to
    /// connect is check-then-act across a resolver we do not control — the
    /// second answer can be the attacker's internal address. Name-based
    /// rebinding must be enforced at CONNECT time, which belongs with whatever
    /// wires this feature up.
    static bool isBlockedDestination(const QUrl& url);

    /// THE DOCUMENTED EXCEPTION to isBlockedDestination(), and the only one.
    ///
    /// Default **false** — a form submission from a document must never reach
    /// a private address, because that is the whole SSRF class. It exists
    /// because one legitimate caller genuinely needs the opposite: a test
    /// posting to a `QTcpServer` bound on 127.0.0.1, which is the only honest
    /// way to exercise the real network path end to end.
    ///
    /// Deliberately per-INSTANCE and opt-in rather than a build flag or an
    /// environment variable: a caller has to write the line, so the exercise
    /// of a normally-blocked path is visible in the calling code and in code
    /// review, instead of being a property of how the binary was compiled.
    /// It does not relax the scheme check, and `shouldSubmit` — which is
    /// static, and is the decision the UI asks — is not affected by it at all.
    void setAllowPrivateDestinations(bool allow) { allowPrivate_ = allow; }
    bool allowPrivateDestinations() const { return allowPrivate_; }

    /// POST `payload` with `contentType` to `url` synchronously. Blocks
    /// up to `timeoutMs`. Returns a SubmitResult. HTTPS is the normal
    /// path; plain HTTP is allowed so local mock servers work in tests,
    /// but the UI layer should warn the user before calling submit()
    /// with a non-HTTPS URL.
    ///
    /// Enforces the http(s) scheme rule and isBlockedDestination() ITSELF,
    /// independently of shouldSubmit(): this method is public, and a guard
    /// that only runs when a calling convention is honoured is one the first
    /// new caller silently opts out of. Redirects use
    /// SameOriginRedirectPolicy, because the allowlist decision was made about
    /// the ORIGINAL url and any cross-origin redirect carries the POST body
    /// past it.
    SubmitResult submit(const QUrl& url,
                        const QByteArray& payload,
                        const std::string& contentType,
                        int timeoutMs = 15000);

    /// PDF-48 / GitHub #18. Encode `fields` in `format` and POST them.
    ///
    /// THIS IS THE OVERLOAD A CALLER SHOULD USE, and the required-field rule
    /// is enforced HERE rather than only in shouldSubmit() for the same reason
    /// the scheme and destination rules are re-enforced in submit(): a guard
    /// that runs only when a calling convention is honoured is one the first
    /// new caller silently opts out of. shouldSubmit() is advice the UI asks
    /// for so it can prompt; this is the rule.
    ///
    /// The raw submit(payload, contentType) overload above is deliberately
    /// left alone. It takes bytes, not fields, so it has nothing to check —
    /// which is itself the reason to route form submissions through here.
    SubmitResult submitForm(const QUrl& url,
                            const std::vector<FormField>& fields,
                            SubmitFormat format,
                            int timeoutMs = 15000);

private:
    std::unique_ptr<QNetworkAccessManager> nam_;
    bool allowPrivate_ = false;
};

} // namespace pdfe::formsubmit
