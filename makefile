# ══════════════════════════════════════════════════════════════════════════════
#  HFT Simulator — Production Makefile
#  Compiler: g++ / clang++ with C++17, -O3, strict warnings
# ══════════════════════════════════════════════════════════════════════════════

CXX      ?= g++
CXXFLAGS  = -std=c++17 -O3 -funroll-loops \
            -fno-omit-frame-pointer \
            -Wall -Wextra -Wpedantic \
            -Wshadow -Wconversion -Wno-unused-parameter \
            -pthread
INCLUDES  = -Iinclude
LDFLAGS   = -pthread

# Sanitiser builds (for development / CI)
ASAN_FLAGS  = -fsanitize=address,undefined -fno-sanitize-recover=all -g -O1
TSAN_FLAGS  = -fsanitize=thread -g -O1

SRC_DIR   = src
OBJ_DIR   = build/obj
BIN_DIR   = build/bin
TEST_DIR  = tests

SRCS      = $(wildcard $(SRC_DIR)/*.cpp)
OBJS      = $(SRCS:$(SRC_DIR)/%.cpp=$(OBJ_DIR)/%.o)
TARGET    = $(BIN_DIR)/hft_simulator

TEST_SRCS = $(wildcard $(TEST_DIR)/*.cpp)
TEST_BINS = $(TEST_SRCS:$(TEST_DIR)/%.cpp=$(BIN_DIR)/%)

# ── Default target ─────────────────────────────────────────────────────────────
.PHONY: all
all: dirs $(TARGET)

$(TARGET): $(OBJS)
	@echo "[LD]  $@"
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)
	@echo "[OK]  Build complete → $@"

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cpp
	@echo "[CC]  $<"
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

# ── Tests ──────────────────────────────────────────────────────────────────────
.PHONY: tests
tests: dirs $(TEST_BINS)

$(BIN_DIR)/%: $(TEST_DIR)/%.cpp $(filter-out $(OBJ_DIR)/main.o, $(OBJS))
	@echo "[CC]  $<"
	$(CXX) $(CXXFLAGS) $(INCLUDES) $^ -o $@ $(LDFLAGS)
	@echo "[OK]  Test binary → $@"

.PHONY: run_tests
run_tests: tests
	@for t in $(TEST_BINS); do \
	    echo "\n[RUN] $$t"; \
	    $$t; \
	done

# ── Run ────────────────────────────────────────────────────────────────────────
.PHONY: run
run: all
	@echo "\n[RUN] $(TARGET)\n"
	$(TARGET)

# ── Sanitiser builds ──────────────────────────────────────────────────────────
.PHONY: asan
asan: dirs
	$(CXX) $(ASAN_FLAGS) $(INCLUDES) $(SRCS) -o $(BIN_DIR)/hft_asan $(LDFLAGS)
	@echo "[OK]  ASAN build → $(BIN_DIR)/hft_asan"

.PHONY: tsan
tsan: dirs
	$(CXX) $(TSAN_FLAGS) $(INCLUDES) $(SRCS) -o $(BIN_DIR)/hft_tsan $(LDFLAGS)
	@echo "[OK]  TSAN build → $(BIN_DIR)/hft_tsan"

# ── Benchmarks ────────────────────────────────────────────────────────────────
.PHONY: bench
bench: dirs
	$(CXX) $(CXXFLAGS) -DBENCHMARK_MODE $(INCLUDES) $(SRCS) -o $(BIN_DIR)/hft_bench $(LDFLAGS)
	$(BIN_DIR)/hft_bench

# ── Utilities ─────────────────────────────────────────────────────────────────
.PHONY: dirs
dirs:
	@mkdir -p $(OBJ_DIR) $(BIN_DIR)

.PHONY: clean
clean:
	@rm -rf build/
	@echo "[OK]  Cleaned."

.PHONY: format
format:
	@command -v clang-format >/dev/null 2>&1 && \
	    find include src tests -name "*.hpp" -o -name "*.cpp" | \
	    xargs clang-format -i --style="{BasedOnStyle: Google, IndentWidth: 4}" \
	    && echo "[OK]  Formatted." || echo "[SKIP] clang-format not found."

.PHONY: info
info:
	@echo "Compiler  : $(CXX)"
	@$(CXX) --version | head -1
	@echo "CXXFLAGS  : $(CXXFLAGS)"
	@echo "Sources   : $(SRCS)"