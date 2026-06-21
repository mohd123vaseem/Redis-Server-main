CXX = g++
CXXFLAGS = -std=c++17 -Wall -pthread -MMD -MP -O2

SRC_DIR = src
BUILD_DIR = build

SRCS := $(wildcard $(SRC_DIR)/*.cpp)
OBJS := $(patsubst $(SRC_DIR)/%.cpp, $(BUILD_DIR)/%.o, $(SRCS))

TARGET = my_redis_server

all: $(TARGET)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) $(OBJS) -o $(TARGET)

clean:
	rm -rf $(BUILD_DIR) $(TARGET)

rebuild: clean all

run: all
	./$(TARGET)

# ----------------------------------------------------------------------------
# Testing (Phase 2): Google Test, vendored as source under third_party/ and
# compiled directly with g++ (no cmake needed).
# ----------------------------------------------------------------------------
GTEST_REPO = https://github.com/google/googletest.git
GTEST_TAG  = v1.14.0
GTEST_ROOT = third_party/googletest
GTEST_DIR  = $(GTEST_ROOT)/googletest
GTEST_INC  = -I$(GTEST_DIR)/include -I$(GTEST_DIR)

TEST_DIR  = tests
TEST_SRCS = $(wildcard $(TEST_DIR)/*.cpp)
TEST_OBJS = $(patsubst $(TEST_DIR)/%.cpp, $(BUILD_DIR)/%.test.o, $(TEST_SRCS))
TEST_BIN  = $(BUILD_DIR)/run_tests

# Code under test = all sources EXCEPT main.cpp (it defines the server's main();
# Google Test supplies its own main via gtest_main).
TESTABLE_OBJS = $(filter-out $(BUILD_DIR)/main.o, $(OBJS))

# Fetch Google Test once (idempotent).
gtest:
	@test -d $(GTEST_ROOT) || git clone --depth 1 --branch $(GTEST_TAG) $(GTEST_REPO) $(GTEST_ROOT)
	@echo "Google Test ready at $(GTEST_ROOT)"

# Build the framework from source into single objects.
$(BUILD_DIR)/gtest-all.o: $(GTEST_DIR)/src/gtest-all.cc | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(GTEST_INC) -c $< -o $@

$(BUILD_DIR)/gtest_main.o: $(GTEST_DIR)/src/gtest_main.cc | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(GTEST_INC) -c $< -o $@

# Compile each test file.
$(BUILD_DIR)/%.test.o: $(TEST_DIR)/%.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(GTEST_INC) -c $< -o $@

# Link the test binary: test objects + code under test + framework.
$(TEST_BIN): $(TEST_OBJS) $(TESTABLE_OBJS) $(BUILD_DIR)/gtest-all.o $(BUILD_DIR)/gtest_main.o
	$(CXX) $(CXXFLAGS) $^ -o $@

# Build and run the tests.
test: $(TEST_BIN)
	./$(TEST_BIN)

# ----------------------------------------------------------------------------
# Coverage (Phase 2, Step 6): compile the code under test + tests WITH gcov
# instrumentation (--coverage), run them, then summarise with gcovr.
#
# Only src/RedisDatabase.cpp and src/RedisCommandHandler.cpp are instrumented
# (the layers the unit tests exercise). RedisServer.cpp is networking, covered
# by test_all.sh, so it's excluded here. Google Test itself is reused
# un-instrumented (we don't measure the framework's own coverage).
#
# Needs gcovr:  sudo apt-get install -y gcovr
# ----------------------------------------------------------------------------
COV_DIR      = $(BUILD_DIR)/cov
COV_CXXFLAGS = -std=c++17 -pthread -O0 -g --coverage
COV_SRC_OBJS = $(COV_DIR)/RedisDatabase.o $(COV_DIR)/RedisCommandHandler.o
COV_TEST_OBJS = $(patsubst $(TEST_DIR)/%.cpp, $(COV_DIR)/%.test.o, $(TEST_SRCS))
COV_BIN      = $(COV_DIR)/run_tests_cov

$(COV_DIR):
	mkdir -p $(COV_DIR)

$(COV_DIR)/%.o: $(SRC_DIR)/%.cpp | $(COV_DIR)
	$(CXX) $(COV_CXXFLAGS) $(GTEST_INC) -c $< -o $@

$(COV_DIR)/%.test.o: $(TEST_DIR)/%.cpp | $(COV_DIR)
	$(CXX) $(COV_CXXFLAGS) $(GTEST_INC) -c $< -o $@

# Instrumented objects link with the (un-instrumented) gtest objects; --coverage
# on the link line pulls in libgcov.
coverage: $(COV_SRC_OBJS) $(COV_TEST_OBJS) $(BUILD_DIR)/gtest-all.o $(BUILD_DIR)/gtest_main.o
	$(CXX) $(COV_CXXFLAGS) $^ -o $(COV_BIN)
	./$(COV_BIN)
	gcovr --root . --filter 'src/' --print-summary \
	      --xml-pretty --output $(COV_DIR)/coverage.xml \
	      --html-details $(COV_DIR)/coverage.html
	@echo "----------------------------------------------------------------"
	@echo "Coverage XML  -> $(COV_DIR)/coverage.xml   (for Codecov upload)"
	@echo "Coverage HTML -> $(COV_DIR)/coverage.html  (open in a browser)"

# ----------------------------------------------------------------------------
# AddressSanitizer + UndefinedBehaviorSanitizer build — for hunting memory bugs
# (buffer overflows, use-after-free). Recompiles with tripwires that print the
# EXACT file:line of any bad memory access at runtime.
#
# IMPORTANT: do NOT run this build under `ulimit -v` — ASan reserves a huge
# virtual address space for its shadow memory, so a virtual-memory cap breaks it.
# Use `timeout <secs>` alone to cage it.
# ----------------------------------------------------------------------------
ASAN_BIN = my_redis_server_asan
asan:
	$(CXX) -std=c++17 -pthread -g -O1 -fsanitize=address,undefined \
	    -fno-omit-frame-pointer $(SRCS) -o $(ASAN_BIN)
	@echo "Built $(ASAN_BIN). Run it caged:  timeout 20 ./$(ASAN_BIN) 6399"

# ----------------------------------------------------------------------------
# ThreadSanitizer build — for hunting DATA RACES (two threads touching the same
# memory without synchronization). Catches bugs AddressSanitizer cannot, and
# prints the exact lines of both racing accesses. Cage with `timeout`.
# ----------------------------------------------------------------------------
TSAN_BIN = my_redis_server_tsan
tsan:
	$(CXX) -std=c++17 -pthread -g -O1 -fsanitize=thread \
	    -fno-omit-frame-pointer $(SRCS) -o $(TSAN_BIN)
	@echo "Built $(TSAN_BIN). Run it caged:  timeout 30 ./$(TSAN_BIN) 6399"

.PHONY: all clean rebuild run gtest test coverage asan tsan
