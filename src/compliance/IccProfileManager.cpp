#include "IccProfileManager.h"
#include "srgb_icc_profile.h"
#include "../core/MuPdfContext.h"
#include "../core/Document.h"

#include <cstring>
#include <vector>
#include <algorithm>

extern "C" {
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
}

namespace pdfe::compliance {

// ============================================================================
// Helper functions for ICC profile generation
// ============================================================================

namespace {

// Write big-endian uint32
void writeBE32(std::vector<uint8_t>& data, size_t offset, uint32_t value) {
    data[offset + 0] = static_cast<uint8_t>((value >> 24) & 0xFF);
    data[offset + 1] = static_cast<uint8_t>((value >> 16) & 0xFF);
    data[offset + 2] = static_cast<uint8_t>((value >> 8) & 0xFF);
    data[offset + 3] = static_cast<uint8_t>(value & 0xFF);
}

// Write big-endian uint16
void writeBE16(std::vector<uint8_t>& data, size_t offset, uint16_t value) {
    data[offset + 0] = static_cast<uint8_t>((value >> 8) & 0xFF);
    data[offset + 1] = static_cast<uint8_t>(value & 0xFF);
}

// Write s15Fixed16Number (fixed-point number)
void writeS15Fixed16(std::vector<uint8_t>& data, size_t offset, double value) {
    int32_t fixed = static_cast<int32_t>(value * 65536.0 + 0.5);
    writeBE32(data, offset, static_cast<uint32_t>(fixed));
}

// Write 4-byte signature
void writeSignature(std::vector<uint8_t>& data, size_t offset, const char* sig) {
    std::memcpy(&data[offset], sig, 4);
}

// Write ICC DateTime structure (12 bytes).
//
// GitHub #228. This used to stamp the WALL CLOCK, which made
// generateSrgbIccProfile() non-deterministic: two calls in the same run return
// different bytes whenever they straddle a second boundary. That is a poor
// property for a profile that is otherwise a fixed constant -- it defeats
// byte-comparison of output PDFs, content-addressed caching, and any
// reproducible-build claim, and it does so intermittently, which is the worst
// way to fail.
//
// ICC.1:2010 section 7.2.8 makes zero the sanctioned value: the field carries
// the profile creation date and time, and "if the date and time are not known,
// the field shall be set to zeros". For a profile synthesised identically on
// every call there IS no meaningful creation moment to record, so zero is not
// merely permitted here, it is the accurate answer.
//
// Kept as a function rather than inlined so the offset and 12-byte width stay
// documented at the call site.
void writeDateTime(std::vector<uint8_t>& data, size_t offset) {
    // All twelve bytes zero: year, month, day, hour, minute, second.
    std::memset(&data[offset], 0, 12);
}

// Write XYZ type tag data
std::vector<uint8_t> createXYZType(double x, double y, double z) {
    std::vector<uint8_t> data(20, 0);
    writeSignature(data, 0, "XYZ ");  // Type signature
    // 4 bytes reserved (already zeroed)
    writeS15Fixed16(data, 8, x);
    writeS15Fixed16(data, 12, y);
    writeS15Fixed16(data, 16, z);
    return data;
}

// Write text description type (simplified)
std::vector<uint8_t> createTextDescType(const char* text) {
    size_t textLen = std::strlen(text);
    size_t totalSize = 12 + textLen + 1;  // Type(4) + Reserved(4) + Count(4) + Text + Null

    std::vector<uint8_t> data(totalSize, 0);
    writeSignature(data, 0, "desc");  // Type signature
    // 4 bytes reserved (already zeroed)
    writeBE32(data, 8, static_cast<uint32_t>(textLen + 1));  // Count includes null terminator
    std::memcpy(&data[12], text, textLen);
    // Null terminator already present from zero-initialization

    return data;
}

// Write curve type (for gamma TRC)
std::vector<uint8_t> createCurveType(uint16_t gamma256) {
    std::vector<uint8_t> data(14, 0);  // 4 sig + 4 reserved + 4 count + 2 gamma
    writeSignature(data, 0, "curv");  // Type signature
    // 4 bytes reserved (already zeroed)
    writeBE32(data, 8, 1);  // Count = 1 (single gamma value)
    writeBE16(data, 12, gamma256);  // Gamma value (gamma * 256)
    return data;
}

// Write text type (for copyright)
std::vector<uint8_t> createTextType(const char* text) {
    size_t textLen = std::strlen(text);
    size_t totalSize = 8 + textLen;  // Type(4) + Reserved(4) + Text (no null terminator)

    std::vector<uint8_t> data(totalSize, 0);
    writeSignature(data, 0, "text");  // Type signature
    // 4 bytes reserved (already zeroed)
    std::memcpy(&data[8], text, textLen);

    return data;
}

} // anonymous namespace

// Generate minimal sRGB ICC v2 profile
std::vector<uint8_t> generateSrgbIccProfile() {
    // Tag data preparation
    auto descData = createTextDescType("sRGB IEC61966-2.1");
    auto wtptData = createXYZType(0.9504560, 1.0000000, 1.0890578);  // D50 white point
    auto rXYZData = createXYZType(0.4360747, 0.2225045, 0.0139322);  // Red primary
    auto gXYZData = createXYZType(0.3850649, 0.7168786, 0.0971045);  // Green primary
    auto bXYZData = createXYZType(0.1430804, 0.0606169, 0.7141733);  // Blue primary
    auto rTRCData = createCurveType(0x023A);  // Gamma 2.2 (approx 563/256)
    auto gTRCData = createCurveType(0x023A);
    auto bTRCData = createCurveType(0x023A);
    auto cprtData = createTextType("No copyright, use freely");

    // Calculate offsets (after header + tag table)
    const size_t headerSize = 128;
    const size_t tagCount = 9;
    const size_t tagTableSize = 4 + (tagCount * 12);  // Count + (sig+offset+size)*N

    size_t offset = headerSize + tagTableSize;

    struct TagEntry {
        const char* sig;
        const std::vector<uint8_t>* data;
        size_t offset;
    };

    std::vector<TagEntry> tags = {
        {"desc", &descData, offset},
        {"wtpt", &wtptData, offset += descData.size()},
        {"rXYZ", &rXYZData, offset += wtptData.size()},
        {"gXYZ", &gXYZData, offset += rXYZData.size()},
        {"bXYZ", &bXYZData, offset += gXYZData.size()},
        {"rTRC", &rTRCData, offset += bXYZData.size()},
        {"gTRC", &gTRCData, offset += rTRCData.size()},
        {"bTRC", &bTRCData, offset += gTRCData.size()},
        {"cprt", &cprtData, offset += bTRCData.size()}
    };

    const size_t profileSize = offset + cprtData.size();

    // Allocate profile data
    std::vector<uint8_t> profile(profileSize, 0);

    // ========================================================================
    // Header (128 bytes)
    // ========================================================================
    writeBE32(profile, 0, static_cast<uint32_t>(profileSize));  // Profile size
    writeBE32(profile, 4, 0);  // CMM type (none)
    writeBE32(profile, 8, 0x02100000);  // Version 2.1.0
    writeSignature(profile, 12, "mntr");  // Device class (monitor)
    writeSignature(profile, 16, "RGB ");  // Color space
    writeSignature(profile, 20, "XYZ ");  // PCS (XYZ)
    writeDateTime(profile, 24);  // Creation date/time
    writeSignature(profile, 36, "acsp");  // File signature
    writeSignature(profile, 40, "MSFT");  // Primary platform
    writeBE32(profile, 44, 0);  // Profile flags
    writeBE32(profile, 48, 0);  // Device manufacturer
    writeBE32(profile, 52, 0);  // Device model
    writeBE32(profile, 56, 0);  // Device attributes (low)
    writeBE32(profile, 60, 0);  // Device attributes (high)
    writeBE32(profile, 64, 0);  // Rendering intent (perceptual)
    writeBE32(profile, 68, 0);  // Reserved
    writeS15Fixed16(profile, 72, 0.9504560);  // PCS illuminant X (D50)
    writeS15Fixed16(profile, 76, 1.0000000);  // PCS illuminant Y (D50)
    writeS15Fixed16(profile, 80, 1.0890578);  // PCS illuminant Z (D50)
    writeBE32(profile, 84, 0);  // Creator signature
    // Bytes 88-127 reserved (already zeroed)

    // ========================================================================
    // Tag table
    // ========================================================================
    size_t tagTableOffset = headerSize;
    writeBE32(profile, tagTableOffset, static_cast<uint32_t>(tagCount));
    tagTableOffset += 4;

    for (const auto& tag : tags) {
        writeSignature(profile, tagTableOffset, tag.sig);
        writeBE32(profile, tagTableOffset + 4, static_cast<uint32_t>(tag.offset));
        writeBE32(profile, tagTableOffset + 8, static_cast<uint32_t>(tag.data->size()));
        tagTableOffset += 12;
    }

    // ========================================================================
    // Tag data
    // ========================================================================
    for (const auto& tag : tags) {
        std::memcpy(&profile[tag.offset], tag.data->data(), tag.data->size());
    }

    return profile;
}

// ============================================================================
// IccProfileManager Implementation
// ============================================================================

class IccProfileManager::Impl {
public:
    explicit Impl(std::shared_ptr<pdfe::core::MuPdfContext> context)
        : context_(std::move(context))
    {}

    std::shared_ptr<pdfe::core::MuPdfContext> context_;
};

IccProfileManager::IccProfileManager(std::shared_ptr<pdfe::core::MuPdfContext> context)
    : impl_(std::make_unique<Impl>(std::move(context)))
{}

IccProfileManager::~IccProfileManager() = default;

bool IccProfileManager::hasOutputIntent(pdfe::core::Document* doc) const {
    if (!doc) return false;
    bool hasIntent = false;

    impl_->context_->execute([&](fz_context* ctx) {
        pdf_document* pdfDoc = pdf_document_from_fz_document(ctx, doc->fzDocument());
        if (!pdfDoc) {
            return;  // Not a PDF document
        }

        pdf_obj* catalog = pdf_dict_get(ctx, pdf_trailer(ctx, pdfDoc), PDF_NAME(Root));
        if (!catalog) {
            return;
        }

        pdf_obj* intents = pdf_dict_get(ctx, catalog, PDF_NAME(OutputIntents));
        hasIntent = intents && pdf_is_array(ctx, intents) && pdf_array_len(ctx, intents) > 0;
    });

    return hasIntent;
}

void IccProfileManager::addSrgbOutputIntent(pdfe::core::Document* doc) {
    if (!doc) return;

    // Skip if already has output intent (idempotent)
    if (hasOutputIntent(doc)) return;

    // Generate sRGB ICC profile data
    auto profileData = generateSrgbIccProfile();

    impl_->context_->execute([&](fz_context* ctx) {
        pdf_document* pdfDoc = pdf_document_from_fz_document(ctx, doc->fzDocument());
        if (!pdfDoc) {
            return;  // Not a PDF document
        }

        // Create ICC stream
        fz_buffer* iccBuf = fz_new_buffer_from_copied_data(
            ctx,
            profileData.data(),
            profileData.size()
        );

        pdf_obj* iccStream = pdf_add_stream(ctx, pdfDoc, iccBuf, nullptr, 0);
        fz_drop_buffer(ctx, iccBuf);

        // Set N=3 for RGB color space
        pdf_dict_put_int(ctx, iccStream, PDF_NAME(N), 3);

        // Create OutputIntent dictionary
        pdf_obj* intent = pdf_new_dict(ctx, pdfDoc, 6);
        pdf_dict_put(ctx, intent, PDF_NAME(Type), PDF_NAME(OutputIntent));
        pdf_dict_put_name(ctx, intent, PDF_NAME(S), "GTS_PDFA1");
        pdf_dict_put_text_string(ctx, intent, PDF_NAME(OutputConditionIdentifier), "sRGB IEC61966-2.1");
        pdf_obj* registryName = pdf_new_text_string(ctx, "http://www.color.org");
        pdf_dict_puts(ctx, intent, "RegistryName", registryName);
        pdf_drop_obj(ctx, registryName);
        pdf_dict_puts(ctx, intent, "DestOutputProfile", iccStream);
        pdf_drop_obj(ctx, iccStream);

        // Add to catalog's OutputIntents array
        pdf_obj* catalog = pdf_dict_get(ctx, pdf_trailer(ctx, pdfDoc), PDF_NAME(Root));
        pdf_obj* outputIntents = pdf_dict_get(ctx, catalog, PDF_NAME(OutputIntents));

        if (!outputIntents) {
            outputIntents = pdf_new_array(ctx, pdfDoc, 1);
            pdf_dict_put(ctx, catalog, PDF_NAME(OutputIntents), outputIntents);
            pdf_drop_obj(ctx, outputIntents);
            // Re-fetch from catalog (catalog holds the reference now)
            outputIntents = pdf_dict_get(ctx, catalog, PDF_NAME(OutputIntents));
        }

        pdf_array_push(ctx, outputIntents, intent);
        pdf_drop_obj(ctx, intent);
    });
}

} // namespace pdfe::compliance
