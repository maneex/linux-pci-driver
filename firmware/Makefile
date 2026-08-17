# firmware/Makefile -- build the remoteproc firmware image of spec section 6.3.
#
# The image is generated, not compiled from source: see mkfw.c for why.  It is
# a build artefact and is not committed; devtools/build-firmware.sh is the
# usual way in, and the guest reaches the result through the 9p share.

CC       ?= cc
CFLAGS   ?= -O2 -Wall -Wextra -Wno-unused-parameter
CPPFLAGS += -I../qemu-device

FIRMWARE := velocitor-fw.elf

all: $(FIRMWARE)

mkfw: mkfw.c ../qemu-device/velocitor_hw.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ mkfw.c

$(FIRMWARE): mkfw
	./mkfw $@

# Reads back what was just produced, with the host's own ELF tools rather
# than with the generator that wrote it -- a check that agrees with itself
# proves nothing.
check: $(FIRMWARE)
	readelf -hSl $(FIRMWARE)

clean:
	$(RM) mkfw $(FIRMWARE)

.PHONY: all check clean
