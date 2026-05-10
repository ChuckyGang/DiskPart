# DiskPart Makefile
# AmigaOS 3.1+ GadTools hard-drive selector
# Toolchain: m68k-amiga-elf-gcc (Bartman/Abyss), -nostdlib

BARTMAN = /home/john/.vscode/extensions/bartmanabyss.amiga-debug-1.8.2/bin/linux

program  = out/DiskPart
CC       = $(BARTMAN)/opt/bin/m68k-amiga-elf-gcc
AS       = $(BARTMAN)/opt/bin/m68k-amiga-elf-as
ELF2HUNK = $(BARTMAN)/elf2hunk

SDKDIR = $(BARTMAN)/opt/m68k-amiga-elf/sys-include

CCFLAGS = -g -MP -MMD -m68000 -O2 -nostdlib \
          -Wextra -Wno-unused-function -Wno-volatile-register-var \
          -Wno-int-conversion -Wno-incompatible-pointer-types \
          -DNO_INLINE_STDARG \
          -fomit-frame-pointer -fno-tree-loop-distribution \
          -fno-exceptions -ffunction-sections -fdata-sections \
          -Isrc -I$(SDKDIR)

ASFLAGS = -mcpu=68000 -g --register-prefix-optional -I$(SDKDIR)
LDFLAGS = -Wl,--emit-relocs,--gc-sections,-Ttext=0,-Map=$(program).map

src_c   := $(wildcard src/*.c)
src_obj := $(addprefix obj/,$(patsubst src/%.c,%.o,$(src_c)))
sup_obj := obj/gcc8_c_support.o obj/gcc8_a_support.o
objects := $(src_obj) $(sup_obj)

.PHONY: all clean FORCE

all: $(program).exe

# build.o embeds __DATE__/__TIME__ via build.c — force-rebuild every
# invocation so the stamp matches the current build.
obj/build.o: FORCE
FORCE:

$(program).exe: $(program).elf
	$(info Elf2Hunk $@)
	@$(ELF2HUNK) $(program).elf $(program).exe

$(program).elf: $(objects)
	$(info Linking $@)
	@$(CC) $(CCFLAGS) $(LDFLAGS) $(objects) -o $@
	@$(BARTMAN)/opt/bin/m68k-amiga-elf-objdump --disassemble --no-show-raw-ins \
	    --visualize-jumps -S $@ >$(program).s

clean:
	$(info Cleaning...)
	@rm -f obj/* out/*

-include $(src_obj:.o=.d) obj/gcc8_c_support.d

$(src_obj) : obj/%.o : src/%.c
	$(info Compiling $<)
	@$(CC) $(CCFLAGS) -c -o $@ $<

obj/gcc8_c_support.o : support/gcc8_c_support.c
	$(info Compiling $<)
	@$(CC) $(CCFLAGS) -c -o $@ $<

obj/gcc8_a_support.o : support/gcc8_a_support.s
	$(info Assembling $<)
	@$(AS) $(ASFLAGS) --MD obj/gcc8_a_support.d -o $@ $<
