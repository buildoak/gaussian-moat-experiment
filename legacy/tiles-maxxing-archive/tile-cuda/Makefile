NVCC ?= /usr/local/cuda/bin/nvcc
CXX ?= g++

# K_SQ parameterization: `make K_SQ=36` or just `make` for default K_SQ=40
K_SQ ?= 40

TARGET := tile_kernel
PROFILE_TARGET := tile_kernel_profile
SRC_DIR := src
OBJ_DIR := build-k$(K_SQ)
INCLUDE_DIR := include
TEST_DIR := test

# Paths to tile-cpp source
CPP_DIR := ../tile-cpp
CPP_SRC := $(CPP_DIR)/src
CPP_INC := $(CPP_DIR)/include

NVCCFLAGS := -arch=sm_87 -O3 -std=c++17 --expt-relaxed-constexpr -lineinfo
NVCCFLAGS_PROFILE := $(NVCCFLAGS) -DPROFILE_PHASES
CPPFLAGS := -I$(INCLUDE_DIR) -DK_SQ_VAL=$(K_SQ)
CXXFLAGS := -O3 -std=c++17 -Wall -I$(CPP_INC) -I$(TEST_DIR) -DK_SQ_VAL=$(K_SQ)
LDFLAGS :=

SRCS := $(SRC_DIR)/main.cu $(SRC_DIR)/tile_kernel.cu
OBJS := $(patsubst $(SRC_DIR)/%.cu,$(OBJ_DIR)/%.o,$(SRCS))
PROFILE_OBJS := $(patsubst $(SRC_DIR)/%.cu,$(OBJ_DIR)/%_profile.o,$(SRCS))

# C++ reference sources for cross-validation
CPP_TILE_SRCS := $(CPP_SRC)/sieve.cpp $(CPP_SRC)/compact.cpp $(CPP_SRC)/union_find.cpp \
                 $(CPP_SRC)/face_extract.cpp $(CPP_SRC)/prune.cpp $(CPP_SRC)/encode.cpp \
                 $(CPP_SRC)/process_tile.cpp
CPP_TILE_OBJS := $(patsubst $(CPP_SRC)/%.cpp,$(OBJ_DIR)/cpp_%.o,$(CPP_TILE_SRCS))

.PHONY: all clean test profile

all: $(TARGET)

$(TARGET): $(OBJS)
	$(NVCC) $(NVCCFLAGS) $(LDFLAGS) -o $@ $(OBJS)

$(PROFILE_TARGET): $(PROFILE_OBJS)
	$(NVCC) $(NVCCFLAGS_PROFILE) $(LDFLAGS) -o $@ $(PROFILE_OBJS)

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cu | $(OBJ_DIR)
	$(NVCC) $(NVCCFLAGS) $(CPPFLAGS) -dc -c $< -o $@

$(OBJ_DIR)/%_profile.o: $(SRC_DIR)/%.cu | $(OBJ_DIR)
	$(NVCC) $(NVCCFLAGS_PROFILE) $(CPPFLAGS) -dc -c $< -o $@

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

# --- Cross-validation test targets ---

# Compile tile-cpp sources with g++
$(OBJ_DIR)/cpp_%.o: $(CPP_SRC)/%.cpp | $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# cpp_dump: C++ reference pipeline dump
$(OBJ_DIR)/cpp_dump.o: $(TEST_DIR)/cpp_dump.cpp | $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

cpp_dump: $(OBJ_DIR)/cpp_dump.o $(CPP_TILE_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^

# cuda_dump: CUDA kernel dump
$(OBJ_DIR)/cuda_dump.o: $(TEST_DIR)/cuda_dump.cu | $(OBJ_DIR)
	$(NVCC) $(NVCCFLAGS) $(CPPFLAGS) -I$(TEST_DIR) -dc -c $< -o $@

cuda_dump: $(OBJ_DIR)/cuda_dump.o $(OBJ_DIR)/tile_kernel.o
	$(NVCC) $(NVCCFLAGS) $(LDFLAGS) -o $@ $^

# cross_compare: comparison tool
$(OBJ_DIR)/cross_compare.o: $(TEST_DIR)/cross_compare.cpp | $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

cross_compare: $(OBJ_DIR)/cross_compare.o
	$(CXX) $(CXXFLAGS) -o $@ $^

# Build all test binaries
test: cpp_dump cuda_dump cross_compare

profile: $(PROFILE_TARGET)
	./$(PROFILE_TARGET) 1000

clean:
	rm -rf $(OBJ_DIR) $(TARGET) $(PROFILE_TARGET) cpp_dump cuda_dump cross_compare \
	       cpp_tileops.bin cuda_tileops.bin
