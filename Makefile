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

.PHONY: all clean rebuild run gtest test
