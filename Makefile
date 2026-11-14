fl2000-y := \
	fl2000_drv.o \
	fl2000_registers.o \
	fl2000_interrupt.o \
	fl2000_streaming.o \
	fl2000_i2c.o \
	fl2000_drm.o

it66121-y := \
	bridge/it66121_drv.o

# Vectorized pixel conversion: the kernel builds without SIMD, so enable it for this file only and
# keep the 16-byte stack alignment SSE needs (callers only guarantee 8)
fl2000-$(CONFIG_X86_64) += fl2000_simd.o
CFLAGS_fl2000_simd.o := -msse2 -mssse3 -msse4.1 -ffreestanding \
	-mpreferred-stack-boundary=4 -mincoming-stack-boundary=3 \
	-I$(src)/simd-include -isystem $(shell $(CC) -print-file-name=include)

obj-m := fl2000.o it66121.o

KVER ?= $(shell uname -r)
KSRC ?= /lib/modules/$(KVER)/build

EXTRA_CFLAGS := -Wall

all:	modules

modules:
	make CHECK="/usr/bin/sparse" -C $(KSRC) M=$(PWD) modules

clean:
	make -C $(KSRC) M=$(PWD) clean
	rm -f $(PWD)/Module.symvers $(PWD)/*.ur-safe
