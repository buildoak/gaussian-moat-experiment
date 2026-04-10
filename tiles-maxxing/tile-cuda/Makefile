NVCC ?= nvcc

TARGET := tile_kernel
SRC_DIR := src
OBJ_DIR := build
INCLUDE_DIR := include

NVCCFLAGS := -arch=sm_87 -O3 -std=c++17 --expt-relaxed-constexpr -lineinfo
CPPFLAGS := -I$(INCLUDE_DIR)
LDFLAGS :=

SRCS := $(SRC_DIR)/main.cu $(SRC_DIR)/tile_kernel.cu
OBJS := $(patsubst $(SRC_DIR)/%.cu,$(OBJ_DIR)/%.o,$(SRCS))

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(NVCC) $(NVCCFLAGS) $(LDFLAGS) -o $@ $(OBJS)

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cu | $(OBJ_DIR)
	$(NVCC) $(NVCCFLAGS) $(CPPFLAGS) -dc -c $< -o $@

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

clean:
	rm -rf $(OBJ_DIR) $(TARGET)
