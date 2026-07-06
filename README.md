# URL Shortener (C++17)

A URL shortener service with a REST API, thread-safe core, LRU cache, base62
IDs, custom aliases, and link expiration (TTL) — built with **zero external
dependencies**: the HTTP server is hand-rolled on raw POSIX sockets and
persistence is a simple flat file, so the whole project is 5 files.

## Files

| File | What it is |
|---|---|
| `url_shortener.hpp` | Core engine: base62 codec, thread-safe LRU cache, flat-file storage, `UrlShortener` service |
| `main.cpp` | HTTP server (raw sockets, one thread per connection) + route handlers |
| `tests.cpp` | 11 unit tests (custom ~10-line test macros, no framework) |
| `build.sh` | One-line build via `g++` |
| `README.md` | this file |

## Design

- **Thread safety** — `std::shared_mutex` in storage (many concurrent
  readers, exclusive writers), `std::atomic` counter for ID generation, and
  each HTTP connection runs on its own thread.
- **LRU cache** — hand-written, O(1) get/put, fronts the storage layer so
  hot redirects skip disk I/O.
- **Base62 IDs** — monotonic counter → short, URL-safe code.
- **Custom aliases** — user-chosen codes with collision detection.
- **TTL expiry** — optional per-link expiration, evicted lazily on access.
- **Persistence** — flat file, one record per line, reloaded on startup.

## Build

```bash
chmod +x build.sh && ./build.sh
```

## Run

```bash
./url_shortener_server [port] [data_file] [base_url]
./url_shortener_server 8080 data/urls.txt http://localhost:8080
```

## Test

```bash
./run_tests
```

Covers base62 round-tripping, LRU eviction under concurrent load, URL
validation, alias conflicts, TTL expiry, click counting, deletion, and
persistence across restarts, including an 8-thread/1600-request test
asserting zero short-code collisions.

## API

Request bodies are `application/x-www-form-urlencoded` (simple to parse
without a JSON library).

**Shorten**
```
POST /api/shorten
url=https://example.com/path&custom_alias=optional&ttl_seconds=3600
→ {"short_code":"abc123","short_url":"http://localhost:8080/abc123"}
```

**Redirect**
```
GET /{code}  →  302 redirect
```

**Stats**
```
GET /api/stats/{code}
→ {"short_code","long_url","clicks","created_at","expires_at","custom_alias"}
```

**Delete**
```
DELETE /api/{code}  →  {"deleted":"abc123"}
```

**Health**
```
GET /health  →  {"status":"ok","total_urls":N}
```

Example:
```bash
curl -X POST localhost:8080/api/shorten --data-urlencode "url=https://leetcode.com"
```

- Built a thread-safe URL shortener in C++17 from scratch: raw-socket HTTP
  server (no framework), base62 ID generation, custom aliases, and
  TTL-based expiration, backed by a flat-file store guarded by a
  `std::shared_mutex` reader-writer lock.
- Implemented a fixed-capacity, thread-safe LRU cache (doubly-linked list +
  hash map, O(1) get/put) from scratch to front the storage layer and cut
  disk I/O on hot-path redirects.
- Wrote an 11-case unit test suite with a custom lightweight test harness,
  including an 8-thread/1600-request concurrency test verifying zero
  short-code collisions under simultaneous writes.
