# runtime/Makefile -- libvelocitor and the tools that talk to /dev/velocitor.
#
# Two artefacts from one set of sources, because they answer two different
# needs.  libvelocitor.so is the deliverable of spec section 10.3: a C core,
# so that the measurements section 12 asks for stay scriptable from anything
# that can bind a C ABI, which a C++ one would not allow.  libvelocitor.a is
# what the guest links: its initramfs has no dynamic loader at all -- busybox
# itself is statically linked -- so anything meant to run in the VM has to be
# static or it will not start, with an unhelpful message.
#
# No dependency beyond libc, as section 10.3 requires.  Enumeration walks
# /sys/class/velocitor itself rather than linking libudev: udev is absent from
# the guest, so a library that needed it would be untestable in the only place
# the device exists.
#
# Built by devtools/build-runtime.sh; the artefacts are not committed.

# -Werror on purpose: the module is built with it (kbuild adds it), and a
# warning nobody trips over is a warning nobody reads. CFLAGS stays a ?=
# assignment, so overriding it from the command line remains the escape hatch.
CC       ?= cc
CFLAGS   ?= -O2 -g -Wall -Wextra -Werror -Wshadow -Wno-unused-parameter
CFLAGS   += -std=gnu11 -fvisibility=hidden
CPPFLAGS += -I. -I../uapi -I../qemu-device

# -I../uapi is the frozen kernel/userland contract of section 10.2, and
# -I../qemu-device the device constants of section 2 -- VEL_CAP_*, VEL_NODES
# and friends.  Both are read as-is: the whole point of a shared header is
# that no side retypes the other's values.

SRCS     := src/scan.c src/open.c src/alloc.c src/stats.c
OBJS     := $(SRCS:.c=.o)
PICOBJS  := $(SRCS:.c=.lo)

LIBS     := libvelocitor.a libvelocitor.so
TOOLS    := scan-test

all: $(LIBS) $(TOOLS)

%.o: %.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

%.lo: %.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -fPIC -c -o $@ $<

libvelocitor.a: $(OBJS)
	$(AR) rcs $@ $^

libvelocitor.so: $(PICOBJS)
	$(CC) -shared -o $@ $^

# The guest test links the archive: static, because the initramfs has no
# dynamic loader.  It reaches the VM through the 9p share, so building it is
# enough -- there is no initramfs to rebuild.
scan-test: scan-test.c libvelocitor.a
	$(CC) $(CFLAGS) $(CPPFLAGS) -static -o $@ scan-test.c libvelocitor.a

clean:
	$(RM) $(OBJS) $(PICOBJS) $(LIBS) $(TOOLS)

.PHONY: all clean
