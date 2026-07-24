CXX ?= g++
CPPFLAGS ?=
CXXFLAGS ?= -std=c++11 -O3 -g
LDFLAGS ?=
LDLIBS ?=

MODULES := exec host nvm_chip nvm_chip/flash_memory policy sim ssd utils

SRC_DIRS := $(addprefix src/,$(MODULES)) src
SOURCES := $(sort $(foreach dir,$(SRC_DIRS),$(wildcard $(dir)/*.cpp)))
OBJECTS := $(patsubst src/%.cpp,build/%.o,$(SOURCES))
DEPS := $(OBJECTS:.o=.d)
INCLUDES := $(addprefix -I,$(SRC_DIRS))

ifeq ($(OS),Windows_NT)
SHELL := cmd.exe
.SHELLFLAGS := /C
EXEEXT := .exe
RUNNER = $(TARGET)
MAKE_DIR = if not exist "$(subst /,\,$(@D))" mkdir "$(subst /,\,$(@D))"
REMOVE_BUILD = if exist build rmdir /S /Q build
REMOVE_BINARY = if exist $(TARGET) del /Q $(TARGET)
else
EXEEXT :=
RUNNER = ./$(TARGET)
MAKE_DIR = mkdir -p "$(@D)"
REMOVE_BUILD = rm -rf build
REMOVE_BINARY = rm -f $(TARGET)
endif

TARGET := MQSim$(EXEEXT)
ZONE_TEST := build/tests/zone_directory_mapping_test$(EXEEXT)
WEAR_TEST := build/tests/wear_leveling_policy_test$(EXEEXT)
MIGRATION_TEST := build/tests/migration_executor_test$(EXEEXT)
TEST_TARGETS := $(ZONE_TEST) $(WEAR_TEST) $(MIGRATION_TEST)
TEST_OBJECTS := $(patsubst tests/%.cpp,build/tests/%.o,$(wildcard tests/*.cpp))
TEST_DEPS := $(TEST_OBJECTS:.o=.d)

.PHONY: all clean run test

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CXX) $(LDFLAGS) $^ $(LDLIBS) -o $@

build/%.o: src/%.cpp
	@$(MAKE_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(INCLUDES) -MMD -MP -c $< -o $@

build/tests/%.o: tests/%.cpp
	@$(MAKE_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(INCLUDES) -MMD -MP -c $< -o $@

$(ZONE_TEST): build/tests/zone_directory_mapping_test.o build/policy/zone_directory.o
	$(CXX) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(WEAR_TEST): build/tests/wear_leveling_policy_test.o build/policy/wear_leveling_policy.o build/policy/zone_directory.o
	$(CXX) $(LDFLAGS) $^ $(LDLIBS) -o $@

$(MIGRATION_TEST): build/tests/migration_executor_test.o build/policy/migration_executor.o build/policy/zone_directory.o
	$(CXX) $(LDFLAGS) $^ $(LDLIBS) -o $@

test: $(TEST_TARGETS)
	$(ZONE_TEST)
	$(WEAR_TEST)
	$(MIGRATION_TEST)

run: $(TARGET)
	$(RUNNER) -i ssdconfig.xml -w workload.xml

clean:
	@$(REMOVE_BUILD)
	@$(REMOVE_BINARY)

-include $(DEPS) $(TEST_DEPS)
