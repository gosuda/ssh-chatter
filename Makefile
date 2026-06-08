# Default setting: Disable Garbage Collector (GC)
ENABLE_GC ?= 0
# LTO currently triggers a TLS type mismatch inside libttak when linking ssh-chatter.
# Keep it opt-in so the default build remains stable.
ENABLE_LTO ?= 1

# The C compiler to use
CC := gcc

# Get the number of available CPU cores for parallel LTO processing
NPROC := $(shell nproc)
LTO_NPROC := $(shell nproc >/dev/null 2>&1 && nproc || echo 1)
LTO_JOBS := $(shell jobs=$$(( $(LTO_NPROC) / 4 )); if [ $$jobs -lt 1 ]; then jobs=1; fi; echo $$jobs)

# ==============================================================================
# CFLAGS: EXTREME Low-Latency and LTO Optimization Flags
# DANGER: Contains highly aggressive, potentially unsafe, and experimental flags.
# ==============================================================================
SRC_DIR := src
INCLUDE_DIR := include
BUILD_DIR := build
TTAK_DIR := lib/libttak
TTAK_LIB := /usr/local/lib/libttak.a

# NOTE: -march=native builds binaries tuned for the current host CPU and must
#       be rebuilt on any deployment target with a different microarchitecture.
#       For portable/container deployments use -march=x86-64 -mtune=generic.

# Optional third-party libraries for charset conversion
HAVE_UCHARDET := $(shell pkg-config --exists uchardet && echo 1 || echo 0)
HAVE_ICU := $(shell pkg-config --exists icu-uc && echo 1 || echo 0)

ifeq ($(HAVE_UCHARDET),1)
CFLAGS_UCHARDET := $(shell pkg-config --cflags uchardet)
LDFLAGS_UCHARDET := $(shell pkg-config --libs uchardet)
endif

ifeq ($(HAVE_ICU),1)
CFLAGS_ICU := $(shell pkg-config --cflags icu-uc)
LDFLAGS_ICU := $(shell pkg-config --libs icu-uc)
endif

CFLAGS = -std=c2x -Ofast \
              -Werror \
              -Wno-error=deprecated-declarations -DSSH_CHATTER_USE_GC=$(ENABLE_GC) \
              $(if $(filter 1,$(HAVE_UCHARDET)),-DSSH_CHATTER_HAVE_UCHARDET $(CFLAGS_UCHARDET)) \
              $(if $(filter 1,$(HAVE_ICU)),-DSSH_CHATTER_HAVE_ICU $(CFLAGS_ICU)) \
              -I $(INCLUDE_DIR) -I /usr/local/include -I/usr/include -I/usr/include/libssh -I/usr/include/x86_64-linux-gnu \
              -D_DEFAULT_SOURCE -D_XOPEN_SOURCE=700 \
              -Wall -Wextra -Wshadow -Wformat=2 -Wundef -Wconversion -Wdouble-promotion \
              -fstack-protector-strong -fno-common \
              -fPIC -ftls-model=global-dynamic \
              -g \
              -D_FORTIFY_SOURCE=3 \
              -march=native -mtune=native \
              -fomit-frame-pointer \
              -fno-signed-zeros \
              -funroll-loops \
              -falign-functions=32 -falign-loops=32 -falign-jumps=32 -falign-labels=32 \
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
              -fmodulo-sched -fmodulo-sched-allow-regmoves \
              -ftracer \
              -fvisibility=hidden \
              -fno-plt \
              -funsafe-math-optimizations \
              -ftree-loop-vectorize -ftree-slp-vectorize \
              -fno-exceptions \
              -fdelete-null-pointer-checks \
              -MMD -MP

# ==============================================================================
# LINKER FLAGS (LDFLAGS) - Extreme Security and Optimization
# ==============================================================================
COMMON_LDFLAGS = \
    -L/usr/local/lib \
    -lpthread -ldl -lcurl -lm -lcrypto -llz4 -lttak -lutil -lc \
    $(LDFLAGS_UCHARDET) $(LDFLAGS_ICU) \
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
    -Wl,-z,stack-size=524288 \
    -Wl,-z,combreloc \
    -Wl,--build-id=none

LDFLAGS = $(COMMON_LDFLAGS) -lssh

ifeq ($(ENABLE_LTO),1)
CFLAGS += -flto=$(LTO_JOBS) -fuse-linker-plugin
COMMON_LDFLAGS += -flto=$(LTO_JOBS) -fuse-linker-plugin
endif

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

DISPLAY_TEST_TARGET := display-model-test
DISPLAY_TEST_SRC := tests/display_model_test.c src/display_model.c
DISPLAY_TEST_OBJ := $(patsubst %.c,$(BUILD_DIR)/%.o,$(DISPLAY_TEST_SRC))

.PHONY: all clean run stress-test display-model-test

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

$(DISPLAY_TEST_TARGET): $(DISPLAY_TEST_OBJ) $(TTAK_LIB)
	$(CC) $(CFLAGS) -o $@ $^ -Wno-error $(LDFLAGS)

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
	rm -rf $(BUILD_DIR) $(TARGET) $(SHARED_TARGET) $(DEP) $(STRESS_OBJ) $(STRESS_TARGET) $(DISPLAY_TEST_TARGET)

# Include dependency files
-include $(DEP)

# Add Garbage Collector library if enabled
ifeq ($(ENABLE_GC),1)
COMMON_LDFLAGS += -lgc
endif
