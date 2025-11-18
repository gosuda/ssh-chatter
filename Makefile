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
CFLAGS = -std=c2x -Ofast \
              -Werror \
              -Wno-error=deprecated-declarations -DSSH_CHATTER_USE_GC=$(ENABLE_GC) \
              -I lib/headers -I/usr/include -I/usr/include/libssh \
              -D_DEFAULT_SOURCE -D_XOPEN_SOURCE=700 \
              -Wall -Wextra -Wshadow -Wformat=2 -Wundef -Wconversion -Wdouble-promotion \
              -fno-omit-frame-pointer -fstack-protector-strong -fno-common \
              -fPIC \
              -g \
              -D_FORTIFY_SOURCE=2 \
              -march=native -mtune=native \
              -fwhole-program \
              -flto=$(NPROC) -fuse-linker-plugin \
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
    -lpthread -ldl -lcurl -lm -lcrypto -lc \
    -flto=$(NPROC) -fuse-linker-plugin \
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
    -Wl,-z,stack-size=786432

LDFLAGS = $(COMMON_LDFLAGS) -lssh

# Define targets and source files
TARGET := ssh-chatter
SHARED_TARGET := libssh_chatter_backend.so
SRC := main.c lib/host.c lib/client.c lib/webssh_client.c lib/translator.c \
       lib/translation_helpers.c lib/ssh_chatter_backend.c lib/user_data.c \
       lib/matrix_client.c lib/irc_client.c lib/fidonet_client.c lib/security_layer.c lib/memory_manager.c \
       lib/ssh_chatter_sync.c
OBJ := $(SRC:.c=.o)
SHARED_SRC := lib/translator.c lib/translation_helpers.c lib/ssh_chatter_backend.c lib/memory_manager.c
SHARED_OBJ := $(SHARED_SRC:.c=.o)
DEP := $(OBJ:.o=.d)

STRESS_TARGET := stress-test
STRESS_SRC := tests/stress_main.c
STRESS_OBJ := $(STRESS_SRC:.c=.o)


.PHONY: all clean run stress-test

# ==============================================================================
# BUILD RULES (Single Stage)
# ==============================================================================

# Default goal: Build the executable and shared library
all: $(TARGET) $(SHARED_TARGET)

# Final linking for the executable
$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Final linking for the shared library
$(SHARED_TARGET): $(SHARED_OBJ)
	$(CC) $(CFLAGS) -shared -o $@ $^ $(COMMON_LDFLAGS)

$(STRESS_TARGET): $(filter-out main.o,$(OBJ)) $(STRESS_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Rule for compiling object files
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

run: $(TARGET)
	./$(TARGET)

clean:
# Cleanup only for LTO/standard build files
	rm -f $(OBJ) $(TARGET) $(SHARED_TARGET) $(DEP) $(STRESS_OBJ) $(STRESS_TARGET)

# Include dependency files
-include $(DEP)

# Add Garbage Collector library if enabled
ifeq ($(ENABLE_GC),1)
COMMON_LDFLAGS += -lgc
endif
