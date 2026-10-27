#pragma once

#include <QByteArray>
#include <string>
#include <utility>
#include <vector>

namespace pdfe::formsubmit {

/// A single form field's value as it appears on a PDF form at submit time.
struct FormField {
    std::string name;
    std::string value;

    /// PDF-48 / GitHub #18. The AcroForm `Ff` **Required** bit (bit position 2)
    /// for this field, as parsed by `FormManager` into
    /// `forms::FormFieldData::isRequired`.
    ///
    /// It lives on the wire struct rather than staying in the UI value object
    /// because the flag is only worth anything at SUBMIT time: before this,
    /// `isRequired` was parsed and then used solely to append the word
    /// "Required" to a label, so an empty required field submitted exactly
    /// like any other. `FormSubmitter::missingRequiredFields()` is what
    /// consumes it. Defaults false, so a caller that has not been taught about
    /// required fields keeps today's behaviour rather than silently refusing
    /// every submit.
    bool required = false;
};

/// Wire formats from PDF's /SubmitForm Flags bit. The spec covers a few
/// more (HTML, embedded PDF, etc.) but these three are the realistic
/// ones web backends handle today.
enum class SubmitFormat {
    UrlEncoded,  // application/x-www-form-urlencoded
    Fdf,         // Forms Data Format (PDF binary)
    Xfdf,        // XML Forms Data Format
};

/// Encodes a list of FormField values into the chosen wire format.
/// All methods are static + pure — no network, no Qt dialogs — so
/// tests can assert exact byte output.
class FormEncoder {
public:
    static QByteArray encode(const std::vector<FormField>& fields,
                             SubmitFormat format);

    /// MIME type that should accompany the encoded payload. Matches
    /// Acrobat's behavior for each format.
    static std::string contentType(SubmitFormat format);

    /// Percent-encode per RFC 3986 Section 2.3 (safe chars are
    /// ALPHA/DIGIT/"-._~"; space → '+'). Exposed for testing.
    static std::string percentEncodeFormValue(const std::string& s);

    /// XML-escape a form value for XFDF. Exposed for testing.
    static std::string xmlEscape(const std::string& s);

    /// F15 / PDF-40: escape a string for a PDF *literal string* body, i.e.
    /// the bytes that go between the parentheses of `(...)` in FDF output.
    /// Backslash, '(' and ')' are backslash-escaped and control bytes are
    /// written as 3-digit octal, so a field value can never terminate the
    /// literal early and inject FDF syntax. Exposed for testing.
    static std::string pdfLiteralEscape(const std::string& s);
};

} // namespace pdfe::formsubmit
