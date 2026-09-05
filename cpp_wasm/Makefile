# ========================
# Project configuration
# ========================
PROJECT      := nexrad_parser
WASM_MODULE  := module

ROOT_DIR     := $(PWD)
SRC_DIR      := cpp
TP_DIR       := third_party
BUILD_DIR    := build

NATIVE_DIR   := $(BUILD_DIR)/native
WASM_DIR     := $(BUILD_DIR)/wasm

OBJ_NATIVE   := $(NATIVE_DIR)/obj
OBJ_WASM     := $(WASM_DIR)/obj
DIST_WASM    := $(WASM_DIR)/dist

# ========================
# Toolchains
# ========================
CXX   := g++
CC    := gcc
EMCC  := emcc
AR    := ar

# ========================
# Includes
# ========================
BZ2_DIR  := $(TP_DIR)/bzip2
STB_DIR  := $(TP_DIR)/stb

INCLUDES := -I$(BZ2_DIR) -I$(STB_DIR) -I$(SRC_DIR)

# ========================
# Flags
# ========================
COMMON_CFLAGS := -O3 -Wall
CXXFLAGS      := $(COMMON_CFLAGS) -std=c++17 $(INCLUDES)
CFLAGS        := $(COMMON_CFLAGS) $(INCLUDES)

EMCC_FLAGS := \
  $(COMMON_CFLAGS) \
  -pthread \
  -msimd128 \
  -fdata-sections \
  -ffunction-sections \
  $(INCLUDES)

# ========================
# Sources
# ========================
CPP_SOURCES := $(wildcard $(SRC_DIR)/*.cpp)

BZ2_SOURCES := \
  bzlib.c \
  decompress.c \
  crctable.c \
  huffman.c \
  randtable.c

BZ2_OBJS_NATIVE := $(addprefix $(OBJ_NATIVE)/, $(BZ2_SOURCES:.c=.o))
BZ2_OBJS_WASM   := $(addprefix $(OBJ_WASM)/,   $(BZ2_SOURCES:.c=.o))

CPP_OBJS_NATIVE := $(CPP_SOURCES:$(SRC_DIR)/%.cpp=$(OBJ_NATIVE)/%.o)
CPP_OBJS_WASM   := $(CPP_SOURCES:$(SRC_DIR)/%.cpp=$(OBJ_WASM)/%.o)

# ========================
# WASM Exports (MINIMAL)
# ========================
WASM_EXPORTS := \
  _parse_nexrad \
  _get_png_data \
  _get_png_size \
  _malloc \
  _free

#
# -pthread \
# -s USE_PTHREADS=1 \
# -s PTHREAD_POOL_SIZE=8 \
# -s MODULARIZE=1 \
# -s EXPORT_NAME=ModuleFactory \
# -s ENVIRONMENT=web,worker \
# -s ALLOW_MEMORY_GROWTH=1


EMCC_LDFLAGS := \
  -O3 \
  --bind \
  -pthread \
  -s USE_PTHREADS=1 \
  -s PTHREAD_POOL_SIZE=8 \
  -s WASM=1 \
  -s ENVIRONMENT=web,worker \
  -s MODULARIZE=1 \
  -s EXPORT_ES6=1 \
  -s EXPORT_NAME=$(WASM_MODULE) \
  -s EXPORTED_FUNCTIONS="['_parse_nexrad', \
                          '_get_png_data', \
                          '_get_png_data_size', \
                          '_get_tilt_count', \
                          '_get_tilt_angle', \
                          '_get_tilt_name', \
                          '_set_tilt_index', \
                          '_get_number_of_radar_moments', \
                          '_get_radar_moment_name', \
                          '_set_selected_radar_moment', \
                          '_get_moment_data', \
                          '_get_moment_data_vertices_then_val_size', \
                          '_get_packed_radar_data', \
                          '_get_packed_radar_data_size', \
                          '_get_tilt_angles', \
                          '_get_tilt_angles_size', \
                          '_malloc', \
                          '_free']" \
  -s EXPORTED_RUNTIME_METHODS="['ccall','cwrap','HEAPU8','lengthBytesUTF8','stringToUTF8','HEAPF32']" \
  -s INITIAL_MEMORY=128MB \
  -s MAXIMUM_MEMORY=2GB \
  -s ALLOW_MEMORY_GROWTH=1

    #-s EXPORTED_FUNCTIONS="$(foreach f,$(WASM_EXPORTS),'$(f)')" \

# ========================
# Targets
# ========================
.PHONY: all native wasm clean copy-wasm

all: native wasm

# ------------------------
# Native build (static lib OK)
# ------------------------
native: $(NATIVE_DIR)/bin/$(PROJECT)

$(NATIVE_DIR)/bin/$(PROJECT): $(CPP_OBJS_NATIVE) $(OBJ_NATIVE)/libbz2.a
	@mkdir -p $(dir $@)
	$(CXX) $^ -o $@

# ------------------------
# WASM build (NO ARCHIVE)
# ------------------------
wasm: $(DIST_WASM)/$(WASM_MODULE).js

$(DIST_WASM)/$(WASM_MODULE).js: $(CPP_OBJS_WASM) $(BZ2_OBJS_WASM)
	@mkdir -p $(DIST_WASM)
	$(EMCC) $^ $(EMCC_LDFLAGS) -o $@

# ------------------------
# bzip2 static library (native only)
# ------------------------
$(OBJ_NATIVE)/libbz2.a: $(BZ2_OBJS_NATIVE)
	@mkdir -p $(OBJ_NATIVE)
	$(AR) rcs $@ $^

# ------------------------
# Compile rules
# ------------------------
$(OBJ_NATIVE)/%.o: $(SRC_DIR)/%.cpp
	@mkdir -p $(OBJ_NATIVE)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OBJ_WASM)/%.o: $(SRC_DIR)/%.cpp
	@mkdir -p $(OBJ_WASM)
	$(EMCC) $(EMCC_FLAGS) -c $< -o $@

$(OBJ_NATIVE)/%.o: $(BZ2_DIR)/%.c
	@mkdir -p $(OBJ_NATIVE)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_WASM)/%.o: $(BZ2_DIR)/%.c
	@mkdir -p $(OBJ_WASM)
	$(EMCC) $(EMCC_FLAGS) -c $< -o $@

# ------------------------
# Cleanup
# ------------------------
clean:
	rm -rf build

# ------------------------
# Frontend copy
# ------------------------
WEB_WASM_DIR := ../lizard_radar_frontend/public/wasm

copy-wasm: wasm
	mkdir -p $(WEB_WASM_DIR)
	cp $(DIST_WASM)/$(WASM_MODULE).* $(WEB_WASM_DIR)