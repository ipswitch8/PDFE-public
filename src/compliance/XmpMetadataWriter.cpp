#include "XmpMetadataWriter.h"
#include "../core/MuPdfContext.h"
#include "../core/Document.h"
#include "../core/XmlSafety.h"
#include <pugixml.hpp>
#include <sstream>
#include <cstring>
#include <stdexcept>

extern "C" {
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
}

namespace pdfe::compliance {

namespace {

// Helper to safely read metadata from PDF Info dict.
//
// WRITES THROUGH AN OUT-PARAMETER RATHER THAN RETURNING BY VALUE, and that is
// the whole point (GitHub #118, fz_try/RAII hazard burn-down).
//
// `out` lives in the CALLER's frame. The returning form materialised a
// `std::string` inside the `fz_try` frame, and MuPDF's error handling is
// setjmp/longjmp: an `fz_throw` from a later `fz_lookup_metadata` jumps
// straight to the `fz_catch` inside MuPdfContext, skipping every destructor in
// between. Six of these were declared back-to-back at three call sites, so a
// throw on the third lookup leaked the heap buffers of the first two — real
// whenever a title or author exceeds the small-string optimisation, which is
// most real documents.
//
// `buf` is a raw char array: trivially destructible, so it is not a hazard.
void readInfoMetadataInto(fz_context* ctx, fz_document* fzDoc, const char* key,
                          std::string& out) {
    char buf[2048] = {0};
    int len = fz_lookup_metadata(ctx, fzDoc, key, buf, sizeof(buf));
    if (len > 0 && len < static_cast<int>(sizeof(buf))) {
        // ASSIGN FROM THE C STRING, NOT (buf, len).
        //
        // `fz_lookup_metadata` returns "the number of bytes need to store the
        // string plus terminator" (fitz/document.h), so `len` COUNTS THE NUL.
        // `assign(buf, len)` therefore produces a string one byte longer than
        // the text, with an embedded '\0' at the end -- which the original
        // `std::string(buf)` never had.
        //
        // Caught by the phase-10 security-audit gate reading MuPDF's contract,
        // not by a test -- and NOT COVERED BY ONE EITHER, which is recorded
        // here rather than glossed.
        //
        // A regression test was written and then DELETED because it could not
        // fail. It asserted on the emitted XMP packet, but every consumer
        // reaches these values through `.c_str()`
        // (`li.text().set(title.c_str())`), which stops at the NUL -- so the
        // packet is laundered and cannot show the defect. That is the "assert
        // against an input, never another output" trap. Reinstating
        // `assign(buf, len)` left the test GREEN, which is how it was found.
        //
        // The fix is kept regardless: it restores the semantics the original
        // `std::string(buf)` had, and a future consumer using `.size()` or
        // comparing these values would see the extra byte. There is no seam at
        // which it is currently observable, and inventing one purely for a test
        // was judged the wrong trade for a one-line correctness fix.
        //
        // This lifetime change was supposed to alter WHERE the strings live,
        // not what they contain.
        out.assign(buf);
    } else {
        out.clear();
    }
}

/// The six Info-dict values the XMP packet is built from.
///
/// Declared by the caller and filled through `readInfoSnapshot`, so every
/// `std::string` is destroyed by ordinary C++ unwinding after MuPdfContext
/// rethrows, instead of being longjmp'd past.
struct InfoSnapshot {
    std::string title;
    std::string author;
    std::string creator;
    std::string producer;
    std::string createDate;
    std::string modDate;
};

// Takes the raw fz_document*, not the Document*: `Document::fzDocument()` is
// private and reachable only from XmpMetadataWriter's own members, so the
// caller resolves it and passes it through.
void readInfoSnapshot(pdfe::core::MuPdfContext& context,
                      fz_document* fzDoc,
                      InfoSnapshot& out) {
    context.execute([&](fz_context* ctx) {
        readInfoMetadataInto(ctx, fzDoc, "info:Title",        out.title);
        readInfoMetadataInto(ctx, fzDoc, "info:Author",       out.author);
        readInfoMetadataInto(ctx, fzDoc, "info:Creator",      out.creator);
        readInfoMetadataInto(ctx, fzDoc, "info:Producer",     out.producer);
        readInfoMetadataInto(ctx, fzDoc, "info:CreationDate", out.createDate);
        readInfoMetadataInto(ctx, fzDoc, "info:ModDate",      out.modDate);
    });
}

// Convert PdfALevel to part/conformance strings
std::pair<std::string, std::string> pdfALevelToParts(PdfALevel level) {
    switch (level) {
        case PdfALevel::None:
            return {"", ""};
        case PdfALevel::PdfA_1b:
            return {"1", "B"};
        case PdfALevel::PdfA_1a:
            return {"1", "A"};
        case PdfALevel::PdfA_2b:
            return {"2", "B"};
        case PdfALevel::PdfA_2u:
            return {"2", "U"};
        case PdfALevel::PdfA_2a:
            return {"2", "A"};
        case PdfALevel::PdfA_3b:
            return {"3", "B"};
        case PdfALevel::PdfA_3u:
            return {"3", "U"};
        case PdfALevel::PdfA_3a:
            return {"3", "A"};
        default:
            return {"", ""};
    }
}

// Convert part/conformance strings to PdfALevel
PdfALevel partsToPdfALevel(const std::string& part, const std::string& conformance) {
    if (part == "1") {
        if (conformance == "A") return PdfALevel::PdfA_1a;
        if (conformance == "B") return PdfALevel::PdfA_1b;
    } else if (part == "2") {
        if (conformance == "A") return PdfALevel::PdfA_2a;
        if (conformance == "U") return PdfALevel::PdfA_2u;
        if (conformance == "B") return PdfALevel::PdfA_2b;
    } else if (part == "3") {
        if (conformance == "A") return PdfALevel::PdfA_3a;
        if (conformance == "U") return PdfALevel::PdfA_3u;
        if (conformance == "B") return PdfALevel::PdfA_3b;
    }
    return PdfALevel::None;
}

// Convert PdfUALevel to part string
std::string pdfUALevelToPart(PdfUALevel level) {
    switch (level) {
        case PdfUALevel::None:
            return "";
        case PdfUALevel::PdfUA_1:
            return "1";
        case PdfUALevel::PdfUA_2:
            return "2";
        default:
            return "";
    }
}

// Write an XMP packet into the document catalog's Root /Metadata stream,
// updating an existing stream in place or creating one if absent. Mirrors the
// write path used by writePdfAIdentification / writePdfUAIdentification.
void writeXmpToCatalog(fz_context* ctx, pdf_document* pdfDoc,
                       const std::vector<uint8_t>& xmpBytes) {
    pdf_obj* catalog = pdf_dict_get(ctx, pdf_trailer(ctx, pdfDoc), PDF_NAME(Root));
    if (!catalog) {
        fz_throw(ctx, FZ_ERROR_ARGUMENT, "Document catalog not found");
    }

    fz_buffer* buf = fz_new_buffer_from_copied_data(ctx,
        reinterpret_cast<const unsigned char*>(xmpBytes.data()),
        xmpBytes.size());

    fz_try(ctx) {
        pdf_obj* existingMeta = pdf_dict_get(ctx, catalog, PDF_NAME(Metadata));

        if (existingMeta && pdf_is_stream(ctx, existingMeta)) {
            pdf_update_stream(ctx, pdfDoc, existingMeta, buf, 0);
        } else {
            pdf_obj* stmObj = pdf_add_stream(ctx, pdfDoc, buf, nullptr, 0);

            fz_try(ctx) {
                pdf_dict_put(ctx, stmObj, PDF_NAME(Type), PDF_NAME(Metadata));
                pdf_dict_put_name(ctx, stmObj, PDF_NAME(Subtype), "XML");
                pdf_dict_put(ctx, catalog, PDF_NAME(Metadata), stmObj);
            } fz_always(ctx) {
                pdf_drop_obj(ctx, stmObj);
            } fz_catch(ctx) {
                fz_rethrow(ctx);
            }
        }
    } fz_always(ctx) {
        fz_drop_buffer(ctx, buf);
    } fz_catch(ctx) {
        fz_rethrow(ctx);
    }
}

} // anonymous namespace

class XmpMetadataWriter::Impl {
public:
    explicit Impl(std::shared_ptr<pdfe::core::MuPdfContext> context)
        : context_(std::move(context)) {}

    std::shared_ptr<pdfe::core::MuPdfContext> context_;
};

XmpMetadataWriter::XmpMetadataWriter(std::shared_ptr<pdfe::core::MuPdfContext> context)
    : impl_(std::make_unique<Impl>(std::move(context))) {
    if (!impl_->context_) {
        throw std::invalid_argument("MuPdfContext cannot be null");
    }
}

XmpMetadataWriter::~XmpMetadataWriter() = default;

void XmpMetadataWriter::writePdfAIdentification(pdfe::core::Document* doc, PdfALevel level) {
    if (!doc) {
        throw std::invalid_argument("Document cannot be null");
    }

    // Read existing metadata from document Info dict
    // #118: `metadata` and its six strings live HERE, in this frame, not inside
    // the fz_try lambda -- see readInfoSnapshot.
    InfoSnapshot metadata;
    readInfoSnapshot(*impl_->context_, doc->fzDocument(), metadata);

    // Build XMP packet with PDF/A identification
    auto xmpBytes = buildXmpPacket(
        level,
        PdfUALevel::None,
        metadata.title, // title
        metadata.author, // author
        metadata.creator, // creator
        metadata.producer, // producer
        metadata.createDate, // createDate
        metadata.modDate  // modDate
    );

    // Write XMP packet to document catalog
    impl_->context_->execute([&](fz_context* ctx) {
        pdf_document* pdfDoc = pdf_document_from_fz_document(ctx, doc->fzDocument());

        pdf_obj* catalog = pdf_dict_get(ctx, pdf_trailer(ctx, pdfDoc), PDF_NAME(Root));
        if (!catalog) {
            fz_throw(ctx, FZ_ERROR_ARGUMENT, "Document catalog not found");
        }

        fz_buffer* buf = fz_new_buffer_from_copied_data(ctx,
            reinterpret_cast<const unsigned char*>(xmpBytes.data()),
            xmpBytes.size());

        fz_try(ctx) {
            pdf_obj* existingMeta = pdf_dict_get(ctx, catalog, PDF_NAME(Metadata));

            if (existingMeta && pdf_is_stream(ctx, existingMeta)) {
                // Update existing metadata stream
                pdf_update_stream(ctx, pdfDoc, existingMeta, buf, 0);
            } else {
                // Create new metadata stream
                pdf_obj* stmObj = pdf_add_stream(ctx, pdfDoc, buf, nullptr, 0);

                fz_try(ctx) {
                    pdf_dict_put(ctx, stmObj, PDF_NAME(Type), PDF_NAME(Metadata));
                    pdf_dict_put_name(ctx, stmObj, PDF_NAME(Subtype), "XML");
                    pdf_dict_put(ctx, catalog, PDF_NAME(Metadata), stmObj);
                } fz_always(ctx) {
                    pdf_drop_obj(ctx, stmObj);
                } fz_catch(ctx) {
                    fz_rethrow(ctx);
                }
            }
        } fz_always(ctx) {
            fz_drop_buffer(ctx, buf);
        } fz_catch(ctx) {
            fz_rethrow(ctx);
        }
    });
}

void XmpMetadataWriter::writePdfUAIdentification(pdfe::core::Document* doc, PdfUALevel level) {
    if (!doc) {
        throw std::invalid_argument("Document cannot be null");
    }

    // Read existing metadata
    // #118: `metadata` and its six strings live HERE, in this frame, not inside
    // the fz_try lambda -- see readInfoSnapshot.
    InfoSnapshot metadata;
    readInfoSnapshot(*impl_->context_, doc->fzDocument(), metadata);

    // Detect existing PDF/A level
    PdfALevel existingPdfA = detectPdfALevel(doc);

    // Build XMP packet with both PDF/A and PDF/UA identification
    auto xmpBytes = buildXmpPacket(
        existingPdfA,
        level,
        metadata.title, // title
        metadata.author, // author
        metadata.creator, // creator
        metadata.producer, // producer
        metadata.createDate, // createDate
        metadata.modDate  // modDate
    );

    // Write XMP packet to document catalog
    impl_->context_->execute([&](fz_context* ctx) {
        pdf_document* pdfDoc = pdf_document_from_fz_document(ctx, doc->fzDocument());

        pdf_obj* catalog = pdf_dict_get(ctx, pdf_trailer(ctx, pdfDoc), PDF_NAME(Root));
        if (!catalog) {
            fz_throw(ctx, FZ_ERROR_ARGUMENT, "Document catalog not found");
        }

        fz_buffer* buf = fz_new_buffer_from_copied_data(ctx,
            reinterpret_cast<const unsigned char*>(xmpBytes.data()),
            xmpBytes.size());

        fz_try(ctx) {
            pdf_obj* existingMeta = pdf_dict_get(ctx, catalog, PDF_NAME(Metadata));

            if (existingMeta && pdf_is_stream(ctx, existingMeta)) {
                pdf_update_stream(ctx, pdfDoc, existingMeta, buf, 0);
            } else {
                pdf_obj* stmObj = pdf_add_stream(ctx, pdfDoc, buf, nullptr, 0);

                fz_try(ctx) {
                    pdf_dict_put(ctx, stmObj, PDF_NAME(Type), PDF_NAME(Metadata));
                    pdf_dict_put_name(ctx, stmObj, PDF_NAME(Subtype), "XML");
                    pdf_dict_put(ctx, catalog, PDF_NAME(Metadata), stmObj);
                } fz_always(ctx) {
                    pdf_drop_obj(ctx, stmObj);
                } fz_catch(ctx) {
                    fz_rethrow(ctx);
                }
            }
        } fz_always(ctx) {
            fz_drop_buffer(ctx, buf);
        } fz_catch(ctx) {
            fz_rethrow(ctx);
        }
    });
}

bool XmpMetadataWriter::syncFromInfo(pdfe::core::Document* doc) {
    if (!doc) {
        throw std::invalid_argument("Document cannot be null");
    }

    // Only sync when an XMP packet already exists. If the document carries no
    // XMP /Metadata, do nothing — never fabricate a packet on a plain Info edit.
    if (readXmpPacket(doc).empty()) {
        return false;
    }

    // Preserve the existing PDF/A conformance so a metadata edit never downgrades
    // it (the rebuilt packet re-emits the same pdfaid identification).
    PdfALevel existingPdfA = detectPdfALevel(doc);

    // Snapshot the now-current Info values.
    // #118: `metadata` and its six strings live HERE, in this frame, not inside
    // the fz_try lambda -- see readInfoSnapshot.
    InfoSnapshot metadata;
    readInfoSnapshot(*impl_->context_, doc->fzDocument(), metadata);

    // Rebuild the packet from current Info, preserving the detected PDF/A level.
    auto xmpBytes = buildXmpPacket(
        existingPdfA,
        PdfUALevel::None,
        metadata.title, // title      -> dc:title
        metadata.author, // author     -> dc:creator
        metadata.creator, // creator    -> xmp:CreatorTool
        metadata.producer, // producer   -> pdf:Producer
        metadata.createDate, // createDate -> xmp:CreateDate
        metadata.modDate  // modDate    -> xmp:ModifyDate
    );

    // Write it back into Root /Metadata via the shared write path.
    impl_->context_->execute([&](fz_context* ctx) {
        pdf_document* pdfDoc = pdf_document_from_fz_document(ctx, doc->fzDocument());
        if (!pdfDoc) {
            return;
        }
        writeXmpToCatalog(ctx, pdfDoc, xmpBytes);
    });

    return true;
}

std::string XmpMetadataWriter::readXmpPacket(pdfe::core::Document* doc) {
    if (!doc) {
        throw std::invalid_argument("Document cannot be null");
    }

    // #118: `result` lives in THIS frame, not inside the execute lambda. It used
    // to be declared after `pdf_load_stream`, so an fz_throw from
    // fz_buffer_storage (or the rethrow below) longjmp'd past its destructor and
    // leaked whatever it already held. An XMP packet is routinely kilobytes, so
    // this is never the small-string case.
    std::string result;

    impl_->context_->execute([&](fz_context* ctx) {
        pdf_document* pdfDoc = pdf_document_from_fz_document(ctx, doc->fzDocument());

        pdf_obj* catalog = pdf_dict_get(ctx, pdf_trailer(ctx, pdfDoc), PDF_NAME(Root));
        if (!catalog) {
            return;
        }

        pdf_obj* metaStream = pdf_dict_get(ctx, catalog, PDF_NAME(Metadata));
        if (!metaStream || !pdf_is_stream(ctx, metaStream)) {
            return;
        }

        fz_buffer* buf = pdf_load_stream(ctx, metaStream);

        fz_try(ctx) {
            unsigned char* data = nullptr;
            size_t len = fz_buffer_storage(ctx, buf, &data);
            if (data && len > 0) {
                result.assign(reinterpret_cast<const char*>(data), len);
            }
        } fz_always(ctx) {
            fz_drop_buffer(ctx, buf);
        } fz_catch(ctx) {
            fz_rethrow(ctx);
        }

    });

    return result;
}

bool XmpMetadataWriter::isXmpPacketAcceptable(const std::string& xmpPacket,
                                              std::string* reason) {
    const auto verdict = pdfe::core::xmlsafety::inspect(xmpPacket);
    if (verdict.safe()) {
        return true;
    }
    if (reason) {
        *reason = verdict.detail;
    }
    return false;
}

PdfALevel XmpMetadataWriter::detectPdfALevel(pdfe::core::Document* doc) {
    if (!doc) {
        throw std::invalid_argument("Document cannot be null");
    }

    std::string xmpData = readXmpPacket(doc);
    if (xmpData.empty()) {
        return PdfALevel::None;
    }

    // Meta-audit F13 (SEC-40/41): fail-closed BEFORE pugixml. A packet that
    // declares a DTD, an entity or an external identifier is refused outright;
    // an unparsed packet cannot leak a file or expand into a memory bomb.
    if (!isXmpPacketAcceptable(xmpData)) {
        return PdfALevel::None;
    }

    // Parse XMP with pugixml.
    //
    // Meta-audit F36. This was `load_string(xmpData.c_str())`, and that is a
    // NUL-terminated entry point: it stops at the first zero byte and takes the
    // whole buffer to be UTF-8. ISO 16684-1 §7.1 permits an XMP packet to be
    // UTF-8, UTF-16 or UTF-32, and a UTF-16LE packet's SECOND byte is already
    // NUL (`<` is `3C 00`), so exactly one byte reached the parser and every
    // UTF-16 packet in the world parsed as "not PDF/A".
    //
    // The consequence was not merely a missed detection. `syncFromInfo()` and
    // `writePdfUAIdentification()` both call this function to learn the level
    // they must PRESERVE, then rebuild the packet from that answer — so on a
    // UTF-16 PDF/A document an ordinary Info edit silently STRIPPED the
    // pdfaid identification, and `PdfAValidator::validate` reported a
    // conforming file as "XMP metadata missing PDF/A identification".
    //
    // `load_buffer` takes the length from the container (never from a NUL
    // scan) and, with `encoding_auto`, applies pugixml's own
    // `guess_buffer_encoding` — a leading BOM first, then the NUL pattern of
    // the first four bytes, which also covers the realistic packet whose only
    // BOM is inside the `<?xpacket begin=...?>` header. pugixml transcodes to
    // UTF-8 internally, so every node/attribute lookup below is unchanged.
    //
    // ORDER IS LOAD-BEARING. `isXmpPacketAcceptable()` above still runs on the
    // RAW bytes, before this line, and `xmlsafety::inspect()` does its own
    // encoding sniff + transcode (F27) precisely so a UTF-16 packet is scanned
    // rather than skipped. Widening what this parser accepts therefore does
    // not widen what PDFE accepts. Before F36 a hostile UTF-16 packet was also
    // "protected" by the accidental truncation here; that accident is now gone
    // and the F13/F27 guard is the only thing refusing it, which is why
    // XmpEncodingTest.Utf16HostilePacketIsStillRefusedByXmlSafetyNotByTruncation
    // proves the guard still fires on a packet pugixml would now parse.
    pugi::xml_document xmpDoc;
    pugi::xml_parse_result parseResult = xmpDoc.load_buffer(
        xmpData.data(), xmpData.size(),
        pugi::parse_default, pugi::encoding_auto);

    if (!parseResult) {
        return PdfALevel::None;
    }

    // Navigate to rdf:Description node with pdfaid namespace
    pugi::xml_node rdf = xmpDoc.child("x:xmpmeta").child("rdf:RDF");
    if (!rdf) {
        return PdfALevel::None;
    }

    // Look for pdfaid:part and pdfaid:conformance
    std::string part;
    std::string conformance;

    for (pugi::xml_node desc : rdf.children("rdf:Description")) {
        // Check attributes first (common in XMP)
        if (desc.attribute("pdfaid:part")) {
            part = desc.attribute("pdfaid:part").value();
        }
        if (desc.attribute("pdfaid:conformance")) {
            conformance = desc.attribute("pdfaid:conformance").value();
        }

        // Check child elements
        if (part.empty()) {
            pugi::xml_node partNode = desc.child("pdfaid:part");
            if (partNode) {
                part = partNode.child_value();
            }
        }
        if (conformance.empty()) {
            pugi::xml_node confNode = desc.child("pdfaid:conformance");
            if (confNode) {
                conformance = confNode.child_value();
            }
        }

        if (!part.empty() && !conformance.empty()) {
            break;
        }
    }

    return partsToPdfALevel(part, conformance);
}

std::vector<uint8_t> XmpMetadataWriter::buildXmpPacket(
    PdfALevel level, PdfUALevel uaLevel,
    const std::string& title, const std::string& author,
    const std::string& creator, const std::string& producer,
    const std::string& createDate, const std::string& modDate) {

    pugi::xml_document doc;

    // Create RDF structure
    pugi::xml_node xmpmeta = doc.append_child("x:xmpmeta");
    xmpmeta.append_attribute("xmlns:x") = "adobe:ns:meta/";

    pugi::xml_node rdf = xmpmeta.append_child("rdf:RDF");
    rdf.append_attribute("xmlns:rdf") = "http://www.w3.org/1999/02/22-rdf-syntax-ns#";

    pugi::xml_node desc = rdf.append_child("rdf:Description");
    desc.append_attribute("rdf:about") = "";
    desc.append_attribute("xmlns:dc") = "http://purl.org/dc/elements/1.1/";
    desc.append_attribute("xmlns:xmp") = "http://ns.adobe.com/xap/1.0/";
    desc.append_attribute("xmlns:pdf") = "http://ns.adobe.com/pdf/1.3/";

    // Add PDF/A identification if specified
    if (level != PdfALevel::None) {
        auto [part, conformance] = pdfALevelToParts(level);
        desc.append_attribute("xmlns:pdfaid") = "http://www.aiim.org/pdfa/ns/id/";
        desc.append_child("pdfaid:part").text().set(part.c_str());
        desc.append_child("pdfaid:conformance").text().set(conformance.c_str());
    }

    // Add PDF/UA identification if specified
    if (uaLevel != PdfUALevel::None) {
        std::string uaPart = pdfUALevelToPart(uaLevel);
        desc.append_attribute("xmlns:pdfuaid") = "http://www.aiim.org/pdfua/ns/id/";
        desc.append_child("pdfuaid:part").text().set(uaPart.c_str());
    }

    // Add Dublin Core metadata
    if (!title.empty()) {
        pugi::xml_node titleNode = desc.append_child("dc:title");
        pugi::xml_node alt = titleNode.append_child("rdf:Alt");
        pugi::xml_node li = alt.append_child("rdf:li");
        li.append_attribute("xml:lang") = "x-default";
        li.text().set(title.c_str());
    }

    if (!author.empty()) {
        pugi::xml_node creatorNode = desc.append_child("dc:creator");
        pugi::xml_node seq = creatorNode.append_child("rdf:Seq");
        seq.append_child("rdf:li").text().set(author.c_str());
    }

    // Add XMP metadata
    if (!createDate.empty()) {
        desc.append_child("xmp:CreateDate").text().set(createDate.c_str());
    }

    if (!modDate.empty()) {
        desc.append_child("xmp:ModifyDate").text().set(modDate.c_str());
    }

    if (!creator.empty()) {
        desc.append_child("xmp:CreatorTool").text().set(creator.c_str());
    }

    // Add PDF metadata
    if (!producer.empty()) {
        desc.append_child("pdf:Producer").text().set(producer.c_str());
    }

    // Serialize to string.
    //
    // format_no_declaration is REQUIRED, not cosmetic (meta-audit follow-up
    // F14, PDF-17). pugixml's save() synthesises an `<?xml version="1.0"?>`
    // declaration when the document carries none, which would land on line 2
    // of the packet — immediately after the `<?xpacket begin=?>` processing
    // instruction below. ISO 16684-1 §7.3.2 forbids an XML declaration inside
    // an XMP packet, and every conforming XMP parser rejects it:
    //
    //   SAXParseException; lineNumber: 2; columnNumber: 6;
    //   The processing instruction target matching "[xX][mM][lL]" is not allowed.
    //
    // veraPDF (the reference PDF/A validator, wired up in F14) fails the whole
    // document on that, so PDFE was stamping a `pdfaid` PDF/A claim into a
    // packet no conforming reader could parse — a FALSE conformance claim. The
    // defect was invisible while PDF/A was graded only by PDFE's own validator,
    // which reads the packet with the same pugixml that wrote it.
    std::ostringstream oss;
    doc.save(oss, "  ", pugi::format_default | pugi::format_no_declaration,
             pugi::encoding_utf8);
    std::string xmlContent = oss.str();

    // Build complete XMP packet with proper headers and padding
    std::ostringstream packet;

    // XMP packet header (with UTF-8 BOM marker)
    packet << "<?xpacket begin=\"\xEF\xBB\xBF\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>\n";

    // XML content
    packet << xmlContent;

    // Add padding (approximately 2KB) for in-place updates
    // PDF spec recommends padding to allow future metadata updates
    std::string currentContent = packet.str();
    const size_t targetSize = currentContent.size() + 2048;
    const size_t paddingNeeded = targetSize - currentContent.size() - 50; // Reserve space for closing tag

    packet << "\n";
    for (size_t i = 0; i < paddingNeeded; ++i) {
        packet << " ";
    }
    packet << "\n";

    // XMP packet end marker
    packet << "<?xpacket end=\"w\"?>";

    std::string packetStr = packet.str();
    return std::vector<uint8_t>(packetStr.begin(), packetStr.end());
}

} // namespace pdfe::compliance
