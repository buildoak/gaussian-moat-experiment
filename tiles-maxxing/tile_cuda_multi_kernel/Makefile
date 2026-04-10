NVCC ?= /usr/local/cuda/bin/nvcc

TARGET := tile_kernel_multi
PROFILE_TARGET := tile_kernel_multi_profile
SRC_DIR := src
OBJ_DIR := build
INCLUDE_DIR := include

# Base flags
BASE_FLAGS := -arch=sm_87 -O3 -std=c++17 --expt-relaxed-constexpr -lineinfo --ptxas-options=-v
CPPFLAGS := -I$(INCLUDE_DIR)
LDFLAGS :=

# Per-kernel register caps
K_SIEVE_FLAGS := --maxrregcount=40
K_MR_FLAGS :=
K_COMPACT_FLAGS := --maxrregcount=32
K_UF_FLAGS := --maxrregcount=40
K_FACE_FLAGS := --maxrregcount=40

# Source files
SRCS := $(SRC_DIR)/main.cu \
        $(SRC_DIR)/kernel_sieve.cu \
        $(SRC_DIR)/kernel_mr.cu \
        $(SRC_DIR)/kernel_compact.cu \
        $(SRC_DIR)/kernel_uf.cu \
        $(SRC_DIR)/kernel_face_encode.cu

# Object files
OBJS := $(OBJ_DIR)/main.o \
        $(OBJ_DIR)/kernel_sieve.o \
        $(OBJ_DIR)/kernel_mr.o \
        $(OBJ_DIR)/kernel_compact.o \
        $(OBJ_DIR)/kernel_uf.o \
        $(OBJ_DIR)/kernel_face_encode.o

# Device-link object
DLINK_OBJ := $(OBJ_DIR)/dlink.o

.PHONY: all clean profile

all: $(TARGET)

# Final link: device-link + host link
$(TARGET): $(OBJS) $(DLINK_OBJ)
	$(NVCC) $(BASE_FLAGS) $(LDFLAGS) -o $@ $(OBJS) $(DLINK_OBJ)

# Device linking step (required for separate compilation)
$(DLINK_OBJ): $(OBJS)
	$(NVCC) $(BASE_FLAGS) -dlink -o $@ $(OBJS)

# Per-kernel compilation with individual register caps
$(OBJ_DIR)/main.o: $(SRC_DIR)/main.cu | $(OBJ_DIR)
	$(NVCC) $(BASE_FLAGS) $(CPPFLAGS) -dc -c $< -o $@

$(OBJ_DIR)/kernel_sieve.o: $(SRC_DIR)/kernel_sieve.cu | $(OBJ_DIR)
	$(NVCC) $(BASE_FLAGS) $(K_SIEVE_FLAGS) $(CPPFLAGS) -dc -c $< -o $@

$(OBJ_DIR)/kernel_mr.o: $(SRC_DIR)/kernel_mr.cu | $(OBJ_DIR)
	$(NVCC) $(BASE_FLAGS) $(K_MR_FLAGS) $(CPPFLAGS) -dc -c $< -o $@

$(OBJ_DIR)/kernel_compact.o: $(SRC_DIR)/kernel_compact.cu | $(OBJ_DIR)
	$(NVCC) $(BASE_FLAGS) $(K_COMPACT_FLAGS) $(CPPFLAGS) -dc -c $< -o $@

$(OBJ_DIR)/kernel_uf.o: $(SRC_DIR)/kernel_uf.cu | $(OBJ_DIR)
	$(NVCC) $(BASE_FLAGS) $(K_UF_FLAGS) $(CPPFLAGS) -dc -c $< -o $@

$(OBJ_DIR)/kernel_face_encode.o: $(SRC_DIR)/kernel_face_encode.cu | $(OBJ_DIR)
	$(NVCC) $(BASE_FLAGS) $(K_FACE_FLAGS) $(CPPFLAGS) -dc -c $< -o $@

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

profile: $(TARGET)
	./$(TARGET) 1000

clean:
	rm -rf $(OBJ_DIR) $(TARGET) $(PROFILE_TARGET)
