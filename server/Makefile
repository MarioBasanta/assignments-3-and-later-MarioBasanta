CROSS_COMPILE ?=
CC := $(CROSS_COMPILE)gcc

OBJ := aesdsocket

# Add cross-compiler path if needed:
PATH := $(PATH):$(PWD)/../compiler/arm-gnu-toolchain-13.3.rel1-x86_64-aarch64-none-linux-gnu/bin

CFLAGS := -g -Wall -Werror -pedantic -std=c99

all: $(OBJ)

default: all

$(OBJ): $(OBJ).c
	$(CC) $(CFLAGS) $< -o $@

clean:
	rm -f $(OBJ)
