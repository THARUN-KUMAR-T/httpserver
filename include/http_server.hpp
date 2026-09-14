#ifndef CPPHTTP_SERVER_HPP
#define CPPHTTP_SERVER_HPP

#include <string>

namespace cpphttp {

struct ServerConfig {
    int port = 8000;
    std::string root = "public";
    int threads = 0;
    int backlog = 4096;
    int keep_alive_timeout_ms = 15000;
    bool keep_alive = true;
};

int run_server(const ServerConfig& config);

}

#endif
