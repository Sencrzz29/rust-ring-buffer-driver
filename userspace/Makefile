# Cross-compile the user-space tools as static ARM64 binaries (the board's
# initramfs has no shared libraries). Output: userspace/build/, which
# qemu/mkinitramfs.sh copies to /tests on the board.

CROSS_COMPILE ?= aarch64-linux-gnu-
CC      := $(CROSS_COMPILE)gcc
CFLAGS  := -O2 -Wall -Wextra -Werror -static -I../include
LDLIBS  := -lpthread

BUILD   := build
PROGS   := ringctl ringtest ringfuzz ringbench

all: $(addprefix $(BUILD)/,$(PROGS))

$(BUILD)/%: %.c ../include/uapi/ringbuf.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

$(BUILD):
	mkdir -p $@

clean:
	rm -rf $(BUILD)

.PHONY: all clean
