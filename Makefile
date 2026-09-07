CC        := g++
LD        := g++
CC_FLAGS := -std=c++11 -O3 -g

MODULES   := exec host nvm_chip nvm_chip/flash_memory policy sim ssd utils
SRC_DIR   := $(addprefix src/,$(MODULES)) src
BUILD_DIR := $(addprefix build/,$(MODULES)) build

SRC       := $(foreach sdir,$(SRC_DIR),$(wildcard $(sdir)/*.cpp))
SRC       := src/main.cpp $(SRC)
OBJ       := $(patsubst src/%.cpp,build/%.o,$(SRC))
INCLUDES  := $(addprefix -I,$(SRC_DIR))

vpath %.cpp $(SRC_DIR)

define make-goal
$1/%.o: %.cpp
	$(CC) $(CC_FLAGS) $(INCLUDES) -c $$< -o $$@
endef

.PHONY: all checkdirs clean

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
