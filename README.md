# cpp-httpd

A small, dependency-free HTTP/1.1 server written in C++17 for Linux. It serves
static files, accepts `PUT` uploads, keeps connections alive, and scales across
CPU cores with one `epoll` event loop per worker.

It is designed for correctness under real load: bounded concurrency, persistent
connections, fully non-blocking I/O, and a parser that never allocates a string
for every character.

---

## TL;DR

| | |
|---|---|
| Model | N worker threads, each with its own `epoll` loop and `SO_REUSEPORT` listener |
| I/O | Fully non-blocking; level-triggered `epoll`; `sendfile()` for file bodies |
| Keep-alive | HTTP/1.1 persistent connections with an idle timeout |
| Parsing | Buffer-oriented request parsing over `std::string_view`, no per-char concatenation |
| Throughput | ~132k req/s keep-alive at 50 connections (ab); ~409k req/s at 500 connections (wrk) |

---

## Design principles

- **Bounded concurrency** — a fixed pool of event loops, one per core, each with its own `SO_REUSEPORT` listener, so workers never share an accept queue or a lock.
- **Non-blocking I/O** — `SOCK_NONBLOCK` + `accept4()`, `TCP_NODELAY`, level-triggered `epoll`, and `sendfile()` for file bodies.
- **Persistent connections** — HTTP/1.1 keep-alive with an idle timeout and clean `Connection: close` handling.
- **Lean parsing** — buffer-oriented `string_view` scans with zero-copy header fields and explicit size limits.
- **Safe by default** — bounds-checked parsing, `..` traversal rejection, body/header limits, and graceful signal handling.

---

## Architecture

### Concurrency model

At startup the process determines a worker count (CPU cores by default) and each
worker binds its **own** listening socket with `SO_REUSEADDR | SO_REUSEPORT`.
The kernel then load-balances incoming connections across the listeners, so
workers never share an accept queue and never need a lock. Each worker runs:

```
while running:
    epoll_wait(timeout = 200ms)
    for each ready fd:
        if fd == listener:  accept4() until EAGAIN
        else:               drive the connection state machine
    once per second:        close idle keep-alive connections
```

If `SO_REUSEPORT` is unavailable the server falls back to a single shared
listener that all workers poll.

### Connection state machine

Every accepted socket is wrapped in a `Connection`:

- input buffer (`std::string`) collecting bytes until a full request is present
- output buffer plus a descriptor/offset for an in-flight file body
- `keep_alive`, `close_after`, `eof`, and `last_active` bookkeeping

`advance()` is the only place that makes progress. It loops:

1. If output is pending, flush it. `EAGAIN` means "come back when writable".
2. If the peer asked to close and the buffer is drained, close.
3. Otherwise parse as many complete requests as the input buffer holds,
   generate responses, and repeat.

Because output is drained before the next request is parsed, pipelined requests
are answered strictly in order without unbounded buffering.

### Request parsing

Parsing never builds an intermediate representation. The parser locates the
`\r\n\r\n` terminator, splits the request line on the first two spaces, and walks
the header block comparing names case-insensitively. Only the method, target,
version, content type, and body view are retained. It understands:

- `Connection: close` / `keep-alive` token lists
- HTTP/1.1 default keep-alive, HTTP/1.0 default close
- `Content-Length` (with overflow checks) and `Content-Type`
- `Expect: 100-continue`

Malformed input maps to a precise status (`400`, `413`, `414`, `431`, `505`)
instead of crashing.

### Static file serving

A request path is percent-decoded, normalized segment by segment, and rejected
if it contains `..`. The resolved path is `stat`-ed; directories fall back to
`index.html`, and missing files return `404`. Regular files are opened and their
body is streamed with `sendfile()`, so file bytes move from the page cache to the
socket without a copy into user space. `HEAD` sends identical headers with no body.

Responses carry `Date`, `Server`, `Content-Length`, `Content-Type`, and the
appropriate `Connection`/`Keep-Alive` headers. The `Date` value is cached and
refreshed at most once per second per worker.

### Uploads

`PUT` writes the request body to a path under the document root (created with
`0644`), returning `201 Created`. Requests without `Content-Length` get `411`,
and I/O failures get `500`.

---

## Build

Requirements: Linux, a C++17 compiler (GCC 9+ or Clang 10+), and `make`.

```bash
git clone <this-repo> cpp-httpd
cd cpp-httpd
make
```

The binary is written to `build/httpd`. `make clean` removes it.

## Run

```bash
./build/httpd --port 8000 --root public
```

Then visit `http://127.0.0.1:8000/`. The server prints its effective settings on
startup and shuts down cleanly on `Ctrl-C`.

## Configuration

Command-line flags override the environment, which overrides the defaults.

| Flag | Environment | Default | Meaning |
|---|---|---|---|
| `--port N` | `HTTP_PORT` | `8000` | Listening port |
| `--root DIR` | `HTTP_ROOT` | `public` | Document root |
| `--threads N` | `HTTP_THREADS` | CPU cores | Event loops / workers |
| `--backlog N` | `HTTP_BACKLOG` | `4096` | `listen()` backlog |
| `--keepalive SECS` | `HTTP_KEEPALIVE` | `15` | Idle keep-alive timeout |
| `--no-keepalive` | — | off | Force `Connection: close` on every response |
| `--help` | — | — | Usage text |

## Behaviour reference

| Method | Result |
|---|---|
| `GET` | `200` file, or `404`/`403` |
| `HEAD` | Same headers as `GET`, empty body |
| `PUT` | `201 Created`, or `411` without `Content-Length`, `500` on write failure |
| anything else | `405 Method Not Allowed` with an `Allow` header |

Additional statuses: `400`, `403`, `404`, `405`, `408`, `411`, `413`, `414`,
`431`, `500`, `501`, `505`.

---

## Benchmarks

### Environment

- Intel Core i7-13700HX, 24 vCPUs exposed to WSL2
- Linux 6.18 (WSL2), GCC 15.2.0, `-O3 -march=native`
- `wrk 4.2.0` and `ApacheBench 2.3`
- Server and client on the same host over loopback
- Document root copied to `tmpfs`; serving directly from a Windows-mounted
  filesystem under WSL adds a 9p file-open penalty of several milliseconds per
  request and is **not** representative of the server itself.

Reproduce with:

```bash
# defaults: ab keep-alive + wrk sweep, results under bench/results/
./bench/benchmark.sh

# point at custom tools or tune the workload
AB=$(command -v ab) WRK=$(command -v wrk) \
  AB_REQUESTS=500000 WRK_DURATION=15 WRK_CONNECTIONS="50 200 1000" \
  ./bench/benchmark.sh
```

### Results

**ApacheBench, keep-alive** (`-n 300000 -c 50 -k`):

| Metric | Value |
|---|---|
| Requests/sec | **132,541** |
| Mean time/request | **0.377 ms** |
| Keep-alive requests | 300,000 / 300,000 |
| Failed requests | 0 |

Every one of the 300,000 responses was served on a persistent connection with
zero failures.

**ApacheBench, connection close** (`-n 50000 -c 50`): 32,152 req/s, 1.555 ms.
When every request pays for a fresh socket, throughput is bounded by connection
setup rather than the request path.

**wrk, keep-alive sweep** (`-d 8s`, 4 client threads, server `--threads 4`):

| Connections | Throughput | p50 | p90 | p99 |
|---|---|---|---|---|
| 10 | 154,885 req/s | 47 µs | 75 µs | 149 µs |
| 50 | 339,207 req/s | 120 µs | 222 µs | 382 µs |
| 100 | 382,467 req/s | 257 µs | 392 µs | 685 µs |
| 200 | 400,467 req/s | 455 µs | 691 µs | 1.07 ms |
| 500 | 409,391 req/s | 1.12 ms | 1.70 ms | 2.58 ms |

Zero socket errors and zero non-2xx responses across every run. Peak observed
throughput was ~516k req/s at 500 connections with `--threads 8`.

### Reading the numbers

- Keep-alive is the single biggest win. Reusing a socket removes the connect,
  accept, and teardown cost from the hot path.
- Latency stays in the tens of microseconds at low concurrency and grows roughly
  linearly with load, which is the expected shape for a non-blocking reactor.
- Absolute figures are loopback numbers on a fast laptop. Over a real network the
  ceiling becomes the link, but the relative gains from keep-alive and event
  loops still hold.

---

## Project layout

```
http-server/
├── bench/
│   ├── benchmark.sh          # reproducible ab + wrk harness
│   └── results/              # generated raw output
├── include/
│   ├── http_parser.hpp       # request model + parse API
│   ├── http_server.hpp       # ServerConfig + run_server()
│   └── mime.hpp              # extension -> content type, cached Date
├── public/
│   └── index.html            # sample document root
├── src/
│   ├── main.cpp              # config parsing, env + flags
│   ├── http_parser.cpp       # non-allocating HTTP/1.1 parser
│   ├── http_server.cpp       # epoll workers, connections, routing
│   └── mime.cpp              # MIME table + RFC 1123 date cache
├── Makefile
└── README.md
```

---

## Limitations

- No TLS. Terminate TLS at a reverse proxy, or link against OpenSSL and add an
  `SSL_READ`/`SSL_WRITE` branch to the connection flush path.
- No `Transfer-Encoding: chunked` bodies; `Content-Length` is required for `PUT`.
- No compression, ranges, or caching validators.
- `sendfile()` is Linux-specific. A portable build would fall back to
  `read`/`write`.
- Idle keep-alive connections are tracked in a hash map and swept once per
  second; a timing wheel would be nicer at very large connection counts.
