#pragma once
// Minimal ZIP writer — Store method only, no compression. Sufficient for
// DOCX (Office accepts stored entries) and keeps the code footprint small.
//
// Caller constructs a ZipWriter with an output path, adds files by name
// + byte payload via addFile(), and calls close() to flush the central
// directory. The archive is NOT valid until close() succeeds.

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace pdfe::exports::minizip {

class ZipWriter {
public:
    explicit ZipWriter(const std::filesystem::path& outputPath);
    ~ZipWriter();

    ZipWriter(const ZipWriter&) = delete;
    ZipWriter& operator=(const ZipWriter&) = delete;

    bool valid() const { return out_.is_open() && !failed_; }

    /// Add `name` (forward-slash path inside the archive) with `data`
    /// bytes. Returns false on I/O failure (subsequent calls are no-ops).
    bool addFile(const std::string& name, const std::string& data);

    /// Write the central directory and end-of-central-directory record,
    /// then close the stream. Idempotent.
    bool close();

private:
    struct Entry {
        std::string name;
        std::uint32_t crc32     = 0;
        std::uint32_t size      = 0;
        std::uint32_t headerOffset = 0;
    };

    static std::uint32_t crc32(const unsigned char* data, std::size_t len);

    std::ofstream out_;
    std::vector<Entry> entries_;
    bool failed_ = false;
    bool closed_ = false;
};

} // namespace pdfe::exports::minizip
