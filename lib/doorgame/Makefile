CC := gcc
CFLAGS := -std=c2x -O2 -Wall -Wextra -I include -I ../inih -D_DEFAULT_SOURCE -D_XOPEN_SOURCE=700
AR := ar
ARFLAGS := rcs

SRC := src/door_relay.c src/bbs_door.c src/doorgame.c
OBJ := $(SRC:src/%.c=build/%.o)

TARGET := libdoorgame.a

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJ)
	$(AR) $(ARFLAGS) $@ $^

build/%.o: src/%.c
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf build $(TARGET)
