# ─────────────────────────────────────────────────────────────────────────────
# Makefile — MICROHTTP Concurrent HTTP/1.1 Web Server
#
# Targets:
#   make            — build (debug build by default)
#   make debug      — debug build with AddressSanitizer
#   make release    — optimized release build
#   make test       — run automated test suite
#   make valgrind   — run server under Valgrind (Linux only)
#   make clean      — remove build artifacts
#   make format     — format C source with clang-format (if available)
#   make help       — show this help
# ─────────────────────────────────────────────────────────────────────────────

# ── Compiler & tools ──────────────────────────────────────────────────────────
CC      := gcc
RM      := rm -f
MKDIR   := mkdir -p

# ── Directories ───────────────────────────────────────────────────────────────
SRCDIR  := src
OBJDIR  := build/obj
BINDIR  := build

# ── Target binary ─────────────────────────────────────────────────────────────
TARGET  := $(BINDIR)/microhttp

# ── Source files ──────────────────────────────────────────────────────────────
SRCS := \
    $(SRCDIR)/main.c           \
    $(SRCDIR)/server.c         \
    $(SRCDIR)/http.c           \
    $(SRCDIR)/parser.c         \
    $(SRCDIR)/connection.c     \
    $(SRCDIR)/file.c           \
    $(SRCDIR)/mime.c           \
    $(SRCDIR)/logger.c         \
    $(SRCDIR)/signal_handler.c \
    $(SRCDIR)/timer.c          \
    $(SRCDIR)/thread_pool.c    \
    $(SRCDIR)/epoll_server.c

OBJS := $(patsubst $(SRCDIR)/%.c, $(OBJDIR)/%.o, $(SRCS))

# ── Dependency files (auto-generated) ────────────────────────────────────────
DEPS := $(OBJS:.o=.d)

# ── Compiler flags ────────────────────────────────────────────────────────────
# Base flags (always applied)
BASE_CFLAGS := \
    -std=c11            \
    -Wall               \
    -Wextra             \
    -Wpedantic          \
    -Wshadow            \
    -Wstrict-prototypes \
    -Wmissing-prototypes\
    -Wformat=2          \
    -D_GNU_SOURCE       \
    -D_POSIX_C_SOURCE=200809L \
    -I$(SRCDIR)

# Debug flags (default build)
DEBUG_CFLAGS := $(BASE_CFLAGS) -g3 -O0 -DDEBUG

# Release flags
RELEASE_CFLAGS := $(BASE_CFLAGS) -O2 -DNDEBUG

# ASan flags (AddressSanitizer — great for development)
ASAN_CFLAGS := $(DEBUG_CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer

# ── Linker flags ──────────────────────────────────────────────────────────────
LDFLAGS := -pthread

# ── Default build (debug) ─────────────────────────────────────────────────────
CFLAGS ?= $(DEBUG_CFLAGS)

# ────────────────────────────────────────────────────────────────────────────
.PHONY: all debug release asan clean test valgrind format help

all: $(TARGET)

# ── Link ─────────────────────────────────────────────────────────────────────
$(TARGET): $(OBJS) | $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	@echo ""
	@echo "  ✓ Built: $@"
	@echo "  Run: $@ --help"
	@echo ""

# ── Compile each .c → .o, generating .d dependency files ─────────────────────
$(OBJDIR)/%.o: $(SRCDIR)/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

# ── Create directories ────────────────────────────────────────────────────────
$(OBJDIR):
	$(MKDIR) $(OBJDIR)

$(BINDIR):
	$(MKDIR) $(BINDIR)

# ── Include auto-generated dependencies ──────────────────────────────────────
-include $(DEPS)

# ── debug target (explicit) ───────────────────────────────────────────────────
debug: CFLAGS = $(DEBUG_CFLAGS)
debug: clean $(TARGET)

# ── release target ────────────────────────────────────────────────────────────
release: CFLAGS = $(RELEASE_CFLAGS)
release: clean $(TARGET)
	@echo "  Release build complete."

# ── AddressSanitizer build ────────────────────────────────────────────────────
asan: CFLAGS = $(ASAN_CFLAGS)
asan: LDFLAGS += -fsanitize=address,undefined
asan: clean $(TARGET)
	@echo "  ASan build complete. Run with ASAN_OPTIONS=detect_leaks=1"

# ── Clean ─────────────────────────────────────────────────────────────────────
clean:
	$(RM) -r build/
	@echo "  Cleaned build artifacts."

# ── Tests ─────────────────────────────────────────────────────────────────────
# Unit tests (pure C, no server)
TEST_PARSER_SRC := tests/test_parser.c $(SRCDIR)/parser.c $(SRCDIR)/logger.c
TEST_PATH_SRC   := tests/test_path.c   $(SRCDIR)/file.c   $(SRCDIR)/mime.c \
                                        $(SRCDIR)/logger.c

test: all
	@echo ""
	@echo "═══ Running Unit Tests ═══════════════════════════════════════════"
	@$(MKDIR) build/
	$(CC) $(DEBUG_CFLAGS) -o build/test_parser $(TEST_PARSER_SRC) $(LDFLAGS) && \
	    ./build/test_parser && echo "  ✓ test_parser passed"
	$(CC) $(DEBUG_CFLAGS) -o build/test_path $(TEST_PATH_SRC) $(LDFLAGS) && \
	    ./build/test_path && echo "  ✓ test_path passed"
	@echo ""
	@echo "═══ Running Integration Tests (requires server on Ubuntu) ════════"
	@echo "  Start server: ./build/microhttp --mode threadpool --port 8080"
	@echo "  Then run:     bash tests/test_http.sh"
	@echo ""

# ── Valgrind (Linux only) ─────────────────────────────────────────────────────
valgrind: debug
	@echo ""
	@echo "═══ Running under Valgrind ═══════════════════════════════════════"
	@echo "  (Run on Ubuntu Linux — valgrind is not available on macOS M-series)"
	@echo ""
	valgrind \
	    --leak-check=full \
	    --show-leak-kinds=all \
	    --track-origins=yes \
	    --error-exitcode=1 \
	    --log-file=valgrind-report.txt \
	    ./$(TARGET) --mode threadpool --port 8080 --root ./public --workers 2 \
	    &
	@sleep 3
	@echo "  Sending test request..."
	curl -s http://localhost:8080/ > /dev/null || true
	@sleep 1
	kill %1 2>/dev/null || true
	@echo "  Valgrind report: valgrind-report.txt"

# ── Format ────────────────────────────────────────────────────────────────────
format:
	@if command -v clang-format >/dev/null 2>&1; then \
	    clang-format -i --style="{BasedOnStyle: LLVM, IndentWidth: 4, ColumnLimit: 90}" \
	        $(SRCS) tests/test_parser.c tests/test_path.c; \
	    echo "  ✓ Formatted with clang-format"; \
	else \
	    echo "  clang-format not found — skipping"; \
	fi

# ── Help ──────────────────────────────────────────────────────────────────────
help:
	@echo ""
	@echo "MICROHTTP Makefile"
	@echo ""
	@echo "  make              Build debug binary"
	@echo "  make debug        Debug build (explicit)"
	@echo "  make release      Optimized release build"
	@echo "  make asan         AddressSanitizer build"
	@echo "  make test         Build and run unit tests"
	@echo "  make valgrind     Run under Valgrind (Linux only)"
	@echo "  make clean        Remove all build artifacts"
	@echo "  make format       Format source with clang-format"
	@echo "  make help         Show this message"
	@echo ""
	@echo "  Compiler flags: $(BASE_CFLAGS)"
	@echo ""
