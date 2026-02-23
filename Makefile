# Default setting: Disable Garbage Collector (GC)
ENABLE_GC ?= 0

# The C compiler to use
CC := gcc

# Get the number of available CPU cores for parallel LTO processing
NPROC := $(shell nproc)

# ==============================================================================
# CFLAGS: EXTREME Low-Latency and LTO Optimization Flags
# DANGER: Contains highly aggressive, potentially unsafe, and experimental flags.
# ==============================================================================
SRC_DIR := src
INCLUDE_DIR := include
BUILD_DIR := build
TTAK_DIR := lib/libttak
TTAK_LIB := $(TTAK_DIR)/lib/libttak.a

CFLAGS = -std=c2x -Ofast \
              -Werror \
              -Wno-error=deprecated-declarations -DSSH_CHATTER_USE_GC=$(ENABLE_GC) \
              -I $(INCLUDE_DIR) -I $(TTAK_DIR)/include -I/usr/include -I/usr/include/libssh -I/usr/include/x86_64-linux-gnu \
              -D_DEFAULT_SOURCE -D_XOPEN_SOURCE=700 \
              -Wall -Wextra -Wshadow -Wformat=2 -Wundef -Wconversion -Wdouble-promotion \
              -fno-omit-frame-pointer -fstack-protector-strong -fno-common \
              -fPIC \
              -g \
              -D_FORTIFY_SOURCE=2 \
              -flto=auto -fuse-linker-plugin \
              -fomit-frame-pointer \
              -fno-signed-zeros \
              -funroll-loops \
              -falign-functions=32 -falign-loops=32 \
              -ftree-vectorize \
              -fno-math-errno -freciprocal-math \
              -fmerge-all-constants -fipa-pta -fdevirtualize-at-ltrans \
              -fpeel-loops -fweb \
              -fdata-sections -ffunction-sections \
              -fno-asynchronous-unwind-tables \
              -fstrict-aliasing -fno-trapping-math -fstrict-overflow \
              -fno-builtin-malloc -fno-builtin-calloc -fno-builtin-realloc -fno-builtin-free \
              -fipa-pure-const -fipa-cp-clone \
              -floop-nest-optimize -fgraphite-identity -floop-interchange -floop-strip-mine -floop-block \
              -fno-semantic-interposition \
              -fprefetch-loop-arrays \
              -fivopts \
              -faggressive-loop-optimizations \
              -fipa-sra \
              \
              -funsafe-math-optimizations \
              -ftree-loop-vectorize -ftree-slp-vectorize \
              -fno-exceptions \
              -fdelete-null-pointer-checks \
              -MMD -MP

# ==============================================================================
# LINKER FLAGS (LDFLAGS) - Extreme Security and Optimization
# ==============================================================================
COMMON_LDFLAGS = \
    -lpthread -ldl -lcurl -lm -lcrypto -lttak -lc \
    -flto=auto -fuse-linker-plugin \
    -fwhole-program \
    -Wl,-Ofast \
    -Wl,--hash-style=gnu \
    -Wl,--sort-common \
    -Wl,-z,relro \
    -Wl,-z,now \
    -Wl,-Bsymbolic \
    -Wl,--gc-sections \
    -Wl,--as-needed \
    -Wl,--strip-all \
    -Wl,--relax \
    -Wl,--no-undefined \
    -Wl,--warn-execstack -Wl,-z,noexecstack \
    -Wl,-z,separate-code \
    -Wl,-z,stack-size=4194304

LDFLAGS = $(COMMON_LDFLAGS) -lssh

# Define targets and source files
TARGET := ssh-chatter
SHARED_TARGET := libssh_chatter_backend.so
SRC := $(filter-out $(EXCLUDED_SRC),\
       $(wildcard $(SRC_DIR)/*.c) $(wildcard $(SRC_DIR)/stubs/*.c) $(wildcard $(SRC_DIR)/utils/*.c))
OBJ := $(patsubst %.c,$(BUILD_DIR)/%.o,$(SRC))
SHARED_SRC := src/translator.c src/translation_helpers.c src/ssh_chatter_backend.c src/memory_manager.c
SHARED_OBJ := $(patsubst %.c,$(BUILD_DIR)/%.o,$(SHARED_SRC))
DEP := $(OBJ:.o=.d) $(SHARED_OBJ:.o=.d)

STRESS_TARGET := stress-test
STRESS_SRC := tests/stress_main.c
STRESS_OBJ := $(patsubst %.c,$(BUILD_DIR)/%.o,$(STRESS_SRC))

.PHONY: all clean run stress-test

# ==============================================================================
# BUILD RULES (Single Stage)
# ==============================================================================

# Default goal: Build the executable and shared library
all: $(TARGET) $(SHARED_TARGET)

# Final linking for the executable
$(TARGET): $(OBJ) $(TTAK_LIB)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Final linking for the shared library
$(SHARED_TARGET): $(SHARED_OBJ) $(TTAK_LIB)
	$(CC) $(CFLAGS) -shared -o $@ $^ $(COMMON_LDFLAGS)

$(STRESS_TARGET): $(filter-out $(BUILD_DIR)/src/main.o,$(OBJ)) $(STRESS_OBJ) $(TTAK_LIB)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(TTAK_LIB):
	$(MAKE) -C $(TTAK_DIR) all

# Rule for compiling object files
$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

run: $(TARGET)
	./$(TARGET)

clean:
# Cleanup only for LTO/standard build files
	rm -rf $(BUILD_DIR) $(TARGET) $(SHARED_TARGET) $(DEP) $(STRESS_OBJ) $(STRESS_TARGET)

# Include dependency files
-include $(DEP)

# Add Garbage Collector library if enabled
ifeq ($(ENABLE_GC),1)
COMMON_LDFLAGS += -lgc
endif
