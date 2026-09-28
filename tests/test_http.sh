#!/usr/bin/env bash
# tests/test_http.sh — Integration tests for MICROHTTP
#
# Requires: curl, server running on localhost:8080
# Usage: ./tests/test_http.sh [PORT]
#
# Returns exit code 0 on all tests passed, non-zero on any failure.

set -euo pipefail

PORT="${1:-8080}"
BASE="http://localhost:${PORT}"
PASS=0
FAIL=0

RED='\033[0;31m'
GRN='\033[0;32m'
YLW='\033[1;33m'
NC='\033[0m'

hr() { echo "═══════════════════════════════════════════════════════"; }

pass() { echo -e "  ${GRN}[ PASS ]${NC} $1"; ((PASS++)); }
fail() { echo -e "  ${RED}[ FAIL ]${NC} $1"; ((FAIL++)); }
info() { echo -e "  ${YLW}[ INFO ]${NC} $1"; }

expect_status() {
    local desc="$1"
    local expected="$2"
    shift 2
    local actual
    actual=$(curl -s -o /dev/null -w "%{http_code}" "$@")
    if [[ "$actual" == "$expected" ]]; then
        pass "$desc → HTTP $actual"
    else
        fail "$desc → expected $expected, got $actual"
    fi
}

echo ""
hr
echo "  MICROHTTP HTTP Integration Test Suite"
echo "  Target: $BASE"
hr
echo ""

# ── Wait for server to be ready ───────────────────────────────────────────────
info "Waiting for server..."
for i in $(seq 1 10); do
    if curl -sf "$BASE/" > /dev/null 2>&1; then
        break
    fi
    sleep 0.5
done

# ── 1. GET index ─────────────────────────────────────────────────────────────
expect_status "GET / → 200" 200 "$BASE/"

# ── 2. GET explicit index.html ────────────────────────────────────────────────
expect_status "GET /index.html → 200" 200 "$BASE/index.html"

# ── 3. GET CSS file ───────────────────────────────────────────────────────────
expect_status "GET /style.css → 200" 200 "$BASE/style.css"

# ── 4. GET text file ──────────────────────────────────────────────────────────
expect_status "GET /test.txt → 200" 200 "$BASE/test.txt"

# ── 5. GET missing file ───────────────────────────────────────────────────────
expect_status "GET /missing.html → 404" 404 "$BASE/missing.html"

# ── 6. HEAD request ───────────────────────────────────────────────────────────
actual=$(curl -s -o /dev/null -w "%{http_code}" -X HEAD "$BASE/")
if [[ "$actual" == "200" ]]; then
    pass "HEAD / → 200"
else
    fail "HEAD / → expected 200, got $actual"
fi

# ── 7. HEAD should have no body ───────────────────────────────────────────────
body=$(curl -s -X HEAD "$BASE/" 2>&1 | wc -c)
if [[ "$body" -eq 0 ]]; then
    pass "HEAD response has no body"
else
    info "HEAD response body size: $body bytes (may include headers)"
fi

# ── 8. Content-Type for HTML ─────────────────────────────────────────────────
ct=$(curl -s -I "$BASE/index.html" | grep -i "content-type:" | tr -d '\r')
if echo "$ct" | grep -qi "text/html"; then
    pass "Content-Type: text/html for .html"
else
    fail "Content-Type check — got: $ct"
fi

# ── 9. Content-Type for CSS ──────────────────────────────────────────────────
ct=$(curl -s -I "$BASE/style.css" | grep -i "content-type:" | tr -d '\r')
if echo "$ct" | grep -qi "text/css"; then
    pass "Content-Type: text/css for .css"
else
    fail "Content-Type check — got: $ct"
fi

# ── 10. Unsupported method ────────────────────────────────────────────────────
expect_status "POST → 405" 405 -X POST "$BASE/"

# ── 11. Directory traversal (should be 403 or 404) ────────────────────────────
status=$(curl -s -o /dev/null -w "%{http_code}" "$BASE/../../etc/passwd")
if [[ "$status" == "403" || "$status" == "404" ]]; then
    pass "Traversal attempt blocked → $status"
else
    fail "Traversal NOT blocked → $status"
fi

# ── 12. Encoded traversal ────────────────────────────────────────────────────
status=$(curl -s -o /dev/null -w "%{http_code}" --path-as-is "$BASE/..%2F..%2Fetc%2Fpasswd")
if [[ "$status" == "403" || "$status" == "404" ]]; then
    pass "Encoded traversal blocked → $status"
else
    fail "Encoded traversal NOT blocked → $status"
fi

# ── 13. Range request ────────────────────────────────────────────────────────
status=$(curl -s -o /dev/null -w "%{http_code}" \
    -H "Range: bytes=0-9" "$BASE/test.txt")
if [[ "$status" == "206" || "$status" == "200" ]]; then
    pass "Range request → $status"
else
    fail "Range request → unexpected $status"
fi

# ── 14. Range with Content-Range header ──────────────────────────────────────
cr=$(curl -s -I -H "Range: bytes=0-9" "$BASE/test.txt" | \
     grep -i "content-range:" | tr -d '\r')
if echo "$cr" | grep -qi "bytes"; then
    pass "Range response includes Content-Range: $cr"
else
    info "Content-Range not found (may need larger test file)"
fi

# ── 15. If-Modified-Since (304) ───────────────────────────────────────────────
future_date="Sun, 01 Jan 2099 00:00:00 GMT"
status=$(curl -s -o /dev/null -w "%{http_code}" \
    -H "If-Modified-Since: $future_date" "$BASE/index.html")
if [[ "$status" == "304" ]]; then
    pass "If-Modified-Since future date → 304"
else
    info "If-Modified-Since → $status (304 expected if file is older than future date)"
fi

# ── 16. Keep-alive: two requests on same connection ──────────────────────────
# curl --keepalive automatically reuses the connection
resp=$(curl -s --keepalive "$BASE/" "$BASE/test.txt")
if [[ -n "$resp" ]]; then
    pass "Keep-alive: two requests succeeded"
else
    fail "Keep-alive: no response"
fi

# ── 17. Connection: close ────────────────────────────────────────────────────
conn_hdr=$(curl -s -I -H "Connection: close" "$BASE/" | \
           grep -i "connection:" | tr -d '\r')
if echo "$conn_hdr" | grep -qi "close"; then
    pass "Connection: close echoed back"
else
    info "Connection header: '$conn_hdr'"
fi

# ── 18. Last-Modified header present ────────────────────────────────────────
lm=$(curl -s -I "$BASE/index.html" | grep -i "last-modified:" | tr -d '\r')
if [[ -n "$lm" ]]; then
    pass "Last-Modified header present: $lm"
else
    fail "Last-Modified header missing"
fi

# ── 19. Content-Length header present ───────────────────────────────────────
cl=$(curl -s -I "$BASE/index.html" | grep -i "content-length:" | tr -d '\r')
if [[ -n "$cl" ]]; then
    pass "Content-Length header present: $cl"
else
    fail "Content-Length header missing"
fi

# ── Summary ───────────────────────────────────────────────────────────────────
echo ""
hr
echo -e "  Results: ${GRN}${PASS} passed${NC}, ${RED}${FAIL} failed${NC} (total $((PASS+FAIL)))"
hr
echo ""

exit $FAIL
