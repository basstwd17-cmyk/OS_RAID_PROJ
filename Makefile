CC        := g++
.DEFAULT_GOAL := all
LD        := g++
CC_FLAGS := -std=c++11 -O3 -g -MMD -MP

MODULES   := exec host nvm_chip nvm_chip/flash_memory policy sim ssd utils
SRC_DIR   := $(addprefix src/,$(MODULES)) src
BUILD_DIR := $(addprefix build/,$(MODULES)) build

SRC       := $(foreach sdir,$(SRC_DIR),$(wildcard $(sdir)/*.cpp))
SRC       := src/main.cpp $(SRC)
OBJ       := $(patsubst src/%.cpp,build/%.o,$(SRC))
INCLUDES  := $(addprefix -I,$(SRC_DIR))

-include $(OBJ:.o=.d)

vpath %.cpp $(SRC_DIR)

define make-goal
$1/%.o: %.cpp
	$(CC) $(CC_FLAGS) $(INCLUDES) -c $$< -o $$@
endef

.PHONY: all checkdirs clean

.PHONY: test
test: all build/wear_trigger_test build/zone_popularity_test build/reserve_exchange_test build/advanced_swans_test
	./build/wear_trigger_test
	./build/zone_popularity_test
	./build/reserve_exchange_test
	python3 tests/wear_trigger_smoke.py ./MQSim
	python3 tests/reserve_exchange_smoke.py ./MQSim
	python3 tests/advanced_swans_smoke.py ./MQSim
	python3 tests/eol_modes_smoke.py ./MQSim
	python3 tests/run_advanced_unit.py ./build/advanced_swans_test

build/advanced_swans_test: tests/advanced_swans.cpp src/policy/wear_leveling_policy.cpp src/policy/zone_directory.cpp src/host/Trace_Pool.cpp $(wildcard src/policy/*.h) src/ssd/TEC_Wear.h src/host/Trace_Pool.h | checkdirs
	$(CC) $(CC_FLAGS) tests/advanced_swans.cpp src/policy/wear_leveling_policy.cpp src/policy/zone_directory.cpp src/host/Trace_Pool.cpp -o $@

build/wear_trigger_test: tests/wear_trigger.cpp src/policy/wear_leveling_policy.cpp src/policy/zone_directory.cpp $(wildcard src/policy/*.h) | checkdirs
	$(CC) $(CC_FLAGS) tests/wear_trigger.cpp src/policy/wear_leveling_policy.cpp src/policy/zone_directory.cpp -o $@

build/zone_popularity_test: tests/zone_popularity.cpp src/policy/zone_directory.cpp $(wildcard src/policy/*.h) | checkdirs
	$(CC) $(CC_FLAGS) tests/zone_popularity.cpp src/policy/zone_directory.cpp -o $@

build/reserve_exchange_test: tests/reserve_exchange.cpp src/policy/migration_executor.cpp src/policy/zone_directory.cpp src/policy/wear_leveling_policy.cpp src/ssd/User_Request.cpp $(wildcard src/policy/*.h) | checkdirs
	$(CC) $(CC_FLAGS) tests/reserve_exchange.cpp src/policy/migration_executor.cpp src/policy/zone_directory.cpp src/policy/wear_leveling_policy.cpp src/ssd/User_Request.cpp -o $@

ifeq ($(OS),Windows_NT)
MKDIR_CMD = if not exist "$@" mkdir "$@"
RM_BUILD_CMD = if exist build rmdir /S /Q build
RM_BIN_CMD = if exist MQSim.exe del /Q MQSim.exe & if exist MQSim del /Q MQSim
else
MKDIR_CMD = mkdir -p "$@"
RM_BUILD_CMD = rm -rf $(BUILD_DIR)
RM_BIN_CMD = rm -f MQSim
endif

all: checkdirs MQSim

MQSim: $(OBJ)
	$(LD) $^ -o $@

checkdirs: $(BUILD_DIR)

$(BUILD_DIR):
	@$(MKDIR_CMD)

clean:
	@$(RM_BUILD_CMD)
	@$(RM_BIN_CMD)

$(foreach bdir,$(BUILD_DIR),$(eval $(call make-goal,$(bdir))))
