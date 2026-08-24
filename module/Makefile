obj-m += velocitor.o
velocitor-y := src/lifecycle.o
velocitor-y += src/irq.o
velocitor-y += src/debugfs.o
velocitor-y += src/counters.o
velocitor-y += src/window.o
velocitor-y += src/identity.o
velocitor-y += src/dma.o
velocitor-y += src/remoteproc.o
velocitor-y += src/error.o
velocitor-y += src/vring.o
velocitor-y += src/device-trace.o
velocitor-y += src/ctrl.o
velocitor-y += src/ctrl-transaction.o
velocitor-y += src/handles.o
velocitor-y += src/cdev.o

ccflags-y += -I$(src)/../uapi -I$(src)/../qemu-device/ -I$(src)/src
# rproc_elf_* are exported but declared in the remoteproc core's private header.
ccflags-y += -I$(srctree)/drivers/remoteproc

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
