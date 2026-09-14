#include "http_parser.hpp"

#include <cctype>

namespace cpphttp {

namespace {

constexpr std::size_t kMaxTargetLength = 8192;
constexpr std::size_t kMaxHeaderBytes = 64 * 1024;
constexpr std::size_t kMaxBodyBytes = 64 * 1024 * 1024;

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        unsigned char ca = static_cast<unsigned char>(a[i]);
        unsigned char cb = static_cast<unsigned char>(b[i]);
        if (std::tolower(ca) != std::tolower(cb)) {
            return false;
        }
    }
    return true;
}

std::string_view trim(std::string_view value) {
    std::size_t begin = 0;
    std::size_t end = value.size();
    while (begin < end && (value[begin] == ' ' || value[begin] == '\t')) {
        ++begin;
    }
    while (end > begin && (value[end - 1] == ' ' || value[end - 1] == '\t' || value[end - 1] == '\r')) {
        --end;
    }
    return value.substr(begin, end - begin);
}

bool contains_token(std::string_view value, std::string_view token) {
    std::size_t pos = 0;
    while (pos <= value.size()) {
        std::size_t comma = value.find(',', pos);
        std::string_view piece = (comma == std::string_view::npos)
            ? value.substr(pos)
            : value.substr(pos, comma - pos);
        if (iequals(trim(piece), token)) {
            return true;
        }
        if (comma == std::string_view::npos) {
            return false;
        }
        pos = comma + 1;
    }
    return false;
}

bool parse_decimal(std::string_view value, std::size_t& out) {
    value = trim(value);
    if (value.empty()) {
        return false;
    }
    std::size_t result = 0;
    for (char ch : value) {
        if (ch < '0' || ch > '9') {
            return false;
        }
        std::size_t digit = static_cast<std::size_t>(ch - '0');
        if (result > (static_cast<std::size_t>(-1) - digit) / 10) {
            return false;
        }
        result = result * 10 + digit;
    }
    out = result;
    return true;
}

}

ParseStatus parse_request(std::string_view data, Request& request, std::size_t& consumed) {
    consumed = 0;

    std::size_t header_end = data.find("\r\n\r\n");
    if (header_end == std::string_view::npos) {
        if (data.size() > kMaxHeaderBytes) {
            return ParseStatus::HeadersTooLarge;
        }
        return ParseStatus::NeedMore;
    }

    std::size_t header_bytes = header_end + 4;
    std::string_view head = data.substr(0, header_end);

    std::size_t request_line_end = head.find("\r\n");
    if (request_line_end == std::string_view::npos) {
        return ParseStatus::BadRequest;
    }
    std::string_view request_line = head.substr(0, request_line_end);

    std::size_t first_space = request_line.find(' ');
    if (first_space == std::string_view::npos) {
        return ParseStatus::BadRequest;
    }
    std::size_t second_space = request_line.find(' ', first_space + 1);
    if (second_space == std::string_view::npos) {
        return ParseStatus::BadRequest;
    }

    request.method.assign(request_line.substr(0, first_space));
    request.target.assign(request_line.substr(first_space + 1, second_space - first_space - 1));
    request.version.assign(request_line.substr(second_space + 1));

    if (request.method.empty() || request.version.empty()) {
        return ParseStatus::BadRequest;
    }
    if (request.target.size() > kMaxTargetLength) {
        return ParseStatus::UriTooLong;
    }

    if (request.version == "HTTP/1.1") {
        request.keep_alive = true;
    } else if (request.version == "HTTP/1.0") {
        request.keep_alive = false;
    } else {
        return ParseStatus::Unsupported;
    }

    request.head = (request.method == "HEAD");

    std::size_t pos = request_line_end + 2;
    while (pos < head.size()) {
        std::size_t line_end = head.find("\r\n", pos);
        if (line_end == std::string_view::npos) {
            line_end = head.size();
        }
        std::string_view line = head.substr(pos, line_end - pos);
        pos = line_end + 2;
        if (line.empty()) {
            continue;
        }
        std::size_t colon = line.find(':');
        if (colon == std::string_view::npos) {
            continue;
        }
        std::string_view name = trim(line.substr(0, colon));
        std::string_view value = trim(line.substr(colon + 1));
        if (name.empty()) {
            continue;
        }
        if (iequals(name, "Content-Length")) {
            std::size_t length = 0;
            if (!parse_decimal(value, length)) {
                return ParseStatus::BadRequest;
            }
            request.content_length = length;
            request.has_content_length = true;
        } else if (iequals(name, "Content-Type")) {
            request.content_type.assign(value);
        } else if (iequals(name, "Connection")) {
            if (contains_token(value, "close")) {
                request.keep_alive = false;
            } else if (contains_token(value, "keep-alive")) {
                request.keep_alive = true;
            }
        } else if (iequals(name, "Expect")) {
            if (contains_token(value, "100-continue")) {
                request.expect_continue = true;
            }
        }
    }

    if (request.content_length > kMaxBodyBytes) {
        return ParseStatus::BodyTooLarge;
    }

    std::size_t total = header_bytes + request.content_length;
    if (data.size() < total) {
        return ParseStatus::NeedMore;
    }

    request.body = data.substr(header_bytes, request.content_length);
    consumed = total;
    return ParseStatus::Complete;
}

const char* reason_phrase(int status) {
    switch (status) {
        case 100: return "Continue";
        case 200: return "OK";
        case 201: return "Created";
        case 204: return "No Content";
        case 301: return "Moved Permanently";
        case 304: return "Not Modified";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 408: return "Request Timeout";
        case 411: return "Length Required";
        case 413: return "Payload Too Large";
        case 414: return "URI Too Long";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 505: return "HTTP Version Not Supported";
        default: return "Unknown";
    }
}

}
