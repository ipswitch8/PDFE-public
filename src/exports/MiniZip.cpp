#include "MiniZip.h"
#include <cstring>

namespace pdfe::exports::minizip {

namespace {

// CRC-32 (polynomial 0xEDB88320). Self-contained to avoid pulling in
// zlib as a direct dependency of pdfe_exports.
std::uint32_t crc32Table(std::uint8_t b)
{
    std::uint32_t c = b;
    for (int k = 0; k < 8; ++k) {
        c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
    }
    return c;
}

void writeLE16(std::ostream& os, std::uint16_t v)
{
    const char bytes[2] = { static_cast<char>(v & 0xFF),
                            static_cast<char>((v >> 8) & 0xFF) };
    os.write(bytes, 2);
}

void writeLE32(std::ostream& os, std::uint32_t v)
{
    const char bytes[4] = {
        static_cast<char>(v & 0xFF),
        static_cast<char>((v >> 8) & 0xFF),
        static_cast<char>((v >> 16) & 0xFF),
        static_cast<char>((v >> 24) & 0xFF)
    };
    os.write(bytes, 4);
}

} // namespace

ZipWriter::ZipWriter(const std::filesystem::path& outputPath)
    : out_(outputPath, std::ios::binary | std::ios::trunc)
{
}

ZipWriter::~ZipWriter()
{
    close();
}

std::uint32_t ZipWriter::crc32(const unsigned char* data, std::size_t len)
{
    static std::uint32_t table[256];
    static bool initialized = false;
    if (!initialized) {
        for (int i = 0; i < 256; ++i) {
            table[i] = crc32Table(static_cast<std::uint8_t>(i));
        }
        initialized = true;
    }
    std::uint32_t c = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < len; ++i) {
        c = table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

bool ZipWriter::addFile(const std::string& name, const std::string& data)
{
    if (!valid() || closed_) return false;

    Entry entry;
    entry.name = name;
    entry.size = static_cast<std::uint32_t>(data.size());
    entry.crc32 = crc32(reinterpret_cast<const unsigned char*>(data.data()),
                        data.size());
    entry.headerOffset = static_cast<std::uint32_t>(out_.tellp());

    // Local file header (signature 0x04034b50)
    writeLE32(out_, 0x04034b50);
    writeLE16(out_, 20);          // version needed
    writeLE16(out_, 0);           // general purpose flags
    writeLE16(out_, 0);           // compression method: 0 = stored
    writeLE16(out_, 0);           // mod time
    writeLE16(out_, 0x21);        // mod date: 1980-01-01
    writeLE32(out_, entry.crc32);
    writeLE32(out_, entry.size);  // compressed size
    writeLE32(out_, entry.size);  // uncompressed size
    writeLE16(out_, static_cast<std::uint16_t>(name.size()));
    writeLE16(out_, 0);           // extra field length
    out_.write(name.data(), static_cast<std::streamsize>(name.size()));
    out_.write(data.data(),  static_cast<std::streamsize>(data.size()));

    if (!out_) { failed_ = true; return false; }

    entries_.push_back(std::move(entry));
    return true;
}

bool ZipWriter::close()
{
    if (closed_) return !failed_;
    closed_ = true;
    if (!out_.is_open()) return false;
    if (failed_) { out_.close(); return false; }

    const std::uint32_t cdStart = static_cast<std::uint32_t>(out_.tellp());
    for (const Entry& e : entries_) {
        // Central directory header (signature 0x02014b50)
        writeLE32(out_, 0x02014b50);
        writeLE16(out_, 20);     // version made by
        writeLE16(out_, 20);     // version needed
        writeLE16(out_, 0);      // flags
        writeLE16(out_, 0);      // method
        writeLE16(out_, 0);      // mod time
        writeLE16(out_, 0x21);   // mod date
        writeLE32(out_, e.crc32);
        writeLE32(out_, e.size);
        writeLE32(out_, e.size);
        writeLE16(out_, static_cast<std::uint16_t>(e.name.size()));
        writeLE16(out_, 0);      // extra len
        writeLE16(out_, 0);      // comment len
        writeLE16(out_, 0);      // disk number
        writeLE16(out_, 0);      // internal attrs
        writeLE32(out_, 0);      // external attrs
        writeLE32(out_, e.headerOffset);
        out_.write(e.name.data(), static_cast<std::streamsize>(e.name.size()));
    }
    const std::uint32_t cdEnd = static_cast<std::uint32_t>(out_.tellp());

    // End of central directory (signature 0x06054b50)
    writeLE32(out_, 0x06054b50);
    writeLE16(out_, 0); // disk number
    writeLE16(out_, 0); // disk where CD starts
    writeLE16(out_, static_cast<std::uint16_t>(entries_.size())); // entries on this disk
    writeLE16(out_, static_cast<std::uint16_t>(entries_.size())); // total entries
    writeLE32(out_, cdEnd - cdStart); // CD size
    writeLE32(out_, cdStart);         // CD offset
    writeLE16(out_, 0);               // comment length

    const bool ok = static_cast<bool>(out_);
    out_.close();
    if (!ok) failed_ = true;
    return !failed_;
}

} // namespace pdfe::exports::minizip
