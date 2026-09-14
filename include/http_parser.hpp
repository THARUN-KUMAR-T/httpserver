#ifndef CPPHTTP_HTTP_PARSER_HPP
#define CPPHTTP_HTTP_PARSER_HPP

#include <cstddef>
#include <string>
#include <string_view>

namespace cpphttp {

enum class ParseStatus {
    NeedMore,
    Complete,
    BadRequest,
    UriTooLong,
    HeadersTooLarge,
    BodyTooLarge,
    LengthRequired,
    Unsupported
};

struct Request {
    std::string method;
    std::string target;
    std::string version;
    std::string content_type;
    std::string_view body;
    std::size_t content_length = 0;
    bool has_content_length = false;
    bool keep_alive = true;
    bool expect_continue = false;
    bool head = false;
};

ParseStatus parse_request(std::string_view data, Request& request, std::size_t& consumed);
const char* reason_phrase(int status);

}

#endif
