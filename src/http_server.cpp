#include "http_server.hpp"

#include "http_parser.hpp"
#include "mime.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace cpphttp {

namespace {

constexpr int kMaxEvents = 1024;
constexpr std::size_t kReadChunk = 64 * 1024;
constexpr std::size_t kMaxRequestBytes = 128 * 1024 * 1024;

volatile std::sig_atomic_t g_running = 1;

void on_signal(int) {
    g_running = 0;
}

int hex_value(char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

bool url_decode(std::string_view input, std::string& output) {
    output.clear();
    output.reserve(input.size());
    for (std::size_t i = 0; i < input.size(); ++i) {
        char ch = input[i];
        if (ch != '%') {
            output.push_back(ch);
            continue;
        }
        if (i + 2 >= input.size()) {
            return false;
        }
        int high = hex_value(input[i + 1]);
        int low = hex_value(input[i + 2]);
        if (high < 0 || low < 0) {
            return false;
        }
        char decoded = static_cast<char>((high << 4) | low);
        if (decoded == '\0') {
            return false;
        }
        output.push_back(decoded);
        i += 2;
    }
    return true;
}

bool resolve_path(const std::string& root, std::string_view target, std::string& output) {
    std::string decoded;
    if (!url_decode(target, decoded)) {
        return false;
    }
    if (decoded.empty() || decoded[0] != '/') {
        return false;
    }

    std::string normalized;
    normalized.reserve(decoded.size());
    std::size_t index = 0;
    while (index < decoded.size()) {
        while (index < decoded.size() && decoded[index] == '/') {
            ++index;
        }
        std::size_t begin = index;
        while (index < decoded.size() && decoded[index] != '/') {
            ++index;
        }
        if (begin == index) {
            break;
        }
        std::string_view segment(decoded.data() + begin, index - begin);
        if (segment == "..") {
            return false;
        }
        if (segment == ".") {
            continue;
        }
        normalized.push_back('/');
        normalized.append(segment.data(), segment.size());
    }
    if (normalized.empty()) {
        normalized = "/";
    }

    output = root;
    while (!output.empty() && output.back() == '/') {
        output.pop_back();
    }
    output += normalized;
    return true;
}

int status_for_parse_error(ParseStatus status) {
    switch (status) {
        case ParseStatus::UriTooLong: return 414;
        case ParseStatus::HeadersTooLarge: return 431;
        case ParseStatus::BodyTooLarge: return 413;
        case ParseStatus::LengthRequired: return 411;
        case ParseStatus::Unsupported: return 505;
        default: return 400;
    }
}

int make_listener(int port, int backlog, bool want_reuseport, bool& reuseport_ok) {
    reuseport_ok = false;
    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -1;
    }
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (want_reuseport) {
#ifdef SO_REUSEPORT
        reuseport_ok = (::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one)) == 0);
#endif
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(static_cast<std::uint16_t>(port));

    if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        ::close(fd);
        return -1;
    }
    if (::listen(fd, backlog) < 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

struct Connection {
    int fd = -1;
    std::string in;
    std::string out;
    std::size_t out_sent = 0;
    int file_fd = -1;
    off_t file_offset = 0;
    std::size_t file_remaining = 0;
    bool close_after = false;
    bool eof = false;
    std::chrono::steady_clock::time_point last_active;
};

enum class IoAction { Keep, Close };

class Worker {
public:
    Worker(const ServerConfig& config, int listener) : config_(config), listener_(listener) {}

    void run() {
        epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
        if (epoll_fd_ < 0) {
            return;
        }
        epoll_event event{};
        event.events = EPOLLIN;
        event.data.fd = listener_;
        ::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, listener_, &event);

        std::vector<epoll_event> events(kMaxEvents);
        auto next_sweep = std::chrono::steady_clock::now() + std::chrono::seconds(1);

        while (g_running != 0) {
            int ready = ::epoll_wait(epoll_fd_, events.data(), kMaxEvents, 200);
            if (ready < 0) {
                if (errno == EINTR) {
                    continue;
                }
                break;
            }
            for (int i = 0; i < ready; ++i) {
                int fd = events[i].data.fd;
                if (fd == listener_) {
                    accept_ready();
                    continue;
                }
                auto it = connections_.find(fd);
                if (it == connections_.end()) {
                    continue;
                }
                handle_event(*it->second, events[i].events);
            }
            auto now = std::chrono::steady_clock::now();
            if (now >= next_sweep) {
                sweep_idle();
                next_sweep = now + std::chrono::seconds(1);
            }
        }

        for (auto& entry : connections_) {
            Connection& conn = *entry.second;
            if (conn.file_fd >= 0) {
                ::close(conn.file_fd);
            }
            ::close(conn.fd);
        }
        connections_.clear();
        ::close(epoll_fd_);
    }

private:
    void accept_ready() {
        for (;;) {
            sockaddr_in address{};
            socklen_t length = sizeof(address);
            int fd = ::accept4(listener_, reinterpret_cast<sockaddr*>(&address), &length,
                               SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (fd < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                }
                if (errno == EINTR || errno == ECONNABORTED) {
                    continue;
                }
                break;
            }
            int one = 1;
            ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

            auto conn = std::make_unique<Connection>();
            conn->fd = fd;
            conn->last_active = std::chrono::steady_clock::now();

            epoll_event event{};
            event.events = EPOLLIN;
            event.data.fd = fd;
            if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &event) < 0) {
                ::close(fd);
                continue;
            }
            connections_[fd] = std::move(conn);
        }
    }

    void handle_event(Connection& conn, std::uint32_t events) {
        if (events & EPOLLERR) {
            remove_connection(conn.fd);
            return;
        }
        if (events & EPOLLHUP) {
            conn.eof = true;
        }
        if (events & EPOLLIN) {
            read_available(conn);
        }
        if (advance(conn) == IoAction::Close) {
            remove_connection(conn.fd);
        }
    }

    void read_available(Connection& conn) {
        char buffer[kReadChunk];
        for (;;) {
            ssize_t received = ::recv(conn.fd, buffer, sizeof(buffer), 0);
            if (received > 0) {
                conn.in.append(buffer, static_cast<std::size_t>(received));
                if (conn.in.size() > kMaxRequestBytes) {
                    conn.eof = true;
                    return;
                }
                continue;
            }
            if (received == 0) {
                conn.eof = true;
                return;
            }
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return;
            }
            conn.eof = true;
            return;
        }
    }

    IoAction advance(Connection& conn) {
        for (;;) {
            if (has_pending_output(conn)) {
                bool error = false;
                bool flushed = flush(conn, error);
                if (error) {
                    return IoAction::Close;
                }
                if (!flushed) {
                    refresh_interest(conn);
                    return IoAction::Keep;
                }
            }

            if (conn.close_after) {
                return IoAction::Close;
            }

            if (conn.in.empty()) {
                if (conn.eof) {
                    return IoAction::Close;
                }
                refresh_interest(conn);
                return IoAction::Keep;
            }

            Request request;
            std::size_t consumed = 0;
            ParseStatus status = parse_request(conn.in, request, consumed);
            if (status == ParseStatus::NeedMore) {
                if (conn.eof) {
                    return IoAction::Close;
                }
                refresh_interest(conn);
                return IoAction::Keep;
            }

            conn.last_active = std::chrono::steady_clock::now();

            if (status != ParseStatus::Complete) {
                conn.close_after = true;
                enqueue_error(conn, status_for_parse_error(status), false, false, nullptr);
                conn.in.clear();
                continue;
            }

            if (!config_.keep_alive || !request.keep_alive) {
                conn.close_after = true;
            }

            if (request.expect_continue && !request.body.empty()) {
                conn.out += "HTTP/1.1 100 Continue\r\n\r\n";
            }

            std::string_view target = request.target;
            handle_request(conn, request, target);
            conn.in.erase(0, consumed);
        }
    }

    bool flush(Connection& conn, bool& error) {
        error = false;
        while (conn.out_sent < conn.out.size()) {
            ssize_t sent = ::send(conn.fd, conn.out.data() + conn.out_sent,
                                  conn.out.size() - conn.out_sent, MSG_NOSIGNAL);
            if (sent > 0) {
                conn.out_sent += static_cast<std::size_t>(sent);
                continue;
            }
            if (sent < 0 && errno == EINTR) {
                continue;
            }
            if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                return false;
            }
            error = true;
            return false;
        }
        conn.out.clear();
        conn.out_sent = 0;

        while (conn.file_remaining > 0) {
            std::size_t chunk = std::min<std::size_t>(conn.file_remaining, 1U << 20);
            off_t offset = conn.file_offset;
            ssize_t sent = ::sendfile(conn.fd, conn.file_fd, &offset, chunk);
            if (sent > 0) {
                conn.file_offset = offset;
                conn.file_remaining -= static_cast<std::size_t>(sent);
                continue;
            }
            if (sent < 0 && errno == EINTR) {
                continue;
            }
            if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                return false;
            }
            error = true;
            return false;
        }
        if (conn.file_fd >= 0) {
            ::close(conn.file_fd);
            conn.file_fd = -1;
            conn.file_offset = 0;
        }
        conn.last_active = std::chrono::steady_clock::now();
        return true;
    }

    bool has_pending_output(const Connection& conn) const {
        return conn.out_sent < conn.out.size() || conn.file_remaining > 0;
    }

    void refresh_interest(Connection& conn) {
        std::uint32_t want = 0;
        if (!conn.close_after) {
            want |= EPOLLIN;
        }
        if (has_pending_output(conn)) {
            want |= EPOLLOUT;
        }
        if (want == 0) {
            return;
        }
        epoll_event event{};
        event.events = want;
        event.data.fd = conn.fd;
        ::epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, conn.fd, &event);
    }

    void handle_request(Connection& conn, const Request& request, std::string_view target) {
        if (request.method == "GET" || request.method == "HEAD") {
            serve_static(conn, request, target);
        } else if (request.method == "PUT") {
            serve_put(conn, request, target);
        } else {
            conn.close_after = true;
            enqueue_error(conn, 405, request.head, false, "GET, HEAD, PUT");
        }
    }

    void serve_static(Connection& conn, const Request& request, std::string_view target) {
        std::string_view path = target;
        std::size_t query = path.find('?');
        if (query != std::string_view::npos) {
            path = path.substr(0, query);
        }

        std::string full;
        if (!resolve_path(config_.root, path, full)) {
            enqueue_error(conn, 403, request.head, !conn.close_after, nullptr);
            return;
        }

        struct stat info {};
        bool is_file = (::stat(full.c_str(), &info) == 0 && S_ISREG(info.st_mode));
        if (!is_file) {
            std::string index = full;
            if (index.empty() || index.back() != '/') {
                index += '/';
            }
            index += "index.html";
            if (::stat(index.c_str(), &info) == 0 && S_ISREG(info.st_mode)) {
                full = index;
                is_file = true;
            }
        }
        if (!is_file) {
            enqueue_error(conn, 404, request.head, !conn.close_after, nullptr);
            return;
        }

        int fd = ::open(full.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            enqueue_error(conn, 404, request.head, !conn.close_after, nullptr);
            return;
        }

        struct stat opened {};
        if (::fstat(fd, &opened) != 0 || !S_ISREG(opened.st_mode)) {
            ::close(fd);
            enqueue_error(conn, 404, request.head, !conn.close_after, nullptr);
            return;
        }

        std::size_t size = static_cast<std::size_t>(opened.st_size);
        bool keep_alive = !conn.close_after;

        append_status_line(conn.out, 200);
        append_headers(conn.out, size, mime_for_path(full), keep_alive);
        conn.out += "\r\n";

        if (request.head || size == 0) {
            ::close(fd);
        } else {
            conn.file_fd = fd;
            conn.file_offset = 0;
            conn.file_remaining = size;
        }
    }

    void serve_put(Connection& conn, const Request& request, std::string_view target) {
        if (!request.has_content_length) {
            conn.close_after = true;
            enqueue_error(conn, 411, request.head, false, nullptr);
            return;
        }
        std::string_view path = target;
        std::size_t query = path.find('?');
        if (query != std::string_view::npos) {
            path = path.substr(0, query);
        }

        std::string full;
        if (!resolve_path(config_.root, path, full)) {
            conn.close_after = true;
            enqueue_error(conn, 403, request.head, false, nullptr);
            return;
        }

        int fd = ::open(full.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (fd < 0) {
            conn.close_after = true;
            enqueue_error(conn, 500, request.head, false, nullptr);
            return;
        }

        std::size_t written = 0;
        while (written < request.body.size()) {
            ssize_t sent = ::write(fd, request.body.data() + written, request.body.size() - written);
            if (sent > 0) {
                written += static_cast<std::size_t>(sent);
                continue;
            }
            if (sent < 0 && errno == EINTR) {
                continue;
            }
            break;
        }
        ::close(fd);

        if (written != request.body.size()) {
            conn.close_after = true;
            enqueue_error(conn, 500, request.head, false, nullptr);
            return;
        }

        append_status_line(conn.out, 201);
        append_headers(conn.out, 0, std::string_view("text/plain; charset=utf-8"), !conn.close_after);
        conn.out += "\r\n";
    }

    void enqueue_error(Connection& conn, int status, bool head, bool keep_alive, const char* allow) {
        std::string body;
        body += "<!doctype html><html><head><meta charset=\"utf-8\"><title>";
        body += std::to_string(status);
        body += ' ';
        body += reason_phrase(status);
        body += "</title></head><body><h1>";
        body += std::to_string(status);
        body += ' ';
        body += reason_phrase(status);
        body += "</h1></body></html>\n";

        append_status_line(conn.out, status);
        append_headers(conn.out, body.size(), std::string_view("text/html; charset=utf-8"), keep_alive);
        if (allow != nullptr) {
            conn.out += "Allow: ";
            conn.out += allow;
            conn.out += "\r\n";
        }
        conn.out += "\r\n";
        if (!head) {
            conn.out += body;
        }
    }

    void append_status_line(std::string& out, int status) {
        out += "HTTP/1.1 ";
        out += std::to_string(status);
        out += ' ';
        out += reason_phrase(status);
        out += "\r\n";
    }

    void append_headers(std::string& out, std::size_t length, std::string_view content_type, bool keep_alive) {
        out += "Date: ";
        out += http_date();
        out += "\r\n";
        out += "Server: cpp-httpd/2.0\r\n";
        out += "Content-Length: ";
        out += std::to_string(length);
        out += "\r\n";
        out += "Content-Type: ";
        out.append(content_type.data(), content_type.size());
        out += "\r\n";
        if (keep_alive) {
            out += "Connection: keep-alive\r\n";
            out += "Keep-Alive: timeout=";
            out += std::to_string(config_.keep_alive_timeout_ms / 1000);
            out += "\r\n";
        } else {
            out += "Connection: close\r\n";
        }
    }

    void sweep_idle() {
        auto now = std::chrono::steady_clock::now();
        auto limit = std::chrono::milliseconds(config_.keep_alive_timeout_ms);
        for (auto it = connections_.begin(); it != connections_.end();) {
            Connection& conn = *it->second;
            bool idle = (now - conn.last_active) > limit;
            if (idle && !has_pending_output(conn)) {
                if (conn.file_fd >= 0) {
                    ::close(conn.file_fd);
                }
                ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, conn.fd, nullptr);
                ::close(conn.fd);
                it = connections_.erase(it);
            } else {
                ++it;
            }
        }
    }

    void remove_connection(int fd) {
        auto it = connections_.find(fd);
        if (it == connections_.end()) {
            return;
        }
        Connection& conn = *it->second;
        ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
        if (conn.file_fd >= 0) {
            ::close(conn.file_fd);
        }
        ::close(fd);
        connections_.erase(it);
    }

    const ServerConfig& config_;
    int listener_;
    int epoll_fd_ = -1;
    std::unordered_map<int, std::unique_ptr<Connection>> connections_;
};

}

int run_server(const ServerConfig& config) {
    std::signal(SIGPIPE, SIG_IGN);

    struct sigaction action {};
    action.sa_handler = on_signal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    sigaction(SIGINT, &action, nullptr);
    sigaction(SIGTERM, &action, nullptr);

    int thread_count = config.threads > 0 ? config.threads
                                          : static_cast<int>(std::thread::hardware_concurrency());
    if (thread_count <= 0) {
        thread_count = 1;
    }

    std::vector<int> listeners;
    std::vector<int> unique_listeners;
    bool reuseport = true;
    for (int i = 0; i < thread_count; ++i) {
        bool ok = false;
        int fd = make_listener(config.port, config.backlog, true, ok);
        if (fd < 0 || !ok) {
            if (fd >= 0) {
                ::close(fd);
            }
            reuseport = false;
            break;
        }
        listeners.push_back(fd);
        unique_listeners.push_back(fd);
    }

    if (!reuseport) {
        for (int fd : unique_listeners) {
            ::close(fd);
        }
        unique_listeners.clear();
        listeners.clear();
        bool ok = false;
        int fd = make_listener(config.port, config.backlog, false, ok);
        if (fd < 0) {
            std::perror("listen");
            return 1;
        }
        unique_listeners.push_back(fd);
        listeners.assign(thread_count, fd);
    }

    std::printf("cpp-httpd listening on 0.0.0.0:%d root=%s workers=%d keepalive=%s\n",
                config.port, config.root.c_str(), thread_count,
                config.keep_alive ? "on" : "off");
    std::fflush(stdout);

    std::vector<std::thread> pool;
    pool.reserve(thread_count);
    for (int i = 0; i < thread_count; ++i) {
        pool.emplace_back([this_config = config, listener = listeners[i]]() {
            Worker worker(this_config, listener);
            worker.run();
        });
    }
    for (std::thread& thread : pool) {
        thread.join();
    }
    for (int fd : unique_listeners) {
        ::close(fd);
    }
    return 0;
}

}
