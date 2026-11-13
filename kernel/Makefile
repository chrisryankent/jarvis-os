# =============================================================================
# JARVIS AI OS — kernel/ build
#
# Builds the kernel-only sources into a relocatable object (kernel_core.o).
# This is intended to be invoked from the repository root or directly from
# the kernel/ directory.
# =============================================================================

CC      := gcc
AS      := as
LD      := ld

CFLAGS  := -std=c17 -ffreestanding -fno-stack-protector -fno-pic \
           -mno-red-zone -mcmodel=kernel -m64 -O2 -Wall -Wextra \
           -Wno-unused-parameter -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 \
           -MMD -MP

ASFLAGS := --64

C_SRCS  := kernel.c ipc_sched_pmm.c libc_stub.c model_manifest.c \
           platform.c syscall.c vfs.c font8x16.c vmm.c governor.c usermode.c block.c diskfs.c devmgr.c net.c fatfs.c pkg.c ai_tools.c accounts.c
S_SRCS  := context_switch.S interrupts.S

C_OBJS  := $(C_SRCS:.c=.o)
S_OBJS  := $(S_SRCS:.S=.o)
OBJS    := $(C_OBJS) $(S_OBJS)
DEPS    := $(C_SRCS:.c=.d)

.PHONY: all clean check

all: kernel_core.o

kernel_core.o: $(OBJS)
	$(LD) -r $(OBJS) -o $@
	@echo ""
	@echo "Built $@. This is a relocatable object, not a bootable kernel —"
	@echo "link it against boot/, drivers/, ui/, audio/, etc. from the rest"
	@echo "of the project, using linker.ld, to produce kernel.elf."

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.S
	$(AS) $(ASFLAGS) $< -o $@

check: kernel_core.o
	nm -u kernel_core.o | sort

clean:
	rm -f $(OBJS) $(DEPS) kernel_core.o

-include $(DEPS)
