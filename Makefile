CXX ?= g++
CPPFLAGS ?=
CXXFLAGS ?= -std=c++11 -O3 -g
LDFLAGS ?=
LDLIBS ?=

MODULES := exec host nvm_chip nvm_chip/flash_memory sim ssd utils

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

.PHONY: all clean run

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CXX) $(LDFLAGS) $^ $(LDLIBS) -o $@

build/%.o: src/%.cpp
	@$(MAKE_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(INCLUDES) -MMD -MP -c $< -o $@

run: $(TARGET)
	$(RUNNER) -i ssdconfig.xml -w workload.xml

clean:
	@$(REMOVE_BUILD)
	@$(REMOVE_BINARY)

-include $(DEPS)
