#!/usr/bin/env bash
# benchmark/benchmark.sh — MICROHTTP Benchmarking Script
#
# Compares single, threadpool, and epoll server modes.
# Measures: requests/sec, success rate, p50/p99 latency.
#
# Tools used:
#   wrk  (preferred) — https://github.com/wg/wrk
#   ab   (fallback)  — Apache Benchmark (usually available: apt install apache2-utils)
#   curl (last resort) — simple sequential timing
#
# Usage:
#   ./benchmark/benchmark.sh [PORT] [DURATION_SEC] [CONNECTIONS]
#
# Run on Ubuntu Linux.

set -euo pipefail

PORT="${1:-8080}"
DURATION="${2:-10}"
CONNECTIONS="${3:-50}"
THREADS="${4:-4}"

SERVER_BIN="./build/microhttp"
DOC_ROOT="./public"
BASE="http://localhost:${PORT}"
RESULTS_DIR="benchmark/results"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)

mkdir -p "$RESULTS_DIR"

RED='\033[0;31m'
GRN='\033[0;32m'
YLW='\033[1;33m'
CYN='\033[0;36m'
NC='\033[0m'

hr()   { echo "═══════════════════════════════════════════════════════════"; }
info() { echo -e "  ${CYN}[INFO]${NC} $1"; }
warn() { echo -e "  ${YLW}[WARN]${NC} $1"; }

# ── Tool detection ────────────────────────────────────────────────────────────

detect_benchmark_tool() {
    if command -v wrk >/dev/null 2>&1; then
        echo "wrk"
    elif command -v ab >/dev/null 2>&1; then
        echo "ab"
    elif command -v curl >/dev/null 2>&1; then
        echo "curl"
    else
        echo "none"
    fi
}

TOOL=$(detect_benchmark_tool)
info "Benchmark tool: $TOOL"

if [[ "$TOOL" == "none" ]]; then
    warn "No benchmark tool found. Install: apt install apache2-utils"
    exit 1
fi

# ── Server lifecycle ──────────────────────────────────────────────────────────

SERVER_PID=""

start_server() {
    local mode="$1"
    local extra="${2:-}"
    info "Starting server: mode=$mode port=$PORT"
    $SERVER_BIN --mode "$mode" --port "$PORT" --root "$DOC_ROOT" \
        --quiet $extra &
    SERVER_PID=$!
    sleep 1  # wait for server to bind

    # Verify server started
    if ! kill -0 "$SERVER_PID" 2>/dev/null; then
        warn "Server failed to start!"
        return 1
    fi
    info "Server PID: $SERVER_PID"
}

stop_server() {
    if [[ -n "$SERVER_PID" ]] && kill -0 "$SERVER_PID" 2>/dev/null; then
        info "Stopping server PID $SERVER_PID..."
        kill -SIGTERM "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
        SERVER_PID=""
    fi
}

# Ensure cleanup on exit
trap stop_server EXIT

# ── Benchmark runner ──────────────────────────────────────────────────────────

run_benchmark_wrk() {
    local label="$1"
    local output_file="$2"

    echo "" | tee -a "$output_file"
    echo "  Mode: $label" | tee -a "$output_file"
    echo "  Tool: wrk | Duration: ${DURATION}s | Connections: $CONNECTIONS | Threads: $THREADS" | tee -a "$output_file"
    echo "" | tee -a "$output_file"

    wrk -t"$THREADS" -c"$CONNECTIONS" -d"${DURATION}s" \
        --latency "$BASE/index.html" 2>&1 | tee -a "$output_file"
}

run_benchmark_ab() {
    local label="$1"
    local output_file="$2"
    local total_requests=$(( CONNECTIONS * 100 ))

    echo "" | tee -a "$output_file"
    echo "  Mode: $label" | tee -a "$output_file"
    echo "  Tool: ab | Requests: $total_requests | Concurrency: $CONNECTIONS" | tee -a "$output_file"
    echo "" | tee -a "$output_file"

    ab -n "$total_requests" -c "$CONNECTIONS" -k \
        "${BASE}/index.html" 2>&1 | tee -a "$output_file"
}

run_benchmark_curl() {
    local label="$1"
    local output_file="$2"
    local n=100  # sequential requests

    echo "" | tee -a "$output_file"
    echo "  Mode: $label" | tee -a "$output_file"
    echo "  Tool: curl (sequential) | Requests: $n" | tee -a "$output_file"
    echo "" | tee -a "$output_file"

    local total=0
    local ok=0
    local start_time end_time elapsed
    start_time=$(date +%s%N)

    for i in $(seq 1 $n); do
        code=$(curl -s -o /dev/null -w "%{http_code}" "$BASE/index.html")
        ((total++))
        [[ "$code" == "200" ]] && ((ok++))
    done

    end_time=$(date +%s%N)
    elapsed=$(( (end_time - start_time) / 1000000 ))  # ms

    local rps=$(( total * 1000 / (elapsed + 1) ))
    echo "  Total requests: $total" | tee -a "$output_file"
    echo "  Successful:     $ok" | tee -a "$output_file"
    echo "  Elapsed:        ${elapsed}ms" | tee -a "$output_file"
    echo "  Requests/sec:   ~$rps (sequential, not concurrent)" | tee -a "$output_file"
}

run_benchmark() {
    local label="$1"
    local output_file="$2"
    case "$TOOL" in
        wrk)  run_benchmark_wrk  "$label" "$output_file" ;;
        ab)   run_benchmark_ab   "$label" "$output_file" ;;
        curl) run_benchmark_curl "$label" "$output_file" ;;
    esac
}

# ── Main benchmark loop ───────────────────────────────────────────────────────

RESULT_FILE="${RESULTS_DIR}/benchmark_${TIMESTAMP}.txt"
echo "MICROHTTP Benchmark — $(date)" > "$RESULT_FILE"
echo "System: $(uname -a)" >> "$RESULT_FILE"
echo "Tool: $TOOL | Duration: ${DURATION}s | Connections: $CONNECTIONS" >> "$RESULT_FILE"
echo "" >> "$RESULT_FILE"

echo ""
hr
echo "  MICROHTTP Benchmark Suite"
echo "  Tool: $TOOL | Port: $PORT | Duration: ${DURATION}s | Connections: $CONNECTIONS"
hr

# ── Mode 1: Single-threaded ───────────────────────────────────────────────────
echo ""
echo -e "  ${YLW}▶ Mode 1: Single-threaded${NC}"
hr
echo "── Single-threaded ─────────────────────────────────────" >> "$RESULT_FILE"
start_server "single"
run_benchmark "single-threaded" "$RESULT_FILE"
stop_server
sleep 1

# ── Mode 2: Thread pool ───────────────────────────────────────────────────────
echo ""
echo -e "  ${YLW}▶ Mode 2: Thread Pool (4 workers)${NC}"
hr
echo "── Thread Pool (4 workers) ──────────────────────────────" >> "$RESULT_FILE"
start_server "threadpool" "--workers 4"
run_benchmark "threadpool-4" "$RESULT_FILE"
stop_server
sleep 1

# Thread pool with more workers
echo ""
echo -e "  ${YLW}▶ Mode 2b: Thread Pool (8 workers)${NC}"
hr
echo "── Thread Pool (8 workers) ──────────────────────────────" >> "$RESULT_FILE"
start_server "threadpool" "--workers 8"
run_benchmark "threadpool-8" "$RESULT_FILE"
stop_server
sleep 1

# ── Mode 3: epoll (Linux only) ────────────────────────────────────────────────
if [[ "$(uname)" == "Linux" ]]; then
    echo ""
    echo -e "  ${YLW}▶ Mode 3: epoll Event Loop${NC}"
    hr
    echo "── epoll Event Loop ──────────────────────────────────────" >> "$RESULT_FILE"
    start_server "epoll"
    run_benchmark "epoll" "$RESULT_FILE"
    stop_server
    sleep 1
else
    warn "epoll mode: Linux only — skipping on $(uname)"
    echo "── epoll: skipped (not Linux) ────────────────────────────" >> "$RESULT_FILE"
fi

# ── Summary ───────────────────────────────────────────────────────────────────
echo ""
hr
info "Results saved to: $RESULT_FILE"
hr
echo ""

cat "$RESULT_FILE"
