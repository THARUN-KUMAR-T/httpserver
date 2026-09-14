#!/usr/bin/env bash
set -uo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="$REPO_DIR/build/httpd"
HOST="127.0.0.1"
PORT="${HTTP_PORT:-8000}"
SERVER_THREADS="${SERVER_THREADS:-4}"
WRK_DURATION="${WRK_DURATION:-8}"
AB_REQUESTS="${AB_REQUESTS:-300000}"
AB_CONCURRENCY="${AB_CONCURRENCY:-50}"
WRK_CONNECTIONS="${WRK_CONNECTIONS:-10 50 100 200 500 1000}"
RESULTS_DIR="$REPO_DIR/bench/results/$(date +%Y%m%d-%H%M%S)"
WORK_DIR="$(mktemp -d)"
DOCROOT="$WORK_DIR/public"
SERVER_PID=""

log() { printf '%s\n' "$*" | tee -a "$RESULTS_DIR/summary.txt"; }

cleanup() {
    if [ -n "$SERVER_PID" ]; then
        kill "$SERVER_PID" 2>/dev/null
        wait "$SERVER_PID" 2>/dev/null
    fi
    rm -rf "$WORK_DIR"
}
trap cleanup EXIT

find_tool() {
    local name="$1"
    if command -v "$name" >/dev/null 2>&1; then
        command -v "$name"
    else
        printf ''
    fi
}

AB="${AB:-$(find_tool ab)}"
WRK="${WRK:-$(find_tool wrk)}"

if [ -z "$AB" ] && [ -z "$WRK" ]; then
    echo "error: install apachebench (ab) and/or wrk to run benchmarks" >&2
    exit 1
fi

mkdir -p "$RESULTS_DIR"
cp -r "$REPO_DIR/public" "$WORK_DIR/"

make -C "$REPO_DIR" >/dev/null || { echo "build failed" >&2; exit 1; }

start_server() {
    "$BIN" --port "$PORT" --root "$DOCROOT" --threads "$SERVER_THREADS" \
        >"$RESULTS_DIR/server.log" 2>&1 &
    SERVER_PID=$!
    for _ in $(seq 1 50); do
        if (exec 3<>"/dev/tcp/$HOST/$PORT") 2>/dev/null; then
            exec 3>&- 2>/dev/null || true
            return 0
        fi
        sleep 0.1
    done
    echo "error: server did not become ready" >&2
    return 1
}

stop_server() {
    if [ -n "$SERVER_PID" ]; then
        kill "$SERVER_PID" 2>/dev/null
        wait "$SERVER_PID" 2>/dev/null
        SERVER_PID=""
    fi
}

log "cpp-httpd benchmark"
log "date: $(date -u '+%Y-%m-%dT%H:%M:%SZ')"
log "host: $(grep -m1 'model name' /proc/cpuinfo 2>/dev/null | cut -d: -f2 | xargs) ($(nproc) vcpu)"
log "kernel: $(uname -sr)"
log "server: threads=$SERVER_THREADS port=$PORT docroot=copy-on-tmp"
AB_LABEL="n/a"; [ -n "$AB" ] && AB_LABEL="$(basename "$AB")"
WRK_LABEL="n/a"; [ -n "$WRK" ] && WRK_LABEL="$(basename "$WRK")"
log "ab=$AB_LABEL wrk=$WRK_LABEL"
log ""

if [ -n "$AB" ]; then
    log "=== apachebench keep-alive: -n $AB_REQUESTS -c $AB_CONCURRENCY -k ==="
    start_server
    "$AB" -n "$AB_REQUESTS" -c "$AB_CONCURRENCY" -k \
        "http://$HOST:$PORT/index.html" >"$RESULTS_DIR/ab_keepalive.txt" 2>&1
    stop_server
    grep -E "Complete requests|Failed requests|Keep-Alive requests|Non-2xx|Requests per second|Time per request:|Transfer rate" \
        "$RESULTS_DIR/ab_keepalive.txt" | tee -a "$RESULTS_DIR/summary.txt"
    log ""

    log "=== apachebench connection-close: -n 50000 -c $AB_CONCURRENCY ==="
    start_server
    "$AB" -n 50000 -c "$AB_CONCURRENCY" \
        "http://$HOST:$PORT/index.html" >"$RESULTS_DIR/ab_close.txt" 2>&1
    stop_server
    grep -E "Complete requests|Failed requests|Non-2xx|Requests per second|Time per request:|Transfer rate" \
        "$RESULTS_DIR/ab_close.txt" | tee -a "$RESULTS_DIR/summary.txt"
    log ""
fi

if [ -n "$WRK" ]; then
    start_server
    for connections in $WRK_CONNECTIONS; do
        threads=$(( connections < SERVER_THREADS ? connections : SERVER_THREADS ))
        log "=== wrk keep-alive: -t $threads -c $connections -d ${WRK_DURATION}s ==="
        "$WRK" -t"$threads" -c"$connections" -d"${WRK_DURATION}s" --latency \
            "http://$HOST:$PORT/index.html" >"$RESULTS_DIR/wrk_c${connections}.txt" 2>&1
        grep -E "Latency |Req/Sec |requests in|Requests/sec|Socket errors|Non-2xx|50%|75%|90%|99%" \
            "$RESULTS_DIR/wrk_c${connections}.txt" | tee -a "$RESULTS_DIR/summary.txt"
        log ""
    done
    stop_server
fi

log "raw output: bench/results/$(basename "$RESULTS_DIR")"
