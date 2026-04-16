# DiskPart Makefile
# AmigaOS 3.1+ GadTools hard-drive selector
#
# Toolchains supported (auto-detected, override with TOOLCHAIN=...):
#   bartman - m68k-amiga-elf-gcc (Bartman/Abyss VSCode extension), ELF + elf2hunk
#   bebbo   - m68k-amigaos-gcc   (Bebbo's amiga-gcc), native hunk output
#
# Override paths or selection from the command line, e.g.:
#   make TOOLCHAIN=bebbo
#   make TOOLCHAIN=bebbo AMIGA=/opt/amiga
#   make TOOLCHAIN=bartman BARTMAN=/path/to/bartman/bin/linux

# --- Toolchain root paths (overridable) --------------------------------------

BARTMAN ?= /home/john/.vscode/extensions/bartmanabyss.amiga-debug-1.7.9/bin/linux
AMIGA   ?= /opt/amiga

# Auto-detect TOOLCHAIN if not specified: prefer bebbo when /opt/amiga is
# present, otherwise fall back to bartman.
ifeq ($(origin TOOLCHAIN),undefined)
  ifneq ($(wildcard $(AMIGA)/bin/m68k-amigaos-gcc),)
    TOOLCHAIN := bebbo
  else
    TOOLCHAIN := bartman
  endif
endif

program = out/DiskPart

# --- Per-toolchain configuration ---------------------------------------------

ifeq ($(TOOLCHAIN),bartman)
  CC       := $(BARTMAN)/opt/bin/m68k-amiga-elf-gcc
  AS       := $(BARTMAN)/opt/bin/m68k-amiga-elf-as
  OBJDUMP  := $(BARTMAN)/opt/bin/m68k-amiga-elf-objdump
  ELF2HUNK := $(BARTMAN)/elf2hunk
  SDKDIR   := $(BARTMAN)/opt/m68k-amiga-elf/sys-include

  TC_CCFLAGS     := -nostdlib
  TC_LDFLAGS     := -Wl,--emit-relocs,--gc-sections,-Ttext=0,-Map=$(program).map
  TC_SUPPORT_OBJ := obj/gcc8_c_support.o obj/gcc8_a_support.o
  NEED_ELF2HUNK  := 1
else ifeq ($(TOOLCHAIN),bebbo)
  CC       := $(AMIGA)/bin/m68k-amigaos-gcc
  AS       := $(AMIGA)/bin/m68k-amigaos-as
  OBJDUMP  := $(AMIGA)/bin/m68k-amigaos-objdump
  STRIP    := $(AMIGA)/bin/m68k-amigaos-strip
  SDKDIR   := $(AMIGA)/m68k-amigaos/ndk-include

  TC_CCFLAGS     := -noixemul
  TC_LDFLAGS     := -noixemul -Wl,--gc-sections,-Map=$(program).map
  TC_SUPPORT_OBJ :=
  NEED_ELF2HUNK  := 0
else
  $(error Unknown TOOLCHAIN '$(TOOLCHAIN)' - use bartman or bebbo)
endif

# --- Common flags ------------------------------------------------------------

CCFLAGS = -g -MP -MMD -m68000 -O2 $(TC_CCFLAGS) \
          -Wextra -Wno-unused-function -Wno-volatile-register-var \
          -Wno-int-conversion -Wno-incompatible-pointer-types \
          -DNO_INLINE_STDARG \
          -fomit-frame-pointer -fno-tree-loop-distribution \
          -fno-exceptions -ffunction-sections -fdata-sections \
          -Isrc -I$(SDKDIR)

ASFLAGS = -mcpu=68000 -g --register-prefix-optional -I$(SDKDIR)
LDFLAGS = $(TC_LDFLAGS)

$(shell mkdir -p obj out)

src_c   := $(wildcard src/*.c)
src_obj := $(addprefix obj/,$(patsubst src/%.c,%.o,$(src_c)))
objects := $(src_obj) $(TC_SUPPORT_OBJ)

.PHONY: all clean icon

all: $(program)

icon: $(program).info

$(program).info: support/hdicon.png support/make_icon.py
	$(info Generating $@)
	@python3 support/make_icon.py $@

ifeq ($(NEED_ELF2HUNK),1)

$(program): $(program).elf
	$(info Elf2Hunk $@)
	@$(ELF2HUNK) $(program).elf $(program)

$(program).elf: $(objects)
	$(info Linking $@)
	@$(CC) $(CCFLAGS) $(LDFLAGS) $(objects) -o $@
	@$(OBJDUMP) --disassemble --no-show-raw-ins \
	    --visualize-jumps -S $@ >$(program).s

else

$(program): $(objects)
	$(info Linking $@)
	@$(CC) $(CCFLAGS) $(LDFLAGS) $(objects) -o $@
	@$(OBJDUMP) --disassemble --no-show-raw-ins \
	    --visualize-jumps -S $@ >$(program).s
	$(info Stripping $@)
	@$(STRIP) $@

endif

clean:
	$(info Cleaning...)
	@rm -f obj/* out/*

-include $(src_obj:.o=.d)
ifeq ($(TOOLCHAIN),bartman)
-include obj/gcc8_c_support.d obj/gcc8_a_support.d
endif

$(src_obj) : obj/%.o : src/%.c
	$(info Compiling $<)
	@$(CC) $(CCFLAGS) -c -o $@ $<

obj/gcc8_c_support.o : support/gcc8_c_support.c
	$(info Compiling $<)
	@$(CC) $(CCFLAGS) -c -o $@ $<

obj/gcc8_a_support.o : support/gcc8_a_support.s
	$(info Assembling $<)
	@$(AS) $(ASFLAGS) --MD obj/gcc8_a_support.d -o $@ $<
