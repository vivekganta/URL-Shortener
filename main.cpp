// A small HTTP/1.1 server built directly on POSIX sockets — no external
// HTTP or JSON library. Each connection is handled on its own thread, which
// exercises the UrlShortener's thread-safety. Request bodies are parsed as
// application/x-www-form-urlencoded (simple to hand-parse reliably, unlike
// general JSON), and responses are small hand-built JSON strings.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>

#include "url_shortener.hpp"

namespace {

std::string urlDecode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            int value = std::stoi(s.substr(i + 1, 2), nullptr, 16);
            out.push_back(static_cast<char>(value));
            i += 2;
        } else if (s[i] == '+') {
            out.push_back(' ');
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

std::unordered_map<std::string, std::string> parseFormBody(const std::string& body) {
    std::unordered_map<std::string, std::string> fields;
    std::stringstream ss(body);
    std::string pair;
    while (std::getline(ss, pair, '&')) {
        auto eq = pair.find('=');
        if (eq == std::string::npos) continue;
        fields[urlDecode(pair.substr(0, eq))] = urlDecode(pair.substr(eq + 1));
    }
    return fields;
}

// Minimal JSON string escaping: enough for URLs and codes, which is all we emit.
std::string jsonEscape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '"' || c == '\\') out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

struct HttpRequest {
    std::string method;
    std::string path;
    std::string body;
};

struct HttpResponse {
    int status = 200;
    std::string content_type = "application/json";
    std::string body;
    std::string location; // set for redirects
};

HttpRequest parseRequest(const std::string& raw) {
    HttpRequest req;
    std::istringstream stream(raw);
    std::string line;
    std::getline(stream, line);
    std::istringstream startLine(line);
    std::string httpVersion;
    startLine >> req.method >> req.path >> httpVersion;

    size_t content_length = 0;
    while (std::getline(stream, line) && line != "\r" && !line.empty()) {
        if (line.rfind("Content-Length:", 0) == 0 || line.rfind("content-length:", 0) == 0) {
            content_length = std::stoul(line.substr(line.find(':') + 1));
        }
    }
    if (content_length > 0) {
        std::string body(content_length, '\0');
        stream.read(&body[0], content_length);
        req.body = body;
    }
    return req;
}

std::string statusText(int status) {
    switch (status) {
        case 200: return "OK";
        case 302: return "Found";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 409: return "Conflict";
        default: return "Internal Server Error";
    }
}

std::string serialize(const HttpResponse& res) {
    std::ostringstream out;
    out << "HTTP/1.1 " << res.status << " " << statusText(res.status) << "\r\n";
    out << "Content-Type: " << res.content_type << "\r\n";
    out << "Content-Length: " << res.body.size() << "\r\n";
    if (!res.location.empty()) out << "Location: " << res.location << "\r\n";
    out << "Connection: close\r\n\r\n";
    out << res.body;
    return out.str();
}

HttpResponse errorResponse(int status, const std::string& message) {
    HttpResponse res;
    res.status = status;
    res.body = "{\"error\":\"" + jsonEscape(message) + "\"}";
    return res;
}

// Extracts the path segment after a known prefix, e.g. "/api/stats/abc" -> "abc".
std::string pathSuffix(const std::string& path, const std::string& prefix) {
    if (path.rfind(prefix, 0) != 0) return "";
    return path.substr(prefix.size());
}

bool isValidCode(const std::string& code) {
    if (code.empty() || code.size() > 32) return false;
    for (char c : code) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_') return false;
    }
    return true;
}

HttpResponse route(UrlShortener& shortener, const std::string& base_url, const HttpRequest& req) {
    // POST /api/shorten
    if (req.method == "POST" && req.path == "/api/shorten") {
        auto fields = parseFormBody(req.body);
        std::string url = fields.count("url") ? fields["url"] : "";
        std::string alias = fields.count("custom_alias") ? fields["custom_alias"] : "";
        int64_t ttl = fields.count("ttl_seconds") ? std::stoll(fields["ttl_seconds"]) : 0;

        auto result = shortener.shorten(url, alias, ttl);
        switch (result.error) {
            case ShortenError::InvalidUrl:
                return errorResponse(400, "invalid url; must start with http:// or https://");
            case ShortenError::InvalidAlias:
                return errorResponse(400, "alias must be 3-32 chars: letters, digits, - or _");
            case ShortenError::AliasTaken:
                return errorResponse(409, "custom alias already in use");
            case ShortenError::None:
                break;
        }
        HttpResponse res;
        res.body = "{\"short_code\":\"" + jsonEscape(result.short_code) + "\",\"short_url\":\"" +
                    jsonEscape(base_url + "/" + result.short_code) + "\"}";
        return res;
    }

    // GET /health
    if (req.method == "GET" && req.path == "/health") {
        HttpResponse res;
        res.body = "{\"status\":\"ok\",\"total_urls\":" + std::to_string(shortener.totalUrls()) + "}";
        return res;
    }

    // GET /api/stats/{code}
    if (req.method == "GET" && req.path.rfind("/api/stats/", 0) == 0) {
        std::string code = pathSuffix(req.path, "/api/stats/");
        if (!isValidCode(code)) return errorResponse(404, "short code not found or expired");
        auto record = shortener.stats(code);
        if (!record) return errorResponse(404, "short code not found or expired");
        HttpResponse res;
        std::ostringstream body;
        body << "{\"short_code\":\"" << jsonEscape(record->short_code)
             << "\",\"long_url\":\"" << jsonEscape(record->long_url)
             << "\",\"clicks\":" << record->clicks
             << ",\"created_at\":" << record->created_at
             << ",\"expires_at\":" << record->expires_at
             << ",\"custom_alias\":" << (record->custom_alias ? "true" : "false") << "}";
        res.body = body.str();
        return res;
    }

    // DELETE /api/{code}
    if (req.method == "DELETE" && req.path.rfind("/api/", 0) == 0) {
        std::string code = pathSuffix(req.path, "/api/");
        if (!isValidCode(code) || !shortener.remove(code)) return errorResponse(404, "short code not found");
        HttpResponse res;
        res.body = "{\"deleted\":\"" + jsonEscape(code) + "\"}";
        return res;
    }

    // GET /{code} -> redirect (must come after /health and /api/* above)
    if (req.method == "GET" && req.path.size() > 1) {
        std::string code = req.path.substr(1);
        if (isValidCode(code)) {
            auto target = shortener.resolve(code);
            if (target) {
                HttpResponse res;
                res.status = 302;
                res.location = *target;
                res.body = "";
                return res;
            }
        }
        return errorResponse(404, "short code not found or expired");
    }

    return errorResponse(404, "not found");
}

void handleConnection(int client_fd, UrlShortener* shortener, std::string base_url) {
    char buffer[8192];
    ssize_t n = read(client_fd, buffer, sizeof(buffer) - 1);
    if (n > 0) {
        buffer[n] = '\0';
        HttpRequest req = parseRequest(std::string(buffer, n));
        HttpResponse res = route(*shortener, base_url, req);
        std::string raw = serialize(res);
        write(client_fd, raw.data(), raw.size());
    }
    close(client_fd);
}

} // namespace

int main(int argc, char** argv) {
    int port = 8080;
    std::string data_file = "data/urls.txt";
    std::string base_url = "http://localhost:8080";
    if (argc > 1) port = std::atoi(argv[1]);
    if (argc > 2) data_file = argv[2];
    if (argc > 3) base_url = argv[3];

    UrlShortener shortener(data_file, /*cache_capacity=*/2048);

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { std::cerr << "socket() failed\n"; return 1; }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);

    if (bind(server_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        std::cerr << "bind() failed on port " << port << "\n";
        return 1;
    }
    if (listen(server_fd, 64) < 0) { std::cerr << "listen() failed\n"; return 1; }

    std::cout << "URL shortener listening on port " << port << ", data file: " << data_file << std::endl;

    while (true) {
        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, reinterpret_cast<sockaddr*>(&client_addr), &client_len);
        if (client_fd < 0) continue;
        std::thread(handleConnection, client_fd, &shortener, base_url).detach();
    }
}
