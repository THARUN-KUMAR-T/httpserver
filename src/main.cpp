#include "http_server.hpp"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

namespace {

void print_usage(const char* program) {
    std::cout << "usage: " << program << " [options]\n"
              << "  --port <n>          listening port (default 8000)\n"
              << "  --root <dir>        document root (default public)\n"
              << "  --threads <n>       worker event loops (default: cpu cores)\n"
              << "  --backlog <n>       listen backlog (default 4096)\n"
              << "  --keepalive <secs>  keep-alive idle timeout (default 15)\n"
              << "  --no-keepalive      disable persistent connections\n"
              << "  --help              show this message\n";
}

int parse_int(const char* value, int fallback) {
    if (value == nullptr || *value == '\0') {
        return fallback;
    }
    char* end = nullptr;
    long parsed = std::strtol(value, &end, 10);
    if (end == value || *end != '\0') {
        return fallback;
    }
    return static_cast<int>(parsed);
}

}

int main(int argc, char** argv) {
    cpphttp::ServerConfig config;

    if (const char* env = std::getenv("HTTP_PORT")) {
        config.port = parse_int(env, config.port);
    }
    if (const char* env = std::getenv("HTTP_ROOT")) {
        config.root = env;
    }
    if (const char* env = std::getenv("HTTP_THREADS")) {
        config.threads = parse_int(env, config.threads);
    }
    if (const char* env = std::getenv("HTTP_BACKLOG")) {
        config.backlog = parse_int(env, config.backlog);
    }
    if (const char* env = std::getenv("HTTP_KEEPALIVE")) {
        config.keep_alive_timeout_ms = parse_int(env, 15) * 1000;
    }

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto next = [&](int& value) {
            if (i + 1 < argc) {
                value = parse_int(argv[++i], value);
            }
        };
        if (arg == "--port") {
            next(config.port);
        } else if (arg == "--root" && i + 1 < argc) {
            config.root = argv[++i];
        } else if (arg == "--threads") {
            next(config.threads);
        } else if (arg == "--backlog") {
            next(config.backlog);
        } else if (arg == "--keepalive") {
            int seconds = config.keep_alive_timeout_ms / 1000;
            next(seconds);
            config.keep_alive_timeout_ms = seconds * 1000;
        } else if (arg == "--no-keepalive") {
            config.keep_alive = false;
        } else if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return 0;
        } else {
            std::cerr << "unknown option: " << arg << "\n";
            print_usage(argv[0]);
            return 2;
        }
    }

    return cpphttp::run_server(config);
}
