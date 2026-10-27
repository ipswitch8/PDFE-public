#include "fonts/FontAnalysisCache.h"

#include <sqlite3.h>

#include "core/TempFileSafety.h"

#include <filesystem>
#include <system_error>
#include <list>
#include <unordered_map>
#include <mutex>

namespace pdfe::fonts {

class FontAnalysisCache::Impl {
public:
    explicit Impl(const std::filesystem::path& dbPath)
    {
        // SEC-103. Every construction site in the product passes NOTHING —
        // MainWindow.cpp constructs FontAnalysisCache{} and
        // FontSubstitutionEngine defaults its cachePath — so the shipping
        // configuration is ":memory:" and there is no file to attack. The
        // gap's premise ("a local-user-writable SQLite file") does not hold
        // today. What DOES hold is that this constructor is public, takes a
        // path, and used to hand it straight to sqlite3_open with no flag
        // restrictions, while persisting attacker-influenced font paths into
        // it. A single future call site supplying a path would have created
        // exactly the surface described, silently.
        //
        // So the path-taking form is now hardened at the point of creation
        // rather than left to whoever adds that call site: the file is
        // created with a DACL granting the calling user and nobody else, and
        // an existing file whose ACL is wider is REFUSED rather than adopted.
        // Adopting a pre-planted world-writable database is the escalation;
        // failing closed to an in-memory cache costs a cold start.
        std::string dbPathStr = ":memory:";
        if (!dbPath.empty()) {
            std::error_code ec;
            if (core::createOwnerOnlyFile(dbPath, ec)) {
                dbPathStr = dbPath.string();
            }
            // else: fall through to ":memory:". The cache is a performance
            // optimisation; declining to use an unsafe location degrades
            // speed, and using it would degrade the machine.
        }
        int rc = sqlite3_open(dbPathStr.c_str(), &db_);
        if (rc != SQLITE_OK) {
            if (db_) sqlite3_close(db_);
            db_ = nullptr;
            return;
        }
        usingOnDiskPath_ = (dbPathStr != ":memory:");

        createTable();
        prepareStatements();
    }

    bool isOnDisk() const noexcept { return usingOnDiskPath_; }

    ~Impl()
    {
        if (selectStmt_) sqlite3_finalize(selectStmt_);
        if (insertStmt_) sqlite3_finalize(insertStmt_);
        if (deleteStmt_) sqlite3_finalize(deleteStmt_);
        if (countStmt_) sqlite3_finalize(countStmt_);
        if (containsStmt_) sqlite3_finalize(containsStmt_);
        if (db_) sqlite3_close(db_);
    }

    std::optional<FontMetricProfile> get(const std::string& psName)
    {
        // Check LRU first
        auto it = lruMap_.find(psName);
        if (it != lruMap_.end()) {
            // Move to front of LRU
            lruList_.splice(lruList_.begin(), lruList_, it->second);
            return it->second->second;
        }

        // Check SQLite
        if (!db_ || !selectStmt_)
            return std::nullopt;

        sqlite3_reset(selectStmt_);
        sqlite3_bind_text(selectStmt_, 1, psName.c_str(), -1, SQLITE_TRANSIENT);

        if (sqlite3_step(selectStmt_) != SQLITE_ROW)
            return std::nullopt;

        FontMetricProfile profile = readRow(selectStmt_);

        // Promote to LRU
        addToLru(psName, profile);

        return profile;
    }

    void put(const FontMetricProfile& profile)
    {
        // Insert into LRU
        addToLru(profile.postScriptName, profile);

        // Insert into SQLite
        if (!db_ || !insertStmt_)
            return;

        sqlite3_reset(insertStmt_);
        bindProfile(insertStmt_, profile);
        sqlite3_step(insertStmt_);
    }

    bool contains(const std::string& psName) const
    {
        // Check LRU
        if (lruMap_.find(psName) != lruMap_.end())
            return true;

        // Check SQLite
        if (!db_ || !containsStmt_)
            return false;

        sqlite3_reset(containsStmt_);
        sqlite3_bind_text(containsStmt_, 1, psName.c_str(), -1, SQLITE_TRANSIENT);

        return sqlite3_step(containsStmt_) == SQLITE_ROW;
    }

    void clear()
    {
        lruList_.clear();
        lruMap_.clear();

        if (db_) {
            sqlite3_exec(db_, "DELETE FROM font_profiles", nullptr, nullptr, nullptr);
        }
    }

    size_t size() const
    {
        if (!db_ || !countStmt_)
            return 0;

        sqlite3_reset(countStmt_);
        if (sqlite3_step(countStmt_) == SQLITE_ROW) {
            return static_cast<size_t>(sqlite3_column_int64(countStmt_, 0));
        }
        return 0;
    }

    size_t memSize() const
    {
        return lruMap_.size();
    }

private:
    void createTable()
    {
        const char* sql =
            "CREATE TABLE IF NOT EXISTS font_profiles ("
            "ps_name TEXT PRIMARY KEY, "
            "family TEXT, "
            "style TEXT, "
            "units_per_em INT, "
            "ascender REAL, "
            "descender REAL, "
            "x_height REAL, "
            "cap_height REAL, "
            "weight_class INT, "
            "width_class INT, "
            "stem_v REAL, "
            "stem_h REAL, "
            "x_to_cap REAL, "
            "asc_to_cap REAL, "
            "avg_width REAL, "
            "is_serif INT, "
            "is_mono INT, "
            "is_italic INT, "
            "italic_angle REAL, "
            "serif_extent REAL, "
            "glyph_count INT, "
            "has_latin_basic INT, "
            "has_latin_ext_a INT, "
            "has_cyrillic INT, "
            "has_cjk INT, "
            "file_path TEXT, "
            "primary_script INT DEFAULT 0, "
            "supported_scripts INT DEFAULT 0)";

        sqlite3_exec(db_, sql, nullptr, nullptr, nullptr);
    }

    void prepareStatements()
    {
        sqlite3_prepare_v2(db_,
            "SELECT * FROM font_profiles WHERE ps_name = ?",
            -1, &selectStmt_, nullptr);

        sqlite3_prepare_v2(db_,
            "REPLACE INTO font_profiles VALUES ("
            "?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
            -1, &insertStmt_, nullptr);

        sqlite3_prepare_v2(db_,
            "DELETE FROM font_profiles WHERE ps_name = ?",
            -1, &deleteStmt_, nullptr);

        sqlite3_prepare_v2(db_,
            "SELECT COUNT(*) FROM font_profiles",
            -1, &countStmt_, nullptr);

        sqlite3_prepare_v2(db_,
            "SELECT 1 FROM font_profiles WHERE ps_name = ?",
            -1, &containsStmt_, nullptr);
    }

    FontMetricProfile readRow(sqlite3_stmt* stmt)
    {
        FontMetricProfile p;
        int col = 0;
        p.postScriptName = reinterpret_cast<const char*>(sqlite3_column_text(stmt, col++));
        auto txt = sqlite3_column_text(stmt, col++);
        p.familyName = txt ? reinterpret_cast<const char*>(txt) : "";
        txt = sqlite3_column_text(stmt, col++);
        p.styleName = txt ? reinterpret_cast<const char*>(txt) : "";
        p.unitsPerEm = sqlite3_column_int(stmt, col++);
        p.ascender = static_cast<float>(sqlite3_column_double(stmt, col++));
        p.descender = static_cast<float>(sqlite3_column_double(stmt, col++));
        p.xHeight = static_cast<float>(sqlite3_column_double(stmt, col++));
        p.capHeight = static_cast<float>(sqlite3_column_double(stmt, col++));
        p.weightClass = sqlite3_column_int(stmt, col++);
        p.widthClass = sqlite3_column_int(stmt, col++);
        p.stemWeightV = static_cast<float>(sqlite3_column_double(stmt, col++));
        p.stemWeightH = static_cast<float>(sqlite3_column_double(stmt, col++));
        p.xToCapRatio = static_cast<float>(sqlite3_column_double(stmt, col++));
        p.ascToCapRatio = static_cast<float>(sqlite3_column_double(stmt, col++));
        p.avgCharWidth = static_cast<float>(sqlite3_column_double(stmt, col++));
        p.isSerif = sqlite3_column_int(stmt, col++) != 0;
        p.isMonospace = sqlite3_column_int(stmt, col++) != 0;
        p.isItalic = sqlite3_column_int(stmt, col++) != 0;
        p.italicAngle = static_cast<float>(sqlite3_column_double(stmt, col++));
        p.serifExtent = static_cast<float>(sqlite3_column_double(stmt, col++));
        p.glyphCount = sqlite3_column_int(stmt, col++);
        p.hasLatinBasic = sqlite3_column_int(stmt, col++) != 0;
        p.hasLatinExtA = sqlite3_column_int(stmt, col++) != 0;
        p.hasCyrillic = sqlite3_column_int(stmt, col++) != 0;
        p.hasCJK = sqlite3_column_int(stmt, col++) != 0;
        txt = sqlite3_column_text(stmt, col++);
        p.filePath = txt ? reinterpret_cast<const char*>(txt) : "";
        // primary_script column (may not exist in older schemas)
        if (sqlite3_column_count(stmt) > col) {
            p.primaryScript = static_cast<FontScript>(sqlite3_column_int(stmt, col++));
        }
        // supported_scripts bitmask column (may not exist in older schemas).
        // Bit (1 << scriptValue) is set per covered FontScript.
        if (sqlite3_column_count(stmt) > col) {
            int mask = sqlite3_column_int(stmt, col++);
            for (int s = static_cast<int>(FontScript::Latin);
                 s <= static_cast<int>(FontScript::Other); ++s) {
                if (mask & (1 << s))
                    p.supportedScripts.insert(static_cast<FontScript>(s));
            }
        }
        return p;
    }

    void bindProfile(sqlite3_stmt* stmt, const FontMetricProfile& p)
    {
        int col = 1;
        sqlite3_bind_text(stmt, col++, p.postScriptName.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, col++, p.familyName.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, col++, p.styleName.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, col++, p.unitsPerEm);
        sqlite3_bind_double(stmt, col++, p.ascender);
        sqlite3_bind_double(stmt, col++, p.descender);
        sqlite3_bind_double(stmt, col++, p.xHeight);
        sqlite3_bind_double(stmt, col++, p.capHeight);
        sqlite3_bind_int(stmt, col++, p.weightClass);
        sqlite3_bind_int(stmt, col++, p.widthClass);
        sqlite3_bind_double(stmt, col++, p.stemWeightV);
        sqlite3_bind_double(stmt, col++, p.stemWeightH);
        sqlite3_bind_double(stmt, col++, p.xToCapRatio);
        sqlite3_bind_double(stmt, col++, p.ascToCapRatio);
        sqlite3_bind_double(stmt, col++, p.avgCharWidth);
        sqlite3_bind_int(stmt, col++, p.isSerif ? 1 : 0);
        sqlite3_bind_int(stmt, col++, p.isMonospace ? 1 : 0);
        sqlite3_bind_int(stmt, col++, p.isItalic ? 1 : 0);
        sqlite3_bind_double(stmt, col++, p.italicAngle);
        sqlite3_bind_double(stmt, col++, p.serifExtent);
        sqlite3_bind_int(stmt, col++, p.glyphCount);
        sqlite3_bind_int(stmt, col++, p.hasLatinBasic ? 1 : 0);
        sqlite3_bind_int(stmt, col++, p.hasLatinExtA ? 1 : 0);
        sqlite3_bind_int(stmt, col++, p.hasCyrillic ? 1 : 0);
        sqlite3_bind_int(stmt, col++, p.hasCJK ? 1 : 0);
        sqlite3_bind_text(stmt, col++, p.filePath.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, col++, static_cast<int>(p.primaryScript));
        int scriptMask = 0;
        for (FontScript s : p.supportedScripts)
            scriptMask |= (1 << static_cast<int>(s));
        sqlite3_bind_int(stmt, col++, scriptMask);
    }

    void addToLru(const std::string& key, const FontMetricProfile& profile)
    {
        auto it = lruMap_.find(key);
        if (it != lruMap_.end()) {
            // Update existing and move to front
            it->second->second = profile;
            lruList_.splice(lruList_.begin(), lruList_, it->second);
            return;
        }

        // Insert new entry at front
        lruList_.emplace_front(key, profile);
        lruMap_[key] = lruList_.begin();

        // Evict oldest if over capacity
        while (lruMap_.size() > MAX_LRU_SIZE) {
            auto& oldest = lruList_.back();
            lruMap_.erase(oldest.first);
            lruList_.pop_back();
        }
    }

    static constexpr size_t MAX_LRU_SIZE = 256;

    sqlite3* db_ = nullptr;
    bool usingOnDiskPath_ = false;
    sqlite3_stmt* selectStmt_ = nullptr;
    sqlite3_stmt* insertStmt_ = nullptr;
    sqlite3_stmt* deleteStmt_ = nullptr;
    sqlite3_stmt* countStmt_ = nullptr;
    sqlite3_stmt* containsStmt_ = nullptr;

    // LRU cache: list of (key, profile), map from key to iterator
    using LruList = std::list<std::pair<std::string, FontMetricProfile>>;
    LruList lruList_;
    std::unordered_map<std::string, LruList::iterator> lruMap_;
};

FontAnalysisCache::FontAnalysisCache(const std::filesystem::path& dbPath)
    : pImpl(std::make_unique<Impl>(dbPath))
{
}

FontAnalysisCache::~FontAnalysisCache() = default;

bool FontAnalysisCache::isOnDisk() const noexcept
{
    return pImpl && pImpl->isOnDisk();
}


std::optional<FontMetricProfile> FontAnalysisCache::get(const std::string& postScriptName)
{
    return pImpl->get(postScriptName);
}

void FontAnalysisCache::put(const FontMetricProfile& profile)
{
    pImpl->put(profile);
}

bool FontAnalysisCache::contains(const std::string& postScriptName) const
{
    return pImpl->contains(postScriptName);
}

void FontAnalysisCache::clear()
{
    pImpl->clear();
}

size_t FontAnalysisCache::size() const
{
    return pImpl->size();
}

size_t FontAnalysisCache::memSize() const
{
    return pImpl->memSize();
}

} // namespace pdfe::fonts
