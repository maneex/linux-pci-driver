obj-m += velocitor.o
velocitor-y := src/lifecycle.o
velocitor-y += src/irq.o
velocitor-y += src/debugfs.o
velocitor-y += src/counters.o
velocitor-y += src/window.o
velocitor-y += src/identity.o
velocitor-y += src/dma.o

ccflags-y += -I$(src)/include -I$(src)/../qemu-device/ -I$(src)/src

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(CURDIR)
CLANG_FORMAT ?= $(shell command -v clang-format-20 2>/dev/null || command -v clang-format 2>/dev/null)

ifeq ($(CONFIG_STATUS_CHECK_GCC),y)
CC=$(STATUS_CHECK_GCC)
ccflags-y += -fanalyzer
endif

all:
	$(MAKE) -C $(KDIR) CC=$(CC) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) CC=$(CC) M=$(PWD) clean
	$(RM) *.plist

# The shared header lives in qemu-device/ and follows QEMU's style, not the
# kernel's, so it is deliberately left out.
indent:
	@if [ -z "$(CLANG_FORMAT)" ]; then \
		echo "No clang-format found. Install clang-format-20 or clang-format." >&2; \
		exit 1; \
	fi
	"$(CLANG_FORMAT)" -i src/*.[ch] include/*.[ch]
