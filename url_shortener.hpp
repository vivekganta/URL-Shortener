#pragma once
// Core logic for the URL shortener: base62 encoding, a thread-safe LRU
// cache, flat-file persistence, and the UrlShortener service that ties
// them together. Kept in one header on purpose so the whole "engine" is
// readable top to bottom in a single file.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <list>
#include <mutex>
#include <optional>
#include <regex>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

// ---------------------------------------------------------------------------
// base62: turns an integer id into a short, URL-safe string and back.
// ---------------------------------------------------------------------------
namespace base62 {

inline const std::string& alphabet() {
    static const std::string a =
        "0123456789"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz";
    return a;
}

inline std::string encode(uint64_t id) {
    const std::string& digits = alphabet();
    if (id == 0) return std::string(1, digits[0]);
    std::string out;
    while (id > 0) {
        out.push_back(digits[id % 62]);
        id /= 62;
    }
    std::reverse(out.begin(), out.end());
    return out;
}

inline uint64_t decode(const std::string& code) {
    static int8_t lookup[256];
    static bool initialized = false;
    if (!initialized) {
        for (int i = 0; i < 256; ++i) lookup[i] = -1;
        const std::string& digits = alphabet();
        for (size_t i = 0; i < digits.size(); ++i)
            lookup[static_cast<unsigned char>(digits[i])] = static_cast<int8_t>(i);
        initialized = true;
    }
    uint64_t value = 0;
    for (unsigned char c : code) {
        int8_t d = lookup[c];
        if (d < 0) throw std::invalid_argument("invalid base62 character");
        value = value * 62 + static_cast<uint64_t>(d);
    }
    return value;
}

} // namespace base62

// ---------------------------------------------------------------------------
// LRUCache<Key, Value>: fixed-capacity, thread-safe, O(1) get/put.
// ---------------------------------------------------------------------------
template <typename Key, typename Value>
class LRUCache {
public:
    explicit LRUCache(size_t capacity) : capacity_(capacity) {}

    std::optional<Value> get(const Key& key) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = index_.find(key);
        if (it == index_.end()) return std::nullopt;
        items_.splice(items_.begin(), items_, it->second);
        return it->second->second;
    }

    void put(const Key& key, const Value& value) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = index_.find(key);
        if (it != index_.end()) {
            it->second->second = value;
            items_.splice(items_.begin(), items_, it->second);
            return;
        }
        items_.emplace_front(key, value);
        index_[key] = items_.begin();
        if (items_.size() > capacity_) {
            auto last = std::prev(items_.end());
            index_.erase(last->first);
            items_.pop_back();
        }
    }

    void erase(const Key& key) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = index_.find(key);
        if (it == index_.end()) return;
        items_.erase(it->second);
        index_.erase(it);
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return items_.size();
    }

private:
    using ListType = std::list<std::pair<Key, Value>>;
    size_t capacity_;
    ListType items_;
    std::unordered_map<Key, typename ListType::iterator> index_;
    mutable std::mutex mutex_;
};

// ---------------------------------------------------------------------------
// UrlRecord + Storage: the persisted record and its thread-safe flat-file
// backed store. Format is one record per line, fields separated by '\x01'
// (a byte that never legally appears in a URL), so we avoid pulling in a
// JSON library just to persist six fields.
// ---------------------------------------------------------------------------
struct UrlRecord {
    std::string long_url;
    std::string short_code;
    int64_t created_at = 0;
    int64_t expires_at = 0; // 0 = never
    uint64_t clicks = 0;
    bool custom_alias = false;

    bool isExpired(int64_t now) const { return expires_at != 0 && now >= expires_at; }
};

class Storage {
public:
    explicit Storage(std::string filepath) : filepath_(std::move(filepath)) { load(); }

    void put(const UrlRecord& r) {
        std::unique_lock lock(mutex_);
        records_[r.short_code] = r;
        persist();
    }

    std::optional<UrlRecord> get(const std::string& code) const {
        std::shared_lock lock(mutex_);
        auto it = records_.find(code);
        if (it == records_.end()) return std::nullopt;
        return it->second;
    }

    bool exists(const std::string& code) const {
        std::shared_lock lock(mutex_);
        return records_.count(code) != 0;
    }

    std::optional<UrlRecord> recordClick(const std::string& code) {
        std::unique_lock lock(mutex_);
        auto it = records_.find(code);
        if (it == records_.end()) return std::nullopt;
        it->second.clicks++;
        persist();
        return it->second;
    }

    bool remove(const std::string& code) {
        std::unique_lock lock(mutex_);
        bool erased = records_.erase(code) > 0;
        if (erased) persist();
        return erased;
    }

    size_t size() const {
        std::shared_lock lock(mutex_);
        return records_.size();
    }

private:
    static constexpr char kSep = '\x01';

    void persist() const { // caller already holds unique_lock
        std::ofstream out(filepath_, std::ios::trunc);
        if (!out) return;
        for (const auto& [code, r] : records_) {
            out << r.short_code << kSep << r.long_url << kSep << r.created_at << kSep
                << r.expires_at << kSep << r.clicks << kSep << (r.custom_alias ? 1 : 0) << '\n';
        }
    }

    void load() {
        std::ifstream in(filepath_);
        if (!in) return; // fresh install
        std::string line;
        while (std::getline(in, line)) {
            std::vector<std::string> fields;
            std::stringstream ss(line);
            std::string field;
            while (std::getline(ss, field, kSep)) fields.push_back(field);
            if (fields.size() != 6) continue; // skip malformed/corrupt line
            UrlRecord r;
            r.short_code = fields[0];
            r.long_url = fields[1];
            r.created_at = std::stoll(fields[2]);
            r.expires_at = std::stoll(fields[3]);
            r.clicks = std::stoull(fields[4]);
            r.custom_alias = fields[5] == "1";
            records_[r.short_code] = r;
        }
    }

    std::string filepath_;
    std::unordered_map<std::string, UrlRecord> records_;
    mutable std::shared_mutex mutex_;
};

// ---------------------------------------------------------------------------
// UrlShortener: the service facade — validation, id generation, TTL and
// click-tracking, with the cache sitting in front of storage.
// ---------------------------------------------------------------------------
enum class ShortenError { None, InvalidUrl, AliasTaken, InvalidAlias };

struct ShortenResult {
    ShortenError error = ShortenError::None;
    std::string short_code;
};

class UrlShortener {
public:
    explicit UrlShortener(std::string storage_path, size_t cache_capacity = 1024)
        : storage_(std::move(storage_path)), cache_(cache_capacity) {
        next_id_.store(storage_.size() + 1);
    }

    ShortenResult shorten(const std::string& long_url, const std::string& custom_alias = "",
                          int64_t ttl_seconds = 0) {
        ShortenResult result;
        if (!isValidUrl(long_url)) {
            result.error = ShortenError::InvalidUrl;
            return result;
        }

        std::string code;
        if (!custom_alias.empty()) {
            if (!isValidAlias(custom_alias)) {
                result.error = ShortenError::InvalidAlias;
                return result;
            }
            if (storage_.exists(custom_alias)) {
                result.error = ShortenError::AliasTaken;
                return result;
            }
            code = custom_alias;
        } else {
            do {
                code = base62::encode(next_id_.fetch_add(1));
            } while (storage_.exists(code));
        }

        UrlRecord record;
        record.long_url = long_url;
        record.short_code = code;
        record.created_at = nowSeconds();
        record.expires_at = ttl_seconds != 0 ? record.created_at + ttl_seconds : 0;
        record.custom_alias = !custom_alias.empty();

        storage_.put(record);
        cache_.put(code, record);
        result.short_code = code;
        return result;
    }

    std::optional<std::string> resolve(const std::string& code) {
        int64_t now = nowSeconds();
        if (auto cached = cache_.get(code)) {
            if (cached->isExpired(now)) { expireAndEvict(code); return std::nullopt; }
            if (auto updated = storage_.recordClick(code)) cache_.put(code, *updated);
            return cached->long_url;
        }
        auto record = storage_.get(code);
        if (!record) return std::nullopt;
        if (record->isExpired(now)) { expireAndEvict(code); return std::nullopt; }
        if (auto updated = storage_.recordClick(code)) cache_.put(code, *updated);
        return record->long_url;
    }

    std::optional<UrlRecord> stats(const std::string& code) const {
        auto record = storage_.get(code);
        if (!record || record->isExpired(nowSeconds())) return std::nullopt;
        return record;
    }

    bool remove(const std::string& code) {
        cache_.erase(code);
        return storage_.remove(code);
    }

    size_t totalUrls() const { return storage_.size(); }

private:
    static int64_t nowSeconds() {
        return std::chrono::duration_cast<std::chrono::seconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }

    static bool isValidUrl(const std::string& url) {
        static const std::regex pattern(R"(^https?://[^\s/$.?#].[^\s]*$)", std::regex::icase);
        return std::regex_match(url, pattern);
    }

    static bool isValidAlias(const std::string& alias) {
        static const std::regex pattern(R"(^[A-Za-z0-9_-]{3,32}$)");
        return std::regex_match(alias, pattern);
    }

    void expireAndEvict(const std::string& code) {
        cache_.erase(code);
        storage_.remove(code);
    }

    Storage storage_;
    LRUCache<std::string, UrlRecord> cache_;
    std::atomic<uint64_t> next_id_{1};
};
