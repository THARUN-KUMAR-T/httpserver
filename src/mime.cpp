#include "mime.hpp"

#include <cstring>
#include <ctime>

namespace cpphttp {

namespace {

struct TypeEntry {
    std::string_view extension;
    std::string_view type;
};

constexpr TypeEntry kTypes[] = {
    {"html", "text/html; charset=utf-8"},
    {"htm", "text/html; charset=utf-8"},
    {"css", "text/css; charset=utf-8"},
    {"js", "application/javascript; charset=utf-8"},
    {"mjs", "application/javascript; charset=utf-8"},
    {"json", "application/json; charset=utf-8"},
    {"txt", "text/plain; charset=utf-8"},
    {"csv", "text/csv; charset=utf-8"},
    {"xml", "application/xml; charset=utf-8"},
    {"svg", "image/svg+xml"},
    {"png", "image/png"},
    {"jpg", "image/jpeg"},
    {"jpeg", "image/jpeg"},
    {"gif", "image/gif"},
    {"webp", "image/webp"},
    {"avif", "image/avif"},
    {"ico", "image/x-icon"},
    {"woff", "font/woff"},
    {"woff2", "font/woff2"},
    {"ttf", "font/ttf"},
    {"otf", "font/otf"},
    {"pdf", "application/pdf"},
    {"wasm", "application/wasm"},
    {"mp4", "video/mp4"},
    {"webm", "video/webm"},
    {"mp3", "audio/mpeg"},
    {"ogg", "audio/ogg"},
    {"zip", "application/zip"},
    {"gz", "application/gzip"},
};

bool ascii_iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        char ca = a[i];
        char cb = b[i];
        if (ca >= 'A' && ca <= 'Z') {
            ca = static_cast<char>(ca + 32);
        }
        if (ca != cb) {
            return false;
        }
    }
    return true;
}

}

std::string_view mime_for_path(std::string_view path) {
    std::size_t slash = path.find_last_of('/');
    std::size_t dot = path.find_last_of('.');
    if (dot == std::string_view::npos ||
        (slash != std::string_view::npos && dot < slash) ||
        dot + 1 >= path.size()) {
        return "application/octet-stream";
    }
    std::string_view extension = path.substr(dot + 1);
    for (const TypeEntry& entry : kTypes) {
        if (ascii_iequals(extension, entry.extension)) {
            return entry.type;
        }
    }
    return "application/octet-stream";
}

const std::string& http_date() {
    thread_local std::string cached;
    thread_local std::time_t cached_second = 0;
    std::time_t now = std::time(nullptr);
    if (now != cached_second || cached.empty()) {
        cached_second = now;
        std::tm parts{};
        gmtime_r(&now, &parts);
        char buffer[64];
        std::strftime(buffer, sizeof(buffer), "%a, %d %b %Y %H:%M:%S GMT", &parts);
        cached.assign(buffer);
    }
    return cached;
}

}
