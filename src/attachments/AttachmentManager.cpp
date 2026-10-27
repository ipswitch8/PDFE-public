#include <set>
#include "AttachmentManager.h"
#include "../core/MuPdfContext.h"
#include "../core/Document.h"
#include "../core/TempFileSafety.h"

#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

#include <algorithm>
#include <fstream>
#include <stdexcept>

namespace pdfe::attachments {

namespace {

// Walk a Name tree (either /Kids recursive or /Names leaf) and invoke
// visitor(name, filespecObj) for each entry. Returns false if the visitor
// requests stop (returns false).
using Visitor = std::function<bool(const std::string&, pdf_obj*)>;

/// Depth cap for the /Kids recursion. A real name tree is a balanced B-tree
/// over the document's embedded files; even a pathological legitimate document
/// is a handful of levels. 64 is far above anything real and far below
/// anything that threatens the stack.
constexpr int kMaxNameTreeDepth = 64;

/// Recursion guard for walkNameTree.
///
/// The /Kids traversal followed object references with no depth cap and no
/// visited set. A name tree whose /Kids points back at an ancestor — trivial
/// to author, since PDF objects are referenced by number — recursed until the
/// stack ran out. That is a crash on OPENING a document's attachment list, i.e.
/// reachable from anything that lists attachments, and it is a stack overflow
/// rather than a caught exception, so there is no fz_catch that helps.
///
/// Indirect object NUMBERS are what is tracked, not pointers: MuPDF may hand
/// back distinct pdf_obj* for the same underlying object, so a pointer set
/// would miss the cycle it exists to catch.
struct NameTreeGuard {
    std::set<int> visited;
    bool overflowed = false;   ///< depth cap or cycle hit — reported, not silent
};

bool walkNameTree(fz_context* ctx, pdf_obj* tree, const Visitor& v,
                  NameTreeGuard& guard, int depth)
{
    if (!tree) return true;

    if (depth > kMaxNameTreeDepth) {
        guard.overflowed = true;
        return true;   // stop descending; keep whatever was found above
    }

    // Cycle detection on the indirect object number. Direct (inline) objects
    // have number 0 and cannot form a reference cycle, so they are not tracked.
    const int num = pdf_to_num(ctx, tree);
    if (num > 0) {
        if (!guard.visited.insert(num).second) {
            guard.overflowed = true;
            return true;   // already walked this node — a cycle
        }
    }

    pdf_obj* names = pdf_dict_get(ctx, tree, PDF_NAME(Names));
    if (names && pdf_is_array(ctx, names)) {
        const int n = pdf_array_len(ctx, names);
        for (int i = 0; i + 1 < n; i += 2) {
            pdf_obj* keyObj = pdf_array_get(ctx, names, i);
            pdf_obj* valObj = pdf_array_get(ctx, names, i + 1);
            if (!keyObj || !valObj) continue;
            const char* key = pdf_to_text_string(ctx, keyObj);
            if (!v(key ? key : "", valObj)) return false;
        }
    }
    pdf_obj* kids = pdf_dict_get(ctx, tree, PDF_NAME(Kids));
    if (kids && pdf_is_array(ctx, kids)) {
        const int n = pdf_array_len(ctx, kids);
        for (int i = 0; i < n; ++i) {
            if (!walkNameTree(ctx, pdf_array_get(ctx, kids, i), v, guard, depth + 1))
                return false;
        }
    }
    return true;
}

/// Convenience overload for callers that do not inspect the guard.
bool walkNameTree(fz_context* ctx, pdf_obj* tree, const Visitor& v)
{
    NameTreeGuard guard;
    return walkNameTree(ctx, tree, v, guard, 0);
}

/// Write `bytes` into `outDir` under `baseName`, or "notes (1).txt" and so on
/// if that name is taken. Returns the path actually written, or an empty path.
///
/// Rename-with-suffix rather than an error, because the alternative for a user
/// extracting a dozen attachments is a failure they have to resolve by hand.
///
/// THERE IS NO `exists()` CHECK HERE, DELIBERATELY. The first version of this
/// function picked a free name with `std::filesystem::exists()` and handed it
/// back to an ordinary truncating `ofstream`. The `security-audit` gate failed
/// the phase for it, correctly: between the check and the open, anything with
/// write access to the output directory can place a junction or a hardlink at
/// that name, and the open adopts it — writing document-controlled bytes
/// through the attacker's choice of target under PDFE's token.
///
/// **Phase-70 was gate-failed for this same check-then-act shape in
/// `src/search`, and I wrote this one two phases later.** Fixing one instance
/// of a pattern is not fixing the pattern. So the existence test is now the
/// create itself: `writeNewFileExclusive` uses `CREATE_NEW`, which the kernel
/// performs atomically, and a collision comes back as `errc::file_exists` for
/// the loop below to treat as "try the next name" — the one error that is a
/// collision rather than a refusal.
///
/// The loop is bounded: an attacker who can pre-create thousands of names
/// should not get an unbounded loop out of it.
std::filesystem::path writeUniqueTarget(const std::filesystem::path& outDir,
                                        const std::string& baseName,
                                        const void* data, std::size_t len)
{
    std::error_code ec;
    std::filesystem::path candidate = outDir / baseName;
    if (pdfe::core::writeNewFileExclusive(candidate, data, len, ec))
        return candidate;
    if (ec != std::errc::file_exists) return {};   // a refusal, not a collision

    const std::filesystem::path stem = std::filesystem::path(baseName).stem();
    const std::filesystem::path ext  = std::filesystem::path(baseName).extension();
    for (int i = 1; i <= 9999; ++i) {
        candidate = outDir /
            (stem.string() + " (" + std::to_string(i) + ")" + ext.string());
        if (pdfe::core::writeNewFileExclusive(candidate, data, len, ec))
            return candidate;
        if (ec != std::errc::file_exists) return {};
    }
    return {};   // caller treats an empty path as "could not place the file"
}

/// Attach a Mark-of-the-Web to an extracted file.
///
/// Windows treats a file carrying Zone.Identifier=3 as internet-sourced:
/// SmartScreen inspects it, Office opens it in Protected View, and the shell
/// warns before executing it. An attachment lifted out of a PDF has exactly
/// the provenance that mark describes, and without it the extracted file is
/// indistinguishable from one the user authored locally.
///
/// Best-effort by design: the ADS write fails on filesystems without streams
/// (FAT32, most network shares), and failing to mark a file is not a reason to
/// refuse to extract it. The extraction itself has already succeeded.
void markOfTheWeb([[maybe_unused]] const std::filesystem::path& file)
{
#if defined(_WIN32)
    const std::wstring ads = file.wstring() + L":Zone.Identifier";
    std::ofstream zone(ads, std::ios::binary | std::ios::trunc);
    if (!zone) return;
    zone << "[ZoneTransfer]\r\nZoneId=3\r\n";
#endif
}

pdf_obj* getOrCreateEmbeddedFilesTree(fz_context* ctx, pdf_document* doc)
{
    pdf_obj* root = pdf_dict_get(ctx, pdf_trailer(ctx, doc), PDF_NAME(Root));
    pdf_obj* names = pdf_dict_get(ctx, root, PDF_NAME(Names));
    if (!names) {
        names = pdf_new_dict(ctx, doc, 2);
        pdf_dict_put(ctx, root, PDF_NAME(Names), names);
    }
    pdf_obj* ef = pdf_dict_get(ctx, names, PDF_NAME(EmbeddedFiles));
    if (!ef) {
        ef = pdf_new_dict(ctx, doc, 2);
        pdf_dict_put(ctx, names, PDF_NAME(EmbeddedFiles), ef);
        pdf_obj* arr = pdf_new_array(ctx, doc, 2);
        pdf_dict_put(ctx, ef, PDF_NAME(Names), arr);
    }
    return ef;
}

pdf_obj* findEmbeddedFilesTree(fz_context* ctx, pdf_document* doc)
{
    pdf_obj* root = pdf_dict_get(ctx, pdf_trailer(ctx, doc), PDF_NAME(Root));
    if (!root) return nullptr;
    return pdf_dict_getp(ctx, root, "Names/EmbeddedFiles");
}

fz_buffer* readFileToBuffer(fz_context* ctx, const std::filesystem::path& file)
{
    std::ifstream in(file, std::ios::binary);
    if (!in) return nullptr;
    in.seekg(0, std::ios::end);
    const auto size = in.tellg();
    if (size <= 0) return nullptr;
    in.seekg(0, std::ios::beg);

    fz_buffer* buf = fz_new_buffer(ctx, static_cast<size_t>(size));
    std::vector<char> tmp(static_cast<size_t>(size));
    if (!in.read(tmp.data(), size)) {
        fz_drop_buffer(ctx, buf);
        return nullptr;
    }
    fz_append_data(ctx, buf,
        reinterpret_cast<const unsigned char*>(tmp.data()),
        static_cast<size_t>(size));
    return buf;
}

} // namespace

AttachmentManager::AttachmentManager(std::shared_ptr<pdfe::core::MuPdfContext> ctx)
    : ctx_(std::move(ctx))
{
    if (!ctx_) throw std::invalid_argument("AttachmentManager: null context");
}

AttachmentManager::~AttachmentManager() = default;

namespace {

/// Win32 reserved device names. Opening any of these — with OR without an
/// extension, in ANY directory — talks to a device, not a file. "CON.txt" is
/// still CON.
const char* const kWin32DeviceNames[] = {
    "CON", "PRN", "AUX", "NUL",
    "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
    "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9",
};

std::string upperAscii(const std::string& s)
{
    std::string r = s;
    for (char& c : r) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    return r;
}

/// The stem is what Windows matches a device name against: everything before
/// the FIRST dot, with trailing spaces ignored.
bool isWin32DeviceName(const std::string& name)
{
    std::string stem = name.substr(0, name.find('.'));
    while (!stem.empty() && (stem.back() == ' ' || stem.back() == '\t')) stem.pop_back();
    const std::string up = upperAscii(stem);
    for (const char* dev : kWin32DeviceNames) {
        if (up == dev) return true;
    }
    return false;
}

/// True if the 3-byte UTF-8 sequence starting at `p` is a bidirectional
/// override or isolate. These reorder how a name RENDERS without changing the
/// bytes, so "doc‮gnp.exe" is displayed as "docexe.png" — the user sees a
/// PNG and runs an executable.
bool isBidiOverrideUtf8(const unsigned char* p, size_t remaining)
{
    if (remaining < 3) return false;
    // U+202A..U+202E  -> E2 80 AA..AE
    if (p[0] == 0xE2 && p[1] == 0x80 && p[2] >= 0xAA && p[2] <= 0xAE) return true;
    // U+2066..U+2069  -> E2 81 A6..A9
    if (p[0] == 0xE2 && p[1] == 0x81 && p[2] >= 0xA6 && p[2] <= 0xA9) return true;
    // U+200E/U+200F LRM/RLM -> E2 80 8E/8F
    if (p[0] == 0xE2 && p[1] == 0x80 && (p[2] == 0x8E || p[2] == 0x8F)) return true;
    return false;
}

}  // namespace

std::string AttachmentManager::sanitizeFilename(const std::string& raw)
{
    std::string s = raw;

    // Strip any path separators — keep only the basename.
    const auto lastSep = s.find_last_of("/\\");
    if (lastSep != std::string::npos) s = s.substr(lastSep + 1);

    // Reject empty, ".", "..", and anything that still contains separators
    // or colons (drive letter markers).
    if (s.empty() || s == "." || s == "..") return "attachment";

    // Replace residual suspicious bytes, and DROP bidi overrides entirely.
    //
    // Control characters 0x01-0x1F are not merely cosmetic: they let a name
    // carry a newline into a log or a terminal escape into a console. 0x00 was
    // already handled; the rest were not.
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (isBidiOverrideUtf8(reinterpret_cast<const unsigned char*>(s.data()) + i,
                               s.size() - i)) {
            i += 3;          // dropped, not substituted — see the note below
            continue;
        }
        if (c == '/' || c == '\\' || c == ':' || c < 0x20 || c == 0x7F) {
            out.push_back('_');
        } else {
            out.push_back(static_cast<char>(c));
        }
        ++i;
    }

    // Strip leading dots (prevents hidden files / "..foo")
    while (!out.empty() && out.front() == '.') out.erase(out.begin());

    // STRIP TRAILING DOTS AND SPACES — and this is the one that matters most,
    // because it is not about what the name looks like, it is about the name
    // we return being the name on disk.
    //
    // Win32 silently strips trailing dots and spaces when it creates a file.
    // "evil.exe. " therefore lands on disk as "evil.exe" while every caller
    // holding the sanitized string believes it wrote "evil.exe. ". That desync
    // is exploitable: a caller that later checks, quarantines or deletes "the
    // file we extracted" operates on a name that does not exist, and the real
    // file survives under a name nobody is tracking.
    while (!out.empty() && (out.back() == '.' || out.back() == ' ')) out.pop_back();

    if (out.empty()) out = "attachment";

    // Win32 device names, checked AFTER the strips above so "CON. " and
    // "con.txt" are both caught. Prefixing rather than replacing keeps the
    // name recognisable to a human looking at an extraction directory.
    if (isWin32DeviceName(out)) out = "_" + out;

    return out;
}

std::vector<AttachmentEntry> AttachmentManager::list(pdfe::core::Document* doc)
{
    std::vector<AttachmentEntry> out;
    lastTraversalTruncated_ = false;
    if (!doc) return out;

    // Declared in the caller's frame: execute() runs the lambda inside an
    // fz_try, and a longjmp out of it does not unwind C++ objects declared
    // inside the guarded block.
    NameTreeGuard guard;

    ctx_->execute([&](fz_context* ctx) {
        pdf_document* pdoc = pdf_document_from_fz_document(ctx, doc->fzDocument());
        if (!pdoc) return;
        pdf_obj* ef = findEmbeddedFilesTree(ctx, pdoc);
        if (!ef) return;
        walkNameTree(ctx, ef, [&](const std::string& name, pdf_obj* fs) {
            if (!pdf_is_embedded_file(ctx, fs)) return true;
            pdf_filespec_params params{};
            pdf_get_filespec_params(ctx, fs, &params);
            AttachmentEntry e;
            e.name     = name;
            e.filename = params.filename ? params.filename : name;
            e.mimeType = params.mimetype ? params.mimetype : "";
            e.size     = params.size;
            e.created  = params.created;
            e.modified = params.modified;
            out.push_back(std::move(e));
            return true;
        }, guard, 0);
    });
    lastTraversalTruncated_ = guard.overflowed;
    return out;
}

bool AttachmentManager::add(pdfe::core::Document* doc,
                            const std::filesystem::path& file,
                            const std::string& name)
{
    if (!doc) return false;
    if (!std::filesystem::exists(file)) return false;

    bool ok = false;
    ctx_->execute([&](fz_context* ctx) {
        pdf_document* pdoc = pdf_document_from_fz_document(ctx, doc->fzDocument());
        if (!pdoc) return;

        fz_buffer* contents = readFileToBuffer(ctx, file);
        if (!contents) return;

        const std::string sanitized = sanitizeFilename(
            name.empty() ? file.filename().string() : name);

        pdf_obj* filespec = nullptr;
        fz_try(ctx) {
            filespec = pdf_add_embedded_file(ctx, pdoc,
                sanitized.c_str(),
                nullptr,        // mimetype — unknown
                contents,
                -1, -1,         // created/modified — unknown
                0);             // no checksum
        } fz_always(ctx) {
            fz_drop_buffer(ctx, contents);
        } fz_catch(ctx) {
            fz_rethrow(ctx);
        }

        if (!filespec) return;

        pdf_obj* ef = getOrCreateEmbeddedFilesTree(ctx, pdoc);
        pdf_obj* names = pdf_dict_get(ctx, ef, PDF_NAME(Names));
        if (!names || !pdf_is_array(ctx, names)) {
            names = pdf_new_array(ctx, pdoc, 2);
            pdf_dict_put(ctx, ef, PDF_NAME(Names), names);
        }
        pdf_array_push(ctx, names, pdf_new_text_string(ctx, sanitized.c_str()));
        pdf_array_push(ctx, names, filespec);
        ok = true;
    });
    return ok;
}

std::filesystem::path AttachmentManager::extract(pdfe::core::Document* doc,
                                                 const std::string& name,
                                                 const std::filesystem::path& outDir)
{
    lastTraversalTruncated_ = false;
    lastExtractRefusedOversize_ = false;
    if (!doc) return {};
    std::filesystem::create_directories(outDir);

    std::filesystem::path written;
    NameTreeGuard guard;

    // Constructed HERE, in the caller's frame, and captured by reference: the
    // lambda bodies below run inside MuPdfContext::execute's fz_try, which is
    // setjmp/longjmp, and a longjmp does not run destructors for objects
    // declared inside the guarded block.
    std::string baseName;
    std::filesystem::path outPath;
    bool oversize = false;

    ctx_->execute([&](fz_context* ctx) {
        pdf_document* pdoc = pdf_document_from_fz_document(ctx, doc->fzDocument());
        if (!pdoc) return;
        pdf_obj* ef = findEmbeddedFilesTree(ctx, pdoc);
        if (!ef) return;

        walkNameTree(ctx, ef, [&](const std::string& key, pdf_obj* fs) {
            if (key != name) return true;   // keep looking
            if (!pdf_is_embedded_file(ctx, fs)) return false;

            pdf_filespec_params params{};
            pdf_get_filespec_params(ctx, fs, &params);
            baseName = sanitizeFilename(params.filename ? params.filename : key);
            outPath = outDir / baseName;

            fz_buffer* contents = pdf_load_embedded_file_contents(ctx, fs);
            if (!contents) return false;

            // contents was dropped only on the fall-through path, so a throw
            // out of fz_buffer_storage leaked the whole decompressed
            // attachment. fz_var for the same reason as the sibling site in
            // AnnotationManager::extractFileAttachmentPayload: not strictly
            // required today, kept so a later edit cannot make it required
            // without anyone noticing.
            fz_var(contents);
            fz_try(ctx) {
                unsigned char* data = nullptr;
                size_t len = fz_buffer_storage(ctx, contents, &data);

                // SIZE CAP. pdf_load_embedded_file_contents has already
                // decompressed by the time we get here, so this does not stop
                // the decompression itself — it stops the result being spilled
                // to disk. An attachment stream a few KB on the wire can expand
                // to gigabytes; writing that out fills the user's volume from a
                // document they merely opened.
                if (len > maxExtractBytes_) {
                    oversize = true;
                } else {
                    // NO SILENT OVERWRITE, AND NO RACE. The old code opened
                    // with the default truncating mode, so extracting an
                    // attachment named "notes.txt" destroyed any "notes.txt"
                    // already in the output directory — a document choosing
                    // which of the user's files to overwrite. Picking a free
                    // name with exists() first fixed the overwrite and left a
                    // check-then-act window; the create is now the existence
                    // test. See writeUniqueTarget.
                    outPath = writeUniqueTarget(outDir, baseName, data, len);

                    // The name we RETURN must be the name on disk. Win32 strips
                    // trailing dots/spaces at create time, so a sanitizer that
                    // let those through would desync the two. sanitizeFilename
                    // now strips them; this asserts the outcome rather than
                    // trusting it — and writeUniqueTarget returns only a path
                    // it actually created, so an empty result is unambiguous.
                    if (!outPath.empty() && std::filesystem::exists(outPath)) {
                        markOfTheWeb(outPath);
                        written = outPath;
                    }
                }
            }
            fz_always(ctx) {
                fz_drop_buffer(ctx, contents);
            }
            fz_catch(ctx) {
                fz_rethrow(ctx);
            }
            return false;  // stop
        }, guard, 0);
    });
    lastTraversalTruncated_ = guard.overflowed;
    lastExtractRefusedOversize_ = oversize;
    return written;
}

bool AttachmentManager::remove(pdfe::core::Document* doc, const std::string& name)
{
    if (!doc) return false;
    // Removing an embedded file orphans its (potentially sensitive) stream;
    // force a full-rewrite save so it is scrubbed (pipeline b2e47d91, Tier 1).
    doc->markRequiresFullRewrite();
    bool removed = false;
    ctx_->execute([&](fz_context* ctx) {
        pdf_document* pdoc = pdf_document_from_fz_document(ctx, doc->fzDocument());
        if (!pdoc) return;
        pdf_obj* ef = findEmbeddedFilesTree(ctx, pdoc);
        if (!ef) return;

        // Top-level /Names array only — name trees in practice for our added
        // files live here; we don't rewrite /Kids sub-trees.
        pdf_obj* names = pdf_dict_get(ctx, ef, PDF_NAME(Names));
        if (!names || !pdf_is_array(ctx, names)) return;

        const int n = pdf_array_len(ctx, names);
        for (int i = 0; i + 1 < n; i += 2) {
            pdf_obj* keyObj = pdf_array_get(ctx, names, i);
            const char* k = pdf_to_text_string(ctx, keyObj);
            if (k && name == k) {
                // Delete key+value pair.
                pdf_array_delete(ctx, names, i + 1);
                pdf_array_delete(ctx, names, i);
                removed = true;
                break;
            }
        }
    });
    return removed;
}

bool AttachmentManager::isPortfolio(pdfe::core::Document* doc)
{
    if (!doc) return false;
    bool result = false;
    ctx_->execute([&](fz_context* ctx) {
        pdf_document* pdoc = pdf_document_from_fz_document(ctx, doc->fzDocument());
        if (!pdoc) return;
        pdf_obj* trailer = pdf_trailer(ctx, pdoc);
        pdf_obj* root = pdf_dict_get(ctx, trailer, PDF_NAME(Root));
        if (!root) return;
        const pdf_obj* collection = pdf_dict_get(ctx, root, PDF_NAME(Collection));
        result = (collection != nullptr);
    });
    return result;
}

std::string AttachmentManager::portfolioView(pdfe::core::Document* doc)
{
    std::string view = "D"; // PDF spec default
    if (!doc) return view;
    ctx_->execute([&](fz_context* ctx) {
        pdf_document* pdoc = pdf_document_from_fz_document(ctx, doc->fzDocument());
        if (!pdoc) return;
        pdf_obj* trailer = pdf_trailer(ctx, pdoc);
        pdf_obj* root = pdf_dict_get(ctx, trailer, PDF_NAME(Root));
        if (!root) return;
        pdf_obj* collection = pdf_dict_get(ctx, root, PDF_NAME(Collection));
        if (!collection) return;
        pdf_obj* v = pdf_dict_get(ctx, collection, PDF_NAME(View));
        if (!v) return;
        const char* n = pdf_to_name(ctx, v);
        if (!n || !*n) return;
        if (std::string(n) == "D" || std::string(n) == "T" ||
            std::string(n) == "H") {
            view = n;
        }
    });
    return view;
}

} // namespace pdfe::attachments
