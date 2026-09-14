#ifndef CPPHTTP_MIME_HPP
#define CPPHTTP_MIME_HPP

#include <string>
#include <string_view>

namespace cpphttp {

std::string_view mime_for_path(std::string_view path);
const std::string& http_date();

}

#endif
