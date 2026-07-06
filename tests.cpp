// All unit tests in one file, using a ~20-line homemade TEST/REQUIRE macro
// pair instead of pulling in a test framework dependency.
#include <cstdio>
#include <functional>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "url_shortener.hpp"

namespace {

struct TestCase { std::string name; std::function<void()> fn; };
std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}
struct Registrar { Registrar(std::string name, std::function<void()> fn) {
    registry().push_back({std::move(name), std::move(fn)});
}};
struct AssertionFailure { std::string message; };

std::string tempPath(const std::string& name) {
    return "/tmp/url_shortener_test_" + name + ".txt";
}

} // namespace

#define TEST_CASE(name) \
    void name(); \
    static Registrar registrar_##name(#name, name); \
    void name()

#define REQUIRE(cond) \
    do { if (!(cond)) throw AssertionFailure{std::string("REQUIRE failed: ") + #cond + \
        " (" + __FILE__ + ":" + std::to_string(__LINE__) + ")"}; } while (0)

#define REQUIRE_EQ(a, b) \
    do { if (!((a) == (b))) throw AssertionFailure{std::string("REQUIRE_EQ failed: ") + #a + \
        " != " + #b + " (" + __FILE__ + ":" + std::to_string(__LINE__) + ")"}; } while (0)

// --- base62 -----------------------------------------------------------

TEST_CASE(base62_roundtrip) {
    for (uint64_t i = 0; i < 1000; ++i) REQUIRE_EQ(base62::decode(base62::encode(i)), i);
    REQUIRE_EQ(base62::encode(0), "0");
}

TEST_CASE(base62_rejects_invalid_chars) {
    bool threw = false;
    try { base62::decode("abc!"); } catch (const std::invalid_argument&) { threw = true; }
    REQUIRE(threw);
}

// --- LRUCache -----------------------------------------------------------

TEST_CASE(lru_evicts_least_recently_used) {
    LRUCache<std::string, int> cache(2);
    cache.put("a", 1);
    cache.put("b", 2);
    cache.get("a");     // "a" now most recent
    cache.put("c", 3);  // evicts "b"
    REQUIRE(!cache.get("b").has_value());
    REQUIRE(cache.get("a").has_value());
    REQUIRE(cache.get("c").has_value());
}

TEST_CASE(lru_thread_safe_under_concurrent_access) {
    LRUCache<int, int> cache(100);
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&cache, t]() {
            for (int i = 0; i < 1000; ++i) { cache.put(i % 50, t * 1000 + i); cache.get(i % 50); }
        });
    }
    for (auto& th : threads) th.join();
    REQUIRE(cache.size() <= 100);
}

// --- UrlShortener ---------------------------------------------------------

TEST_CASE(shorten_and_resolve_roundtrip) {
    std::string path = tempPath("roundtrip");
    std::remove(path.c_str());
    UrlShortener shortener(path);
    auto result = shortener.shorten("https://example.com/some/path");
    REQUIRE(result.error == ShortenError::None);
    auto resolved = shortener.resolve(result.short_code);
    REQUIRE(resolved.has_value());
    REQUIRE_EQ(*resolved, "https://example.com/some/path");
}

TEST_CASE(rejects_invalid_url) {
    std::string path = tempPath("invalid_url");
    std::remove(path.c_str());
    UrlShortener shortener(path);
    REQUIRE(shortener.shorten("not-a-url").error == ShortenError::InvalidUrl);
}

TEST_CASE(custom_alias_and_conflict) {
    std::string path = tempPath("alias");
    std::remove(path.c_str());
    UrlShortener shortener(path);
    REQUIRE(shortener.shorten("https://example.com/a", "myalias").error == ShortenError::None);
    REQUIRE(shortener.shorten("https://example.com/b", "myalias").error == ShortenError::AliasTaken);
}

TEST_CASE(expired_link_not_resolvable) {
    std::string path = tempPath("expiry");
    std::remove(path.c_str());
    UrlShortener shortener(path);
    auto result = shortener.shorten("https://example.com/x", "", -1); // already expired
    REQUIRE(result.error == ShortenError::None);
    REQUIRE(!shortener.resolve(result.short_code).has_value());
}

TEST_CASE(click_count_increments) {
    std::string path = tempPath("clicks");
    std::remove(path.c_str());
    UrlShortener shortener(path);
    auto result = shortener.shorten("https://example.com/tracked");
    shortener.resolve(result.short_code);
    shortener.resolve(result.short_code);
    REQUIRE_EQ(shortener.stats(result.short_code)->clicks, 2u);
}

TEST_CASE(delete_removes_url) {
    std::string path = tempPath("delete");
    std::remove(path.c_str());
    UrlShortener shortener(path);
    auto result = shortener.shorten("https://example.com/gone");
    REQUIRE(shortener.remove(result.short_code));
    REQUIRE(!shortener.resolve(result.short_code).has_value());
}

TEST_CASE(persists_across_instances) {
    std::string path = tempPath("persist");
    std::remove(path.c_str());
    std::string code;
    { UrlShortener s(path); code = s.shorten("https://example.com/persisted").short_code; }
    { UrlShortener s(path); REQUIRE_EQ(*s.resolve(code), "https://example.com/persisted"); }
}

TEST_CASE(concurrent_shortens_produce_unique_codes) {
    std::string path = tempPath("concurrent");
    std::remove(path.c_str());
    UrlShortener shortener(path);
    const int kThreads = 8, kPerThread = 200;
    std::vector<std::thread> threads;
    std::vector<std::vector<std::string>> results(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t]() {
            for (int i = 0; i < kPerThread; ++i)
                results[t].push_back(shortener.shorten("https://example.com/" + std::to_string(t) + "_" + std::to_string(i)).short_code);
        });
    }
    for (auto& th : threads) th.join();
    std::unordered_map<std::string, int> seen;
    for (auto& v : results) for (auto& c : v) seen[c]++;
    for (auto& [code, count] : seen) REQUIRE_EQ(count, 1);
    REQUIRE_EQ(seen.size(), static_cast<size_t>(kThreads * kPerThread));
}

// --- runner -----------------------------------------------------------

int main() {
    int passed = 0, failed = 0;
    for (auto& test : registry()) {
        try {
            test.fn();
            std::cout << "[PASS] " << test.name << "\n";
            passed++;
        } catch (const AssertionFailure& e) {
            std::cout << "[FAIL] " << test.name << " -- " << e.message << "\n";
            failed++;
        } catch (const std::exception& e) {
            std::cout << "[FAIL] " << test.name << " -- exception: " << e.what() << "\n";
            failed++;
        }
    }
    std::cout << "\n" << passed << " passed, " << failed << " failed, " << registry().size() << " total\n";
    return failed == 0 ? 0 : 1;
}
