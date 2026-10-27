#pragma once

#include "ComplianceTypes.h"
#include <memory>
#include <string>
#include <vector>
#include <cstdint>

namespace pdfe::core {
class MuPdfContext;
class Document;
}

namespace pdfe::compliance {

class XmpMetadataWriter {
public:
    explicit XmpMetadataWriter(std::shared_ptr<pdfe::core::MuPdfContext> context);
    ~XmpMetadataWriter();
    XmpMetadataWriter(const XmpMetadataWriter&) = delete;
    XmpMetadataWriter& operator=(const XmpMetadataWriter&) = delete;

    void writePdfAIdentification(pdfe::core::Document* doc, PdfALevel level);
    void writePdfUAIdentification(pdfe::core::Document* doc, PdfUALevel level);

    // Refresh an EXISTING XMP /Metadata packet from the document's current /Info
    // dictionary so the two metadata stores stay consistent after an Info edit.
    // No-op (returns false) when the document has no XMP packet — this never
    // fabricates a packet. Preserves the packet's current PDF/A conformance level
    // (detected via detectPdfALevel) so a plain Info edit does not downgrade it.
    // Syncs dc:title, dc:creator (author), xmp:CreatorTool (creator),
    // pdf:Producer, and xmp:CreateDate/xmp:ModifyDate.
    bool syncFromInfo(pdfe::core::Document* doc);

    std::string readXmpPacket(pdfe::core::Document* doc);
    PdfALevel detectPdfALevel(pdfe::core::Document* doc);

    // Meta-audit F13 (SEC-40 / SEC-41): explicit, fail-closed acceptance check
    // applied to an attacker-controlled /Metadata XMP packet BEFORE pugixml
    // sees it. ISO 16684-1 §7.1 forbids a DOCTYPE inside an XMP packet, so a
    // packet carrying a DTD, an entity declaration or an external identifier
    // is malicious-or-broken either way and is REFUSED rather than parsed.
    //
    // Returns true when the packet is acceptable. On refusal, `reason` (when
    // non-null) receives a human-readable explanation. This is the observable
    // refusal that tests/test_xml_entity_safety.cpp asserts on: without it the
    // "pugixml does not expand external entities" property would be an
    // untested assumption about a third-party default.
    static bool isXmpPacketAcceptable(const std::string& xmpPacket,
                                      std::string* reason = nullptr);
    std::vector<uint8_t> buildXmpPacket(
        PdfALevel level, PdfUALevel uaLevel,
        const std::string& title, const std::string& author,
        const std::string& creator, const std::string& producer,
        const std::string& createDate, const std::string& modDate
    );

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pdfe::compliance
