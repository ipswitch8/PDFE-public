#include "FormEncoder.h"

#include <cstdio>
#include <sstream>

namespace pdfe::formsubmit {

std::string FormEncoder::percentEncodeFormValue(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        const bool alphaNum = (c >= '0' && c <= '9') ||
                              (c >= 'A' && c <= 'Z') ||
                              (c >= 'a' && c <= 'z');
        if (alphaNum || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else if (c == ' ') {
            out.push_back('+');
        } else {
            char buf[4];
            std::snprintf(buf, sizeof(buf), "%%%02X", c);
            out += buf;
        }
    }
    return out;
}

std::string FormEncoder::xmlEscape(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        switch (c) {
            case '&':  out += "&amp;"; break;
            case '<':  out += "&lt;"; break;
            case '>':  out += "&gt;"; break;
            case '"':  out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default:
                // XML-ILLEGAL CONTROL CHARACTERS. XML 1.0 §2.2 permits exactly
                // three characters below 0x20 — TAB, LF, CR — and nothing else.
                // Everything from 0x00-0x08, 0x0B, 0x0C and 0x0E-0x1F is
                // forbidden OUTRIGHT: not merely in need of escaping, but
                // unrepresentable, because a numeric character reference to
                // them (`&#1;`) is itself illegal in XML 1.0. So they cannot be
                // escaped, only dropped.
                //
                // The field value is document-controlled, and the output is an
                // FDF/XFDF document a receiving parser will read. Emitting a
                // raw 0x00-0x1F byte produces XML that is not well-formed,
                // which turns "the form submitted" into "the recipient's parser
                // rejected the whole submission" — or, on a lenient parser,
                // into whatever that parser decides the bytes meant. The
                // sibling pdfLiteralEscape in this same file already handles
                // control characters; xmlEscape was the outlier.
                //
                // 0x7F (DEL) is legal in XML 1.0 but is stripped by the same
                // argument that removed it in sanitizeFilename: it is
                // invisible, carries no meaning in a form value, and its only
                // use is to make two different values look identical.
                if ((c < 0x20 && c != '\t' && c != '\n' && c != '\r') ||
                    c == 0x7F) {
                    break;      // dropped
                }
                out.push_back(static_cast<char>(c));
        }
    }
    return out;
}

std::string FormEncoder::pdfLiteralEscape(const std::string& s)
{
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '(':  out += "\\("; break;
            case ')':  out += "\\)"; break;
            case '\r': out += "\\r"; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20 || c == 0x7f) {
                    char oct[5];
                    std::snprintf(oct, sizeof(oct), "\\%03o", c);
                    out += oct;
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    return out;
}

std::string FormEncoder::contentType(SubmitFormat f)
{
    switch (f) {
        case SubmitFormat::UrlEncoded: return "application/x-www-form-urlencoded";
        case SubmitFormat::Fdf:        return "application/vnd.fdf";
        case SubmitFormat::Xfdf:       return "application/vnd.adobe.xfdf";
    }
    return "application/octet-stream";
}

namespace {

QByteArray encodeUrl(const std::vector<FormField>& fields)
{
    std::string out;
    bool first = true;
    for (const auto& f : fields) {
        if (!first) out.push_back('&');
        first = false;
        out += FormEncoder::percentEncodeFormValue(f.name);
        out.push_back('=');
        out += FormEncoder::percentEncodeFormValue(f.value);
    }
    return QByteArray::fromStdString(out);
}

QByteArray encodeFdf(const std::vector<FormField>& fields)
{
    // Header + one object containing the field dictionary.
    //
    // F15 / PDF-40 defect fix: names and values are PDF literal strings and
    // MUST be escaped. Before this, a value containing ')' closed the literal
    // early, producing a corrupt FDF (best case) or letting a crafted value
    // inject arbitrary FDF dictionary syntax into the submitted payload
    // (worst case). See FormEncoder::pdfLiteralEscape.
    std::ostringstream os;
    os << "%FDF-1.2\n"
       << "1 0 obj\n"
       << "<<\n"
       << "/FDF << /Fields [\n";
    for (const auto& f : fields) {
        os << "<< /T (" << FormEncoder::pdfLiteralEscape(f.name)
           << ") /V (" << FormEncoder::pdfLiteralEscape(f.value) << ") >>\n";
    }
    os << "] >>\n"
       << ">>\n"
       << "endobj\n"
       << "trailer\n<< /Root 1 0 R >>\n"
       << "%%EOF\n";
    return QByteArray::fromStdString(os.str());
}

QByteArray encodeXfdf(const std::vector<FormField>& fields)
{
    std::ostringstream os;
    os << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
       << "<xfdf xmlns=\"http://ns.adobe.com/xfdf/\" xml:space=\"preserve\">\n"
       << "  <fields>\n";
    for (const auto& f : fields) {
        os << "    <field name=\"" << FormEncoder::xmlEscape(f.name) << "\">"
           << "<value>" << FormEncoder::xmlEscape(f.value) << "</value>"
           << "</field>\n";
    }
    os << "  </fields>\n"
       << "</xfdf>\n";
    return QByteArray::fromStdString(os.str());
}

} // namespace

QByteArray FormEncoder::encode(const std::vector<FormField>& fields,
                               SubmitFormat format)
{
    switch (format) {
        case SubmitFormat::UrlEncoded: return encodeUrl(fields);
        case SubmitFormat::Fdf:        return encodeFdf(fields);
        case SubmitFormat::Xfdf:       return encodeXfdf(fields);
    }
    return {};
}

} // namespace pdfe::formsubmit
