# =========================
# Compilers
# =========================
CC32 = i686-elf-gcc
CC64 = x86_64-elf-gcc
AS32 = i686-elf-as
AS64 = x86_64-elf-as

# =========================
# Flags
# =========================
CFLAGS_COMMON = -Iinclude -O2 -ffreestanding -Wall -Wextra
CFLAGS_BOOT32 = $(CFLAGS_COMMON) -mno-sse -mno-sse2 -mno-mmx -msoft-float -fno-builtin
CFLAGS_KERNEL64 = $(CFLAGS_COMMON) -mno-red-zone -mcmodel=kernel

# Separate flags for special libraries
CFLAGS_LIBFS = $(CFLAGS_COMMON) -mno-red-zone
CFLAGS_LIBK = $(CFLAGS_KERNEL64)

ASFLAGS_COMMON = -Iinclude
ASFLAGS_BOOT32 = $(ASFLAGS_COMMON)
ASFLAGS_KERNEL64 = $(ASFLAGS_COMMON)

# Separate ASM flags for special libraries
ASFLAGS_LIBFS = $(ASFLAGS_COMMON)
ASFLAGS_LIBK = $(ASFLAGS_KERNEL64)

LDFLAGS_BOOT32 = -T boot32/linker.ld -ffreestanding -nostdlib
LDFLAGS_KERNEL64 = -T kernel64/linker.ld -ffreestanding -mno-red-zone -nostdlib
LDFLAGS_BOOT32_POSTFIX = -lgcc
LDFLAGS_KERNEL64_POSTFIX = -lgcc

# Separate linker flags for special libraries
LDFLAGS_LIBFS = rcs
LDFLAGS_LIBK = rcs

# =========================
# Source discovery
# =========================

# Boot32 sources
#BOOT32_C_SRCS := $(wildcard boot32/*.c)
#BOOT32_S_SRCS := $(wildcard boot32/*.s)

BOOT32_BORROW_C_SRCS := kernel64/memory/bump.c

BOOT32_C_SRCS := $(shell find boot32 -name "*.c")
BOOT32_S_SRCS := $(shell find boot32 -name "*.s")

# Kernel64 sources
#KERNEL64_C_SRCS := $(wildcard kernel64/*.c)
#KERNEL64_S_SRCS := $(wildcard kernel64/*.s)

KERNEL64_C_SRCS := $(shell find kernel64 -name "*.c")
KERNEL64_S_SRCS := $(shell find kernel64 -name "*.s")


# =========================
# Special library sources
# =========================

# common/libfs compiled separately
LIBFS_C_SRCS := $(shell find common/libfs -name "*.c")
LIBFS_S_SRCS := $(shell find common/libfs -name "*.s")

# kernel64/libk compiled separately
LIBK_C_SRCS := $(shell find kernel64/libk -name "*.c")
LIBK_S_SRCS := $(shell find kernel64/libk -name "*.s")

# =========================
# Common sources (excluding libfs)
# =========================

COMMON_C_SRCS := $(shell find common -name "*.c" ! -path "common/libfs/*")
COMMON_S_SRCS := $(shell find common -name "*.s" ! -path "common/libfs/*")

# =========================
# Kernel64 sources (excluding libk)
# =========================

KERNEL64_CORE_C_SRCS := $(filter-out $(LIBK_C_SRCS), $(KERNEL64_C_SRCS))
KERNEL64_CORE_S_SRCS := $(filter-out $(LIBK_S_SRCS), $(KERNEL64_S_SRCS))

# =========================
# Object mapping
# =========================

BOOT32_OBJS := \
	$(patsubst kernel64/%.c, build/boot32/kernel64/%.o, $(BOOT32_BORROW_C_SRCS)) \
	$(patsubst boot32/%.c, build/boot32/%.o, $(BOOT32_C_SRCS)) \
	$(patsubst boot32/%.s, build/boot32/%.o, $(BOOT32_S_SRCS)) \
	$(patsubst common/%.c, build/boot32/common/%.o, $(COMMON_C_SRCS)) \
	$(patsubst common/%.s, build/boot32/common/%.o, $(COMMON_S_SRCS))

KERNEL64_OBJS := \
	$(patsubst kernel64/%.c, build/kernel64/%.o, $(KERNEL64_CORE_C_SRCS)) \
	$(patsubst kernel64/%.s, build/kernel64/%.o, $(KERNEL64_CORE_S_SRCS)) \
	$(patsubst common/%.c, build/kernel64/common/%.o, $(COMMON_C_SRCS)) \
	$(patsubst common/%.s, build/kernel64/common/%.o, $(COMMON_S_SRCS))

# =========================
# Separate library object mapping
# =========================

LIBFS_OBJS := \
	$(patsubst common/libfs/%.c, build/libfs/%.o, $(LIBFS_C_SRCS)) \
	$(patsubst common/libfs/%.s, build/libfs/%.o, $(LIBFS_S_SRCS))

LIBK_OBJS := \
	$(patsubst kernel64/libk/%.c, build/libk/%.o, $(LIBK_C_SRCS)) \
	$(patsubst kernel64/libk/%.s, build/libk/%.o, $(LIBK_S_SRCS))

# =========================
# Library outputs
# =========================

LIBFS_ARCHIVE = build/libfs.a
LIBK_ARCHIVE = build/libk.a

# =========================
# Targets
# =========================

all: $(LIBFS_ARCHIVE) $(LIBK_ARCHIVE) build/boot32.elf build/kernel64.elf

# =========================
# Linking
# =========================

build/boot32.elf: $(BOOT32_OBJS)
	$(CC32) $(LDFLAGS_BOOT32) $^ $(LDFLAGS_BOOT32_POSTFIX) -o $@

build/kernel64.elf: $(KERNEL64_OBJS) $(LIBFS_ARCHIVE) $(LIBK_ARCHIVE)
	$(CC64) $(LDFLAGS_KERNEL64) $(KERNEL64_OBJS) $(LIBFS_ARCHIVE) $(LIBK_ARCHIVE) \
 $(LDFLAGS_KERNEL64_POSTFIX) -o $@

# =========================
# Static library creation
# =========================

$(LIBFS_ARCHIVE): $(LIBFS_OBJS)
	@mkdir -p $(dir $@)
	ar $(LDFLAGS_LIBFS) $@ $^

$(LIBK_ARCHIVE): $(LIBK_OBJS)
	@mkdir -p $(dir $@)
	ar $(LDFLAGS_LIBK) $@ $^

# =========================
# Generic compile rules
# =========================

# C files (boot32/kernel64) (borrowed)
build/boot32/kernel64/%.o: kernel64/%.c
	@mkdir -p $(dir $@)
	$(CC32) $(CFLAGS_BOOT32) -c $< -o $@

# C files (boot32)
build/boot32/%.o: boot32/%.c
	@mkdir -p $(dir $@)
	$(CC32) $(CFLAGS_BOOT32) -c $< -o $@

# ASM files (boot32)
build/boot32/%.o: boot32/%.s
	@mkdir -p $(dir $@)
	$(AS32) $(ASFLAGS_BOOT32) -c $< -o $@

# C files (kernel64)
build/kernel64/%.o: kernel64/%.c
	@mkdir -p $(dir $@)
	$(CC64) $(CFLAGS_KERNEL64) -c $< -o $@

# ASM files (kernel64)
build/kernel64/%.o: kernel64/%.s
	@mkdir -p $(dir $@)
	$(AS64) $(ASFLAGS_KERNEL64) -c $< -o $@

# =========================
# COMMON (shared code)
# =========================

build/boot32/common/%.o: common/%.c
	@mkdir -p $(dir $@)
	$(CC32) $(CFLAGS_BOOT32) -c $< -o $@

build/kernel64/common/%.o: common/%.c
	@mkdir -p $(dir $@)
	$(CC64) $(CFLAGS_KERNEL64) -c $< -o $@

build/boot32/common/%.o: common/%.s
	@mkdir -p $(dir $@)
	$(AS32) $(ASFLAGS_BOOT32) -c $< -o $@

build/kernel64/common/%.o: common/%.s
	@mkdir -p $(dir $@)
	$(AS64) $(ASFLAGS_KERNEL64) -c $< -o $@

# =========================
# libfs special build rules
# =========================

build/libfs/%.o: common/libfs/%.c
	@mkdir -p $(dir $@)
	$(CC64) $(CFLAGS_LIBFS) -c $< -o $@

build/libfs/%.o: common/libfs/%.s
	@mkdir -p $(dir $@)
	$(AS64) $(ASFLAGS_LIBFS) -c $< -o $@

# =========================
# libk special build rules
# =========================

build/libk/%.o: kernel64/libk/%.c
	@mkdir -p $(dir $@)
	$(CC64) $(CFLAGS_LIBK) -c $< -o $@

build/libk/%.o: kernel64/libk/%.s
	@mkdir -p $(dir $@)
	$(AS64) $(ASFLAGS_LIBK) -c $< -o $@

# =========================
# Clean
# =========================

clean:
	rm -rf build/*.elf build/*.a build/boot32 build/kernel64 build/libfs build/libk MegaOS.iso

.PHONY: all clean
