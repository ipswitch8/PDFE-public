#include "RedactionScrubber.h"
#include "core/StextOptions.h"

#include <cctype>
#include <cstring>
#include <set>

namespace pdfe::redaction {

namespace {

bool containsAny(const std::string& hay, const std::vector<std::string>& needles)
{
    for (const auto& n : needles) {
        if (!n.empty() && hay.find(n) != std::string::npos) {
            return true;
        }
    }
    return false;
}

/// UTF-8 text of a PDF string / name object; empty for anything else.
void appendObjText(fz_context* ctx, pdf_obj* o, std::string& out)
{
    if (!o) return;
    if (pdf_is_string(ctx, o)) {
        const char* s = pdf_to_text_string(ctx, o);
        if (s) out.append(s);
    } else if (pdf_is_name(ctx, o)) {
        const char* s = pdf_to_name(ctx, o);
        if (s) out.append(s);
    }
}

/// DECOMPRESSED bytes of a stream object. Never throws.
void loadStreamText(fz_context* ctx, pdf_obj* stm, std::string& out)
{
    out.clear();
    if (!stm) return;
    fz_buffer* b = nullptr;
    fz_var(b);
    fz_try(ctx) {
        if (pdf_is_stream(ctx, stm)) {
            b = pdf_load_stream(ctx, stm);
            unsigned char* d = nullptr;
            size_t n = fz_buffer_storage(ctx, b, &d);
            if (d && n) out.assign(reinterpret_cast<const char*>(d), n);
        }
    }
    fz_always(ctx) {
        fz_drop_buffer(ctx, b);
    }
    fz_catch(ctx) {
        out.clear();
    }
}

/// Replace a stream's payload with `data`, stored UNCOMPRESSED (the old
/// /Filter no longer describes the bytes). Never throws.
void replaceStream(fz_context* ctx, pdf_document* doc, pdf_obj* stm, const std::string& data)
{
    fz_buffer* b = nullptr;
    fz_var(b);
    fz_try(ctx) {
        pdf_dict_del(ctx, stm, PDF_NAME(Filter));
        pdf_dict_del(ctx, stm, PDF_NAME(DecodeParms));
        b = fz_new_buffer_from_copied_data(
            ctx, reinterpret_cast<const unsigned char*>(data.data()), data.size());
        pdf_update_stream(ctx, doc, stm, b, 0);
    }
    fz_always(ctx) {
        fz_drop_buffer(ctx, b);
    }
    fz_catch(ctx) {
    }
}

bool isPdfWhite(unsigned char c)
{
    return c == 0 || c == 9 || c == 10 || c == 12 || c == 13 || c == 32;
}

bool isPdfDelim(unsigned char c)
{
    return c == '(' || c == ')' || c == '<' || c == '>' || c == '[' || c == ']'
        || c == '{' || c == '}' || c == '/' || c == '%';
}

int hexVal(unsigned char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

} // namespace

// ---------------------------------------------------------------------------
// Content-stream string blanking
// ---------------------------------------------------------------------------

int blankMatchingStrings(const std::string& in,
                         const std::vector<std::string>& needles,
                         std::string& out)
{
    out.clear();
    out.reserve(in.size());
    int blanked = 0;

    std::string tok;      // current bare keyword token
    std::string decoded;  // decoded string payload

    size_t i = 0;
    const size_t n = in.size();
    while (i < n) {
        const unsigned char c = static_cast<unsigned char>(in[i]);

        if (c == '%') {  // comment: copy to end of line
            while (i < n && in[i] != '\n' && in[i] != '\r') out.push_back(in[i++]);
            continue;
        }

        if (c == '(') {  // literal string
            const size_t start = i;
            decoded.clear();
            int depth = 0;
            while (i < n) {
                const char ch = in[i];
                if (ch == '\\' && i + 1 < n) {
                    decoded.push_back(in[i + 1]);
                    i += 2;
                    continue;
                }
                if (ch == '(') {
                    ++depth;
                    if (depth > 1) decoded.push_back(ch);
                    ++i;
                    continue;
                }
                if (ch == ')') {
                    --depth;
                    ++i;
                    if (depth <= 0) break;
                    decoded.push_back(ch);
                    continue;
                }
                decoded.push_back(ch);
                ++i;
            }
            if (containsAny(decoded, needles)) {
                out.append("()");
                ++blanked;
            } else {
                out.append(in, start, i - start);
            }
            continue;
        }

        if (c == '<' && i + 1 < n && in[i + 1] == '<') {  // dict open: consume BOTH
            out.append("<<");                            // '<' so the second is
            i += 2;                                      // never read as a hex
            continue;                                    // string opener
        }

        if (c == '<' && i + 1 < n && in[i + 1] != '<') {  // hex string
            const size_t start = i;
            decoded.clear();
            ++i;
            int hi = -1;
            while (i < n && in[i] != '>') {
                const int v = hexVal(static_cast<unsigned char>(in[i]));
                if (v >= 0) {
                    if (hi < 0) {
                        hi = v;
                    } else {
                        decoded.push_back(static_cast<char>((hi << 4) | v));
                        hi = -1;
                    }
                }
                ++i;
            }
            if (hi >= 0) decoded.push_back(static_cast<char>(hi << 4));
            if (i < n) ++i;  // consume '>'
            if (containsAny(decoded, needles)) {
                out.append("<>");
                ++blanked;
            } else {
                out.append(in, start, i - start);
            }
            continue;
        }

        if (isPdfWhite(c) || isPdfDelim(c)) {
            out.push_back(in[i++]);
            continue;
        }

        // Bare keyword / number token.
        tok.clear();
        const size_t start = i;
        while (i < n && !isPdfWhite(static_cast<unsigned char>(in[i]))
               && !isPdfDelim(static_cast<unsigned char>(in[i]))) {
            tok.push_back(in[i++]);
        }
        out.append(in, start, i - start);

        if (tok == "BI") {
            // Inline image: copy the dict normally until the ID keyword, then
            // copy the raw (possibly binary) payload verbatim through EI so
            // image bytes are never mistaken for strings.
            while (i < n) {
                // find the ID keyword
                if (in[i] == 'I' && i + 1 < n && in[i + 1] == 'D'
                    && (i + 2 >= n || isPdfWhite(static_cast<unsigned char>(in[i + 2])))) {
                    out.append("ID");
                    i += 2;
                    break;
                }
                out.push_back(in[i++]);
            }
            while (i < n) {
                if (in[i] == 'E' && i + 1 < n && in[i + 1] == 'I'
                    && i > 0 && isPdfWhite(static_cast<unsigned char>(in[i - 1]))
                    && (i + 2 >= n || isPdfWhite(static_cast<unsigned char>(in[i + 2])))) {
                    out.append("EI");
                    i += 2;
                    break;
                }
                out.push_back(in[i++]);
            }
        }
    }

    return blanked;
}

// ---------------------------------------------------------------------------
// Redacted-word capture
// ---------------------------------------------------------------------------

void collectRedactedWords(fz_context* ctx,
                          fz_page* page,
                          const std::vector<fz_rect>& rects,
                          std::vector<std::string>& wordsOut,
                          size_t minLen)
{
    if (!page || rects.empty()) return;

    // Constructed BEFORE fz_try — a longjmp into the fz_catch below must not
    // skip their destructors.
    std::string word;
    char utf8[8];
    fz_stext_page* stext = nullptr;
    fz_var(stext);

    fz_try(ctx) {
        fz_stext_options opts = pdfe::core::pdfeStextOptions();
        stext = fz_new_stext_page_from_page(ctx, page, &opts);
        if (stext) {
            for (fz_stext_block* block = stext->first_block; block; block = block->next) {
                if (block->type != FZ_STEXT_BLOCK_TEXT) continue;
                for (fz_stext_line* line = block->u.t.first_line; line; line = line->next) {
                    bool hit = false;
                    word.clear();
                    for (fz_stext_char* ch = line->first_char; ; ch = ch->next) {
                        const bool atEnd = (ch == nullptr);
                        const bool isSep =
                            atEnd || ch->c == ' ' || ch->c == '\t' || ch->c == '\n'
                            || ch->c == '\r' || ch->c == 0xa0;
                        if (!isSep) {
                            const int len = fz_runetochar(utf8, ch->c);
                            word.append(utf8, static_cast<size_t>(len));
                            const fz_rect cr = fz_rect_from_quad(ch->quad);
                            for (const auto& r : rects) {
                                if (cr.x0 < r.x1 && cr.x1 > r.x0
                                    && cr.y0 < r.y1 && cr.y1 > r.y0) {
                                    hit = true;
                                    break;
                                }
                            }
                        } else {
                            if (hit && word.size() >= minLen) {
                                wordsOut.push_back(word);
                            }
                            word.clear();
                            hit = false;
                        }
                        if (atEnd) break;
                    }
                }
            }
        }
    }
    fz_always(ctx) {
        fz_drop_stext_page(ctx, stext);
    }
    fz_catch(ctx) {
    }
}

// ---------------------------------------------------------------------------
// Page-scoped scrubbing
// ---------------------------------------------------------------------------

namespace {

/// Recursively blank matching strings in every form XObject reachable from
/// `resources`. All C++ state is owned by the caller's frame.
void scrubFormXObjects(fz_context* ctx,
                       pdf_document* doc,
                       pdf_obj* resources,
                       const std::vector<std::string>& words,
                       std::set<int>& seen,
                       std::string& scratchIn,
                       std::string& scratchOut,
                       int depth,
                       ScrubStats& stats)
{
    if (!resources || depth > 16) return;
    pdf_obj* xobjs = pdf_dict_get(ctx, resources, PDF_NAME(XObject));
    if (!pdf_is_dict(ctx, xobjs)) return;

    const int count = pdf_dict_len(ctx, xobjs);
    for (int i = 0; i < count; ++i) {
        pdf_obj* val = pdf_dict_get_val(ctx, xobjs, i);
        if (!val) continue;
        const int num = pdf_to_num(ctx, val);
        if (num > 0) {
            if (seen.count(num)) continue;
            seen.insert(num);
        }
        pdf_obj* sub = pdf_dict_get(ctx, val, PDF_NAME(Subtype));
        if (sub != PDF_NAME(Form)) continue;

        loadStreamText(ctx, val, scratchIn);
        if (!scratchIn.empty() && containsAny(scratchIn, words)) {
            const int blanked = blankMatchingStrings(scratchIn, words, scratchOut);
            if (blanked > 0) {
                replaceStream(ctx, doc, val, scratchOut);
                stats.xobjectStringsBlanked += blanked;
            }
        }

        // Nested form XObjects.
        scrubFormXObjects(ctx, doc, pdf_dict_get(ctx, val, PDF_NAME(Resources)),
                          words, seen, scratchIn, scratchOut, depth + 1, stats);
    }
}

/// Delete /ActualText, /Alt, /E and /T entries in the structure tree whose
/// value carries one of the removed words.
void scrubStructNode(fz_context* ctx,
                     pdf_obj* node,
                     const std::vector<std::string>& words,
                     std::set<int>& seen,
                     std::string& scratch,
                     int depth,
                     ScrubStats& stats)
{
    if (!node || depth > 64) return;

    if (pdf_is_array(ctx, node)) {
        const int n = pdf_array_len(ctx, node);
        for (int i = 0; i < n; ++i) {
            scrubStructNode(ctx, pdf_array_get(ctx, node, i), words, seen,
                            scratch, depth + 1, stats);
        }
        return;
    }
    if (!pdf_is_dict(ctx, node)) return;

    const int num = pdf_to_num(ctx, node);
    if (num > 0) {
        if (seen.count(num)) return;
        seen.insert(num);
    }

    pdf_obj* keys[4] = {PDF_NAME(ActualText), PDF_NAME(Alt), PDF_NAME(E), PDF_NAME(T)};
    for (pdf_obj* key : keys) {
        pdf_obj* v = pdf_dict_get(ctx, node, key);
        if (!v) continue;
        scratch.clear();
        appendObjText(ctx, v, scratch);
        if (!scratch.empty() && containsAny(scratch, words)) {
            pdf_dict_del(ctx, node, key);
            ++stats.structEntriesRemoved;
        }
    }

    scrubStructNode(ctx, pdf_dict_get(ctx, node, PDF_NAME(K)), words, seen,
                    scratch, depth + 1, stats);
}

} // namespace

void scrubPageScopedSurfaces(fz_context* ctx,
                             pdf_document* doc,
                             pdf_page* page,
                             const std::vector<fz_rect>& rects,
                             const std::vector<std::string>& words,
                             ScrubStats& stats)
{
    if (!doc || !page || rects.empty()) return;

    // Owned by THIS frame, constructed before fz_try.
    std::set<int> seen;
    std::string scratchIn;
    std::string scratchOut;
    std::vector<pdf_annot*> doomed;

    fz_try(ctx) {
        // --- PDF-63: annotations overlapping a redaction region -------------
        // Collected first, deleted after the walk (deleting invalidates the
        // iteration). Redaction annotations themselves are left alone —
        // pdf_redact_page consumes them.
        for (pdf_annot* a = pdf_first_annot(ctx, page); a; a = pdf_next_annot(ctx, a)) {
            if (pdf_annot_type(ctx, a) == PDF_ANNOT_REDACT) continue;
            const fz_rect ar = pdf_bound_annot(ctx, a);
            for (const auto& r : rects) {
                if (ar.x0 < r.x1 && ar.x1 > r.x0 && ar.y0 < r.y1 && ar.y1 > r.y0) {
                    doomed.push_back(a);
                    break;
                }
            }
        }
        for (pdf_annot* a : doomed) {
            pdf_delete_annot(ctx, page, a);
            ++stats.annotationsRemoved;
        }

        if (!words.empty()) {
            // --- PDF-62: text inside form XObjects --------------------------
            scrubFormXObjects(ctx, doc, pdf_dict_get(ctx, page->obj, PDF_NAME(Resources)),
                              words, seen, scratchIn, scratchOut, 0, stats);

            // --- PDF-61: struct tree / tagged content (/ActualText, /Alt) ---
            seen.clear();
            pdf_obj* root = pdf_dict_get(ctx, pdf_trailer(ctx, doc), PDF_NAME(Root));
            pdf_obj* structRoot = pdf_dict_get(ctx, root, PDF_NAME(StructTreeRoot));
            if (structRoot) {
                scrubStructNode(ctx, structRoot, words, seen, scratchIn, 0, stats);
            }
        }
    }
    fz_catch(ctx) {
    }
}

void scrubPageContentResiduals(fz_context* ctx,
                               pdf_document* doc,
                               pdf_page* page,
                               const std::vector<std::string>& words,
                               ScrubStats& stats)
{
    if (!doc || !page || words.empty()) return;

    // Owned by THIS frame, constructed before fz_try.
    std::string in;
    std::string out;
    std::vector<pdf_obj*> streams;

    fz_try(ctx) {
        pdf_obj* contents = pdf_dict_get(ctx, page->obj, PDF_NAME(Contents));
        if (pdf_is_array(ctx, contents)) {
            const int n = pdf_array_len(ctx, contents);
            for (int i = 0; i < n; ++i) streams.push_back(pdf_array_get(ctx, contents, i));
        } else if (contents) {
            streams.push_back(contents);
        }

        for (pdf_obj* stm : streams) {
            loadStreamText(ctx, stm, in);
            if (in.empty() || !containsAny(in, words)) continue;  // clean: leave as-is
            const int blanked = blankMatchingStrings(in, words, out);
            if (blanked > 0) {
                replaceStream(ctx, doc, stm, out);
                stats.pageStringsBlanked += blanked;
            }
        }
    }
    fz_catch(ctx) {
    }
}

// ---------------------------------------------------------------------------
// Document-scoped scrubbing
// ---------------------------------------------------------------------------

namespace {

/// Walk a PDF name tree, deleting (name, value) pairs for which `matches`
/// returns true. Returns the number of pairs removed.
template <typename MatchFn>
int pruneNameTree(fz_context* ctx,
                  pdf_obj* node,
                  std::set<int>& seen,
                  int depth,
                  const MatchFn& matches)
{
    if (!node || depth > 32) return 0;
    const int num = pdf_to_num(ctx, node);
    if (num > 0) {
        if (seen.count(num)) return 0;
        seen.insert(num);
    }

    int removed = 0;

    pdf_obj* kids = pdf_dict_get(ctx, node, PDF_NAME(Kids));
    if (pdf_is_array(ctx, kids)) {
        const int n = pdf_array_len(ctx, kids);
        for (int i = 0; i < n; ++i) {
            removed += pruneNameTree(ctx, pdf_array_get(ctx, kids, i), seen, depth + 1, matches);
        }
    }

    pdf_obj* names = pdf_dict_get(ctx, node, PDF_NAME(Names));
    if (pdf_is_array(ctx, names)) {
        for (int i = pdf_array_len(ctx, names) - 2; i >= 0; i -= 2) {
            pdf_obj* key = pdf_array_get(ctx, names, i);
            pdf_obj* val = pdf_array_get(ctx, names, i + 1);
            if (matches(key, val)) {
                pdf_array_delete(ctx, names, i + 1);
                pdf_array_delete(ctx, names, i);
                ++removed;
            }
        }
    }
    return removed;
}

/// Prune outline items whose /Title carries a removed word, relinking the
/// sibling chain so the remaining bookmarks survive intact.
int pruneOutlineChildren(fz_context* ctx,
                         pdf_obj* parent,
                         const std::vector<std::string>& words,
                         std::vector<pdf_obj*>& keptScratch,
                         std::string& scratch,
                         int depth)
{
    if (!parent || depth > 32) return 0;

    // Snapshot the child chain (the local vector is a copy taken at this
    // recursion level; deeper levels get their own via recursion below).
    std::vector<pdf_obj*> kept;
    int removed = 0;
    int guard = 0;
    for (pdf_obj* item = pdf_dict_get(ctx, parent, PDF_NAME(First));
         item && guard < 8192;
         item = pdf_dict_get(ctx, item, PDF_NAME(Next)), ++guard) {
        scratch.clear();
        appendObjText(ctx, pdf_dict_get(ctx, item, PDF_NAME(Title)), scratch);
        if (!scratch.empty() && containsAny(scratch, words)) {
            ++removed;
            continue;  // drops this item AND its subtree
        }
        kept.push_back(item);
    }

    if (removed > 0) {
        if (kept.empty()) {
            pdf_dict_del(ctx, parent, PDF_NAME(First));
            pdf_dict_del(ctx, parent, PDF_NAME(Last));
        } else {
            pdf_dict_put(ctx, parent, PDF_NAME(First), kept.front());
            pdf_dict_put(ctx, parent, PDF_NAME(Last), kept.back());
            for (size_t i = 0; i < kept.size(); ++i) {
                if (i > 0) pdf_dict_put(ctx, kept[i], PDF_NAME(Prev), kept[i - 1]);
                else pdf_dict_del(ctx, kept[i], PDF_NAME(Prev));
                if (i + 1 < kept.size()) pdf_dict_put(ctx, kept[i], PDF_NAME(Next), kept[i + 1]);
                else pdf_dict_del(ctx, kept[i], PDF_NAME(Next));
            }
        }
        if (pdf_dict_get(ctx, parent, PDF_NAME(Count))) {
            pdf_dict_put_int(ctx, parent, PDF_NAME(Count),
                             static_cast<int64_t>(kept.size()));
        }
    }

    for (pdf_obj* item : kept) {
        removed += pruneOutlineChildren(ctx, item, words, keptScratch, scratch, depth + 1);
    }
    return removed;
}

} // namespace

void scrubDocumentScopedSurfaces(fz_context* ctx,
                                 pdf_document* doc,
                                 const std::vector<std::string>& words,
                                 bool fullSanitize,
                                 ScrubStats& stats)
{
    if (!doc) return;
    if (words.empty() && !fullSanitize) return;

    // Owned by THIS frame, constructed before fz_try.
    std::set<int> seen;
    std::string scratch;
    std::string scratch2;
    std::vector<pdf_obj*> doomedKeys;
    std::vector<pdf_obj*> outlineScratch;

    fz_try(ctx) {
        pdf_obj* trailer = pdf_trailer(ctx, doc);
        pdf_obj* root = pdf_dict_get(ctx, trailer, PDF_NAME(Root));
        pdf_obj* names = pdf_dict_get(ctx, root, PDF_NAME(Names));

        // --- PDF-60: document information dictionary ------------------------
        pdf_obj* info = pdf_dict_get(ctx, trailer, PDF_NAME(Info));
        if (pdf_is_dict(ctx, info)) {
            if (fullSanitize) {
                const int n = pdf_dict_len(ctx, info);
                for (int i = n - 1; i >= 0; --i) {
                    pdf_dict_del(ctx, info, pdf_dict_get_key(ctx, info, i));
                    ++stats.infoKeysRemoved;
                }
            } else {
                doomedKeys.clear();
                const int n = pdf_dict_len(ctx, info);
                for (int i = 0; i < n; ++i) {
                    scratch.clear();
                    appendObjText(ctx, pdf_dict_get_val(ctx, info, i), scratch);
                    if (!scratch.empty() && containsAny(scratch, words)) {
                        doomedKeys.push_back(pdf_dict_get_key(ctx, info, i));
                    }
                }
                for (pdf_obj* k : doomedKeys) {
                    pdf_dict_del(ctx, info, k);
                    ++stats.infoKeysRemoved;
                }
            }
        }

        // --- PDF-60: XMP metadata packet ------------------------------------
        pdf_obj* meta = pdf_dict_get(ctx, root, PDF_NAME(Metadata));
        if (meta) {
            if (fullSanitize) {
                pdf_dict_del(ctx, root, PDF_NAME(Metadata));
                ++stats.xmpOccurrencesRemoved;
            } else {
                loadStreamText(ctx, meta, scratch);
                if (!scratch.empty() && containsAny(scratch, words)) {
                    // Excise the occurrences in place: unrelated XMP properties
                    // must survive (blanket-dropping the packet would be its
                    // own defect).
                    int hits = 0;
                    for (const auto& w : words) {
                        if (w.empty()) continue;
                        size_t p = 0;
                        while ((p = scratch.find(w, p)) != std::string::npos) {
                            scratch.erase(p, w.size());
                            ++hits;
                        }
                    }
                    if (hits > 0) {
                        replaceStream(ctx, doc, meta, scratch);
                        stats.xmpOccurrencesRemoved += hits;
                    }
                }
            }
        }

        // --- PDF-64: embedded / attached files ------------------------------
        if (pdf_is_dict(ctx, names)) {
            pdf_obj* ef = pdf_dict_get(ctx, names, PDF_NAME(EmbeddedFiles));
            if (ef) {
                if (fullSanitize) {
                    pdf_dict_del(ctx, names, PDF_NAME(EmbeddedFiles));
                    ++stats.embeddedFilesRemoved;
                } else {
                    seen.clear();
                    stats.embeddedFilesRemoved += pruneNameTree(
                        ctx, ef, seen, 0,
                        [&](pdf_obj* key, pdf_obj* val) -> bool {
                            scratch.clear();
                            appendObjText(ctx, key, scratch);
                            appendObjText(ctx, pdf_dict_get(ctx, val, PDF_NAME(F)), scratch);
                            appendObjText(ctx, pdf_dict_get(ctx, val, PDF_NAME(UF)), scratch);
                            appendObjText(ctx, pdf_dict_get(ctx, val, PDF_NAME(Desc)), scratch);
                            if (containsAny(scratch, words)) return true;
                            pdf_obj* efd = pdf_dict_get(ctx, val, PDF_NAME(EF));
                            if (pdf_is_dict(ctx, efd)) {
                                const int n = pdf_dict_len(ctx, efd);
                                for (int i = 0; i < n; ++i) {
                                    loadStreamText(ctx, pdf_dict_get_val(ctx, efd, i), scratch2);
                                    if (!scratch2.empty() && containsAny(scratch2, words)) {
                                        return true;
                                    }
                                }
                            }
                            return false;
                        });
                }
            }

            // --- PDF-66: document-level JavaScript --------------------------
            pdf_obj* js = pdf_dict_get(ctx, names, PDF_NAME(JavaScript));
            if (js) {
                if (fullSanitize) {
                    pdf_dict_del(ctx, names, PDF_NAME(JavaScript));
                    ++stats.javascriptEntriesRemoved;
                } else {
                    seen.clear();
                    stats.javascriptEntriesRemoved += pruneNameTree(
                        ctx, js, seen, 0,
                        [&](pdf_obj* key, pdf_obj* val) -> bool {
                            scratch.clear();
                            appendObjText(ctx, key, scratch);
                            pdf_obj* body = pdf_dict_get(ctx, val, PDF_NAME(JS));
                            appendObjText(ctx, body, scratch);
                            if (containsAny(scratch, words)) return true;
                            if (body && pdf_is_stream(ctx, body)) {
                                loadStreamText(ctx, body, scratch2);
                                if (!scratch2.empty() && containsAny(scratch2, words)) {
                                    return true;
                                }
                            }
                            return false;
                        });
                }
            }
        }

        // --- PDF-65: outlines / bookmarks -----------------------------------
        pdf_obj* outlines = pdf_dict_get(ctx, root, PDF_NAME(Outlines));
        if (outlines) {
            if (fullSanitize) {
                pdf_dict_del(ctx, root, PDF_NAME(Outlines));
                ++stats.outlineItemsRemoved;
            } else {
                stats.outlineItemsRemoved +=
                    pruneOutlineChildren(ctx, outlines, words, outlineScratch, scratch, 0);
            }
        }
    }
    fz_catch(ctx) {
    }
}

} // namespace pdfe::redaction
