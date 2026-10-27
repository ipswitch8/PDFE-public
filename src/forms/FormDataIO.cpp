#include "FormDataIO.h"
#include "FormManager.h"
#include "FormFieldData.h"
#include "core/Document.h"
#include "core/Page.h"
#include "core/Error.h"
#include "core/XmlSafety.h"
#include "core/MuPdfContext.h"
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
#include <QXmlStreamWriter>
#include <QXmlStreamReader>
#include <QString>
#include <QByteArray>
#include <map>
#include <stdexcept>

namespace pdfe::forms {

class FormDataIO::Impl {
public:
    std::shared_ptr<pdfe::core::MuPdfContext> context;
};

FormDataIO::FormDataIO(std::shared_ptr<pdfe::core::MuPdfContext> ctx)
    : pImpl(std::make_unique<Impl>()) {
    if (!ctx) {
        throw std::invalid_argument("Context cannot be null");
    }
    pImpl->context = std::move(ctx);
}

FormDataIO::~FormDataIO() = default;

FormDataIO::FormDataIO(FormDataIO&&) noexcept = default;
FormDataIO& FormDataIO::operator=(FormDataIO&&) noexcept = default;

std::string FormDataIO::exportToXfdf(pdfe::core::Document* doc, FormManager* manager) {
    if (!doc) {
        throw std::invalid_argument("Document cannot be null");
    }
    if (!manager) {
        throw std::invalid_argument("FormManager cannot be null");
    }

    // Collect unique field name -> value pairs (deduplicate across pages)
    std::map<std::string, std::string> fieldValues;

    for (int pageIndex = 0; pageIndex < doc->pageCount(); ++pageIndex) {
        auto page = doc->page(pageIndex);
        if (!page) {
            continue;
        }

        auto fields = manager->getFormFields(page.get());
        for (const auto& field : fields) {
            // Only add if not already present (include empty values for round-trip fidelity)
            if (!field.fieldName.empty() && fieldValues.find(field.fieldName) == fieldValues.end()) {
                fieldValues[field.fieldName] = field.value;
            }
        }
    }

    // Generate XFDF XML
    QString xmlBuffer;
    QXmlStreamWriter writer(&xmlBuffer);

    writer.setAutoFormatting(true);
    writer.writeStartDocument("1.0");

    // Start xfdf element with namespace
    writer.writeStartElement("xfdf");
    writer.writeAttribute("xmlns", "http://ns.adobe.com/xfdf/");
    writer.writeAttribute("xml:space", "preserve");

    // Start fields element
    writer.writeStartElement("fields");

    // Write each field
    for (const auto& [name, value] : fieldValues) {
        writer.writeStartElement("field");
        writer.writeAttribute("name", QString::fromStdString(name));

        writer.writeStartElement("value");
        writer.writeCharacters(QString::fromStdString(value));
        writer.writeEndElement(); // value

        writer.writeEndElement(); // field
    }

    writer.writeEndElement(); // fields
    writer.writeEndElement(); // xfdf

    writer.writeEndDocument();

    return xmlBuffer.toStdString();
}

void FormDataIO::importFromXfdf(pdfe::core::Document* doc, FormManager* manager,
                                 const std::string& xfdfContent) {
    if (!doc) {
        throw std::invalid_argument("Document cannot be null");
    }
    if (!manager) {
        throw std::invalid_argument("FormManager cannot be null");
    }
    if (xfdfContent.empty()) {
        throw std::invalid_argument("XFDF content cannot be empty");
    }

    // Meta-audit F13 (SEC-41 / SEC-42): XFDF is attacker-supplied XML. ISO
    // 32000-2 §12.7.8 defines no XFDF entities beyond the XML built-ins, so a
    // DTD, an entity declaration or an external identifier is refused BEFORE
    // QXmlStreamReader is constructed. This makes the refusal PDFE's own and
    // observable (a thrown PdfeException) rather than an untested assumption
    // about Qt's entity defaults.
    {
        const auto verdict = pdfe::core::xmlsafety::inspect(xfdfContent);
        if (!verdict.safe()) {
            throw core::PdfeException(
                "Refused XFDF: " + verdict.detail
            );
        }
    }

    // Parse XFDF XML
    QString xmlContent = QString::fromStdString(xfdfContent);
    QXmlStreamReader reader(xmlContent);
    // Defence in depth behind the pre-parse guard: even if a construct slips
    // past the lexical scan, Qt must not expand an entity chain into a bomb.
    // XFDF needs no entity expansion at all, so the limit is set far below
    // Qt's own 4096 default.
    reader.setEntityExpansionLimit(pdfe::core::xmlsafety::kEntityExpansionLimit);

    std::map<std::string, std::string> fieldValues;
    QString currentFieldName;
    QString currentFieldValue;
    bool inFields = false;
    bool inField = false;
    bool inValue = false;

    while (!reader.atEnd()) {
        reader.readNext();

        if (reader.hasError()) {
            throw core::PdfeException(
                "Failed to parse XFDF: " + reader.errorString().toStdString()
            );
        }

        // F13 (SEC-42): a DTD token reaching this loop means the pre-parse
        // guard was bypassed. Refuse rather than continue.
        if (reader.isDTD()) {
            throw core::PdfeException(
                "Refused XFDF: document type (DTD) declarations are not "
                "accepted in XFDF input"
            );
        }

        if (reader.isStartElement()) {
            QString name = reader.name().toString();

            if (name == "fields") {
                inFields = true;
            } else if (name == "field" && inFields) {
                inField = true;
                currentFieldName = reader.attributes().value("name").toString();
                currentFieldValue.clear();
            } else if (name == "value" && inField) {
                inValue = true;
            }
        } else if (reader.isCharacters() && inValue) {
            currentFieldValue += reader.text().toString();
        } else if (reader.isEndElement()) {
            QString name = reader.name().toString();

            if (name == "value" && inValue) {
                inValue = false;
            } else if (name == "field" && inField) {
                if (!currentFieldName.isEmpty()) {
                    fieldValues[currentFieldName.toStdString()] =
                        currentFieldValue.toStdString();
                }
                inField = false;
                currentFieldName.clear();
                currentFieldValue.clear();
            } else if (name == "fields") {
                inFields = false;
            }
        }
    }

    // Apply values to form fields
    applyFieldValues(doc, manager, fieldValues);
}

// ─── F15 / PDF-40: FDF import ────────────────────────────────────────────

namespace {

// Recursively walk an FDF /Fields (or /Kids) array, building fully-qualified
// field names. `prefix` is the parent's qualified name ("" at the top level).
// Depth-limited so a hand-crafted FDF cannot drive unbounded recursion.
void collectFdfFields(fz_context* ctx, pdf_obj* fieldsArray,
                      const std::string& prefix,
                      std::map<std::string, std::string>& out,
                      int depth)
{
    if (!fieldsArray || depth > 32) {
        return;
    }
    fieldsArray = pdf_resolve_indirect(ctx, fieldsArray);
    if (!pdf_is_array(ctx, fieldsArray)) {
        return;
    }

    const int n = pdf_array_len(ctx, fieldsArray);
    for (int i = 0; i < n; ++i) {
        pdf_obj* entry = pdf_resolve_indirect(ctx, pdf_array_get(ctx, fieldsArray, i));
        if (!pdf_is_dict(ctx, entry)) {
            continue;
        }

        std::string name = prefix;
        pdf_obj* t = pdf_resolve_indirect(ctx, pdf_dict_get(ctx, entry, PDF_NAME(T)));
        if (pdf_is_string(ctx, t)) {
            const char* partial = pdf_to_text_string(ctx, t);
            if (partial && *partial) {
                if (!name.empty()) name += ".";
                name += partial;
            }
        }

        // Value. Strings are text values; names are button states (/Yes, /Off);
        // numbers appear for numeric-formatted fields.
        pdf_obj* v = pdf_resolve_indirect(ctx, pdf_dict_get(ctx, entry, PDF_NAME(V)));
        if (v && !name.empty()) {
            if (pdf_is_string(ctx, v)) {
                const char* s = pdf_to_text_string(ctx, v);
                out[name] = s ? s : "";
            } else if (pdf_is_name(ctx, v)) {
                const char* s = pdf_to_name(ctx, v);
                out[name] = s ? s : "";
            } else if (pdf_is_number(ctx, v)) {
                char buf[64];
                fz_snprintf(buf, sizeof(buf), "%g", pdf_to_real(ctx, v));
                out[name] = buf;
            }
        }

        // Children (hierarchical fields).
        pdf_obj* kids = pdf_dict_get(ctx, entry, PDF_NAME(Kids));
        if (kids) {
            collectFdfFields(ctx, kids, name, out, depth + 1);
        }
    }
}

} // namespace

void FormDataIO::importFromFdf(pdfe::core::Document* doc, FormManager* manager,
                               const std::string& fdfContent) {
    if (!doc) {
        throw std::invalid_argument("Document cannot be null");
    }
    if (!manager) {
        throw std::invalid_argument("FormManager cannot be null");
    }
    if (fdfContent.empty()) {
        throw std::invalid_argument("FDF content cannot be empty");
    }

    // Refuse anything that is not an FDF up front. An FDF begins with the
    // %FDF-n.m header (ISO 32000-1 §12.7.7.1). Refusing here means an
    // arbitrary blob — a PDF, an XFDF, or a hostile file of another type —
    // never reaches the object parser through this entry point.
    if (fdfContent.rfind("%FDF-", 0) != 0) {
        throw core::PdfeException(
            "Refused FDF: content does not start with the %FDF- header");
    }

    // Container built BEFORE any fz_try (MuPDF longjmp discipline).
    std::map<std::string, std::string> fieldValues;
    bool sawFieldsArray = false;

    pImpl->context->execute([&](fz_context* ctx) {
        fz_buffer* buf = NULL;
        fz_stream* stm = NULL;
        pdf_document* fdf = NULL;

        fz_try(ctx) {
            buf = fz_new_buffer_from_copied_data(
                ctx,
                reinterpret_cast<const unsigned char*>(fdfContent.data()),
                fdfContent.size());
            stm = fz_open_buffer(ctx, buf);
            fdf = pdf_open_document_with_stream(ctx, stm);

            pdf_obj* trailer = pdf_trailer(ctx, fdf);
            pdf_obj* root = pdf_dict_get(ctx, trailer, PDF_NAME(Root));
            pdf_obj* fdfDict = pdf_dict_gets(ctx, root, "FDF");
            pdf_obj* fields = pdf_dict_get(ctx, fdfDict, PDF_NAME(Fields));
            if (pdf_is_array(ctx, fields)) {
                sawFieldsArray = true;
                collectFdfFields(ctx, fields, std::string(), fieldValues, 0);
            }
            // NOTE (security): /JavaScript, /F, /ID and any action
            // dictionaries present in this FDF are deliberately never read
            // and never executed. Only /Fields is consulted.
        }
        fz_always(ctx) {
            pdf_drop_document(ctx, fdf);
            fz_drop_stream(ctx, stm);
            fz_drop_buffer(ctx, buf);
        }
        fz_catch(ctx) {
            fz_rethrow(ctx);
        }
    });

    if (!sawFieldsArray) {
        throw core::PdfeException(
            "Refused FDF: no /FDF /Fields array found");
    }

    applyFieldValues(doc, manager, fieldValues);
}

void FormDataIO::applyFieldValues(
    pdfe::core::Document* doc, FormManager* manager,
    const std::map<std::string, std::string>& fieldValues)
{
    for (int pageIndex = 0; pageIndex < doc->pageCount(); ++pageIndex) {
        auto page = doc->page(pageIndex);
        if (!page) {
            continue;
        }

        auto fields = manager->getFormFields(page.get());
        for (const auto& field : fields) {
            auto it = fieldValues.find(field.fieldName);
            if (it != fieldValues.end()) {
                manager->setFieldValue(page.get(), field.widgetIndex, it->second);
            }
        }
    }
}

} // namespace pdfe::forms
